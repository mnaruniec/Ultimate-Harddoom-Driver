#include "uharddoom_common.h"

dev_t uharddoom_devno;

struct class uharddoom_class = {
	.name = "uharddoom",
	.owner = THIS_MODULE,
};

void wake_waiters(struct uharddoom_device *dev, uharddoom_va get,
	uharddoom_va put)
{
	struct list_head *pos;
	struct uharddoom_waitlist_entry *entry;

	unsigned get_idx = get / 16;

	while (!list_empty(&dev->waitlist)) {
		pos = dev->waitlist.next;
		entry = list_entry(pos, struct uharddoom_waitlist_entry, lh);

		if (entry->job_idx != get_idx
			&& in_buffer_incl(entry->job_idx * 16, get, put))
			return;

		wake_waiter(entry);
	}
}

void set_next_waitpoint(struct uharddoom_device *dev)
{
	struct uharddoom_waitlist_entry *entry;

	if (list_empty(&dev->waitlist)) {
		uharddoom_iow(dev, UHARDDOOM_BATCH_WAIT, UHARDDOOM_PAGE_SIZE);
	} else {
		entry = list_entry(
			dev->waitlist.next, struct uharddoom_waitlist_entry, lh
		);
		uharddoom_iow(dev, UHARDDOOM_BATCH_WAIT, entry->job_idx * 16);
	}
}

static void init_waitlist_entry(struct uharddoom_waitlist_entry *entry,
	unsigned job_idx)
{
	entry->job_idx = job_idx;
	entry->complete = 0;
	INIT_LIST_HEAD(&entry->lh);
	init_waitqueue_head(&entry->wq);
}

static void add_waitlist_entry(struct uharddoom_device *dev,
	struct uharddoom_waitlist_entry *new, uharddoom_va get,
	uharddoom_va put)
{
	struct list_head *pos = dev->waitlist.next;
	struct uharddoom_waitlist_entry *entry;

	unsigned new_job_addr = new->job_idx * 16;

	/* Skips unreached tasks before ours. */
	while (pos != &dev->waitlist) {
		entry = list_entry(pos, struct uharddoom_waitlist_entry, lh);
		if (!in_buffer_incl(new_job_addr, get, entry->job_idx * 16))
			break;

		pos = pos->next;
	}

	list_add_tail(&new->lh, pos);
}

int wait_for_addr(struct uharddoom_context *ctx, uharddoom_va waitpoint,
	unsigned long *slock_flags, unsigned interruptible)
{
	uharddoom_va get;
	uharddoom_va put;
	struct uharddoom_waitlist_entry entry;
	struct uharddoom_device *dev = ctx->dev;

	/* Pause the device. */
	uharddoom_iow(dev, UHARDDOOM_ENABLE, 0U);

	/* Check if waitpoint already reached. */
	get = uharddoom_ior(dev, UHARDDOOM_BATCH_GET);
	put = uharddoom_ior(dev, UHARDDOOM_BATCH_PUT);
	if (get == waitpoint || !in_buffer_incl(waitpoint, get, put)) {
		uharddoom_iow(dev, UHARDDOOM_ENABLE, UHARDDOOM_ENABLE_ALL);
		return 0;
	}

	wake_waiters(dev, get, put);
	init_waitlist_entry(&entry, waitpoint / 16);
	add_waitlist_entry(dev, &entry, get, put);
	set_next_waitpoint(dev);

	uharddoom_iow(dev, UHARDDOOM_ENABLE, UHARDDOOM_ENABLE_ALL);
	spin_unlock_irqrestore(&dev->slock, *slock_flags);

	if (interruptible) {
		if (wait_event_interruptible(entry.wq, entry.complete)) {
			spin_lock_irqsave(&dev->slock, *slock_flags);
			/* We need to remove the incomplete entry ourselves. */
			if (!entry.complete)
				list_del(&entry.lh);

			return -ERESTARTSYS;
		}
	} else {
		wait_event(entry.wq, entry.complete);
	}

	spin_lock_irqsave(&dev->slock, *slock_flags);
	if (ctx->error)
		return -EIO;

	return 0;
}
