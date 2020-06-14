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
