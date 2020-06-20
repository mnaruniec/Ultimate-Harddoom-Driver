/* Main device node handling. */
#include <linux/module.h>

#include "udoomdev.h"
#include "uharddoom.h"
#include "uharddoom_common.h"
#include "uharddoom_buffer.h"

#include "uharddoom_node.h"

static int udoomdev_open(struct inode *inode, struct file *file);
static int udoomdev_release(struct inode *inode, struct file *file);
static long udoomdev_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg);


const struct file_operations udoomdev_file_ops = {
	.owner = THIS_MODULE,
	.open = udoomdev_open,
	.release = udoomdev_release,
	.unlocked_ioctl = udoomdev_ioctl,
	.compat_ioctl = udoomdev_ioctl,
};

static int init_pagedir(struct uharddoom_device *dev,
			struct uharddoom_pagedir *pagedir)
{
	INIT_LIST_HEAD(&pagedir->pagetables);
	pagedir->data_cpu = dma_alloc_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		&pagedir->data_dma, GFP_KERNEL
	);
	if (!pagedir->data_cpu)
		return -ENOMEM;
	return 0;
}

static int udoomdev_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct uharddoom_device *dev = container_of(
		inode->i_cdev, struct uharddoom_device, cdev);
	struct uharddoom_context *ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
	ctx->error = 0;
	mutex_init(&ctx->vm_lock);
	INIT_LIST_HEAD(&ctx->user_mappings);
	ret = init_pagedir(dev, &ctx->user_pagedir);
	if (ret)
		goto out_free_context;

	file->private_data = ctx;
	return nonseekable_open(inode, file);
out_free_context:
	kfree(ctx);
	return ret;
}

static long ioctl_create_buffer(struct file *filp, unsigned long arg)
{
	struct udoomdev_ioctl_create_buffer arg_struct;
	if (copy_from_user(&arg_struct, (void __user *)arg, sizeof(arg_struct)))
		return -EFAULT;
	return create_buffer_fd(filp, arg_struct.size);
}

static void delete_pagetable(struct uharddoom_device *dev,
	struct uharddoom_pagetable *entry)
{
	struct list_head *pos = &entry->lh;

	list_del(pos);
	dma_free_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		entry->data_cpu, entry->data_dma
	);
	kfree(entry);
}

static void delete_pagetables(struct uharddoom_device *dev,
	struct list_head *list)
{
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_pagetable *entry;

	list_for_each_safe(pos, n, list) {
		entry = list_entry(pos, struct uharddoom_pagetable, lh);
		delete_pagetable(dev, entry);
	}
}

static int allocate_pde_range(struct uharddoom_context *ctx, unsigned start,
	unsigned last)
{
	int ret = -ENOMEM;
	int i;
	unsigned curr = 0;
	struct list_head *pos;
	struct list_head *before;
	struct list_head *n;
	struct uharddoom_pagetable *entry;
	struct uharddoom_pagetable *new;
	struct list_head new_list;
	unsigned *pagedir_entry_p;

	int skip_start = 0;
	int skip_last = 0;

	INIT_LIST_HEAD(&new_list);

	list_for_each(pos, &ctx->user_pagedir.pagetables) {
		entry = list_entry(pos, struct uharddoom_pagetable, lh);
		curr = entry->idx;
		if (curr == start) {
			skip_start = 1;
		} else if (curr > start) {
			break;
		}
	}

	if (last != 0 && curr == last)
		skip_last = 1;

	before = pos;

	for (i = start + skip_start; i <= last - skip_last; ++i) {
		new = kmalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			goto out_free_new_list;

		new->data_cpu = dma_alloc_coherent(
			&ctx->dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
			&new->data_dma, GFP_KERNEL | __GFP_ZERO
		);
		if (!new->data_cpu)
			goto out_free_new;

		new->idx = i;
		new->used = 0;
		INIT_LIST_HEAD(&new->lh);

		list_add_tail(&new->lh, &new_list);
	}

	pagedir_entry_p = ctx->user_pagedir.data_cpu;
	pagedir_entry_p += start + skip_start;

	pos = &new_list;
	n = pos->next;
	for (i = start + skip_start; i <= last - skip_last; ++i) {
		pos = n;
		n = pos->next;

		list_del(pos);
		list_add_tail(pos, before);

		entry = list_entry(pos, struct uharddoom_pagetable, lh);
		*pagedir_entry_p = (entry->data_dma >> UHARDDOOM_PDE_PA_SHIFT)
			| UHARDDOOM_PDE_PRESENT;

		pagedir_entry_p++;
	}

	return 0;
out_free_new:
	kfree(new);
out_free_new_list:
	delete_pagetables(ctx->dev, &new_list);
	return ret;
}

/* Must contain entry. */
static struct uharddoom_pagetable *get_pagetable(
	struct uharddoom_pagedir *pagedir, unsigned index)
{
	struct list_head *pos;
	struct uharddoom_pagetable *entry = NULL;

	list_for_each(pos, &pagedir->pagetables) {
		entry = list_entry(pos, struct uharddoom_pagetable, lh);
		if (entry->idx == index)
			break;
	}

	return entry;
}

static void buffer_fill_pagetables(struct uharddoom_pagedir *pagedir,
	struct uharddoom_buffer *buffer, uharddoom_va start_addr,
	unsigned readonly)
{
	unsigned i;
	unsigned page_count = num_pages(buffer->size);
	unsigned start_pdi = UHARDDOOM_VA_PDI(start_addr);
	unsigned start_pti = UHARDDOOM_VA_PTI(start_addr);

	struct list_head *pos;
	struct uharddoom_pagetable *entry;

	struct list_head *buffer_pos;
	struct uharddoom_page_node *buffer_entry;

	unsigned *pagetable_entry_p;

	unsigned writable = readonly ? 0 : UHARDDOOM_PTE_WRITABLE;

	entry = get_pagetable(pagedir, start_pdi);
	pos = &entry->lh;

	pagetable_entry_p = entry->data_cpu;
	pagetable_entry_p += start_pti;

	buffer_pos = &buffer->page_list;

	for (i = 0; i < page_count; ++i) {
		buffer_pos = buffer_pos->next;
		buffer_entry =
			list_entry(buffer_pos, struct uharddoom_page_node, lh);
		*pagetable_entry_p =
			(buffer_entry->data_dma >> UHARDDOOM_PTE_PA_SHIFT)
			| UHARDDOOM_PTE_PRESENT | writable;

		entry->used++;

		start_pti++;
		if (start_pti >= 1024) {
			start_pti = 0;
			start_pdi++;
			pos = pos->next;
			entry = list_entry(pos, struct uharddoom_pagetable, lh);
			pagetable_entry_p = entry->data_cpu;
		} else {
			pagetable_entry_p++;
		}
	}
}

static int add_mapping(struct uharddoom_context *ctx, struct file *filp,
	struct list_head *before_mapping, uharddoom_va start_addr,
	unsigned page_count, unsigned readonly)
{
	int ret;

	uharddoom_va last_addr = last_address(start_addr, page_count);
	unsigned start_pdi = UHARDDOOM_VA_PDI(start_addr);
	unsigned last_pdi = UHARDDOOM_VA_PDI(last_addr);

	/* Allocate and fill new mapping list node. */
	struct uharddoom_mapping *new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;

	INIT_LIST_HEAD(&new->lh);
	new->file = filp;
	new->start = start_addr;
	new->page_count = page_count;
	new->readonly = readonly;

	/* Allocate needed page tables in page dir. */
	ret = allocate_pde_range(ctx, start_pdi, last_pdi);
	if (ret < 0)
		goto out_free_mapping;

	/* Add new mapping to the list. */
	list_add_tail(&new->lh, before_mapping);

	/* Link buffer's pages in the page tables. */
	buffer_fill_pagetables(
		&ctx->user_pagedir, filp->private_data, start_addr, readonly
	);

	return start_addr;
out_free_mapping:
	kfree(new);
	return ret;
}

static long map_buffer(struct file *filp, unsigned buf_fd, unsigned readonly)
{
	long ret = 0;
	unsigned page_count;
	unsigned start;
	unsigned space;
	uharddoom_va prev_end = 0;
	uharddoom_va global_end = (UINT_MAX >> UHARDDOOM_PAGE_SHIFT) + 1;
	unsigned found = 0;

	struct file *b_filp;

	struct uharddoom_buffer *buffer;
	struct uharddoom_context *ctx = filp->private_data;

	struct list_head *pos;
	struct uharddoom_mapping *entry;

	b_filp = fget(buf_fd);
	if (!b_filp)
		return -EBADF;
	if (!is_buffer(b_filp)) {
		fput(b_filp);
		return -EPERM;
	}

	buffer = b_filp->private_data;
	if (buffer->dev != ctx->dev) {
		fput(b_filp);
		return -EPERM;
	}

	page_count = num_pages(buffer->size);

	if (mutex_lock_interruptible(&ctx->vm_lock)) {
		fput(b_filp);
		return -ERESTARTSYS;
	}

	list_for_each(pos, &ctx->user_mappings) {
		entry = list_entry(pos, struct uharddoom_mapping, lh);
		start = entry->start >> UHARDDOOM_PAGE_SHIFT;
		space = start - prev_end;
		if (space >= page_count) {
			found = 1;
			break;
		}
		prev_end = start + entry->page_count;
	}

	ret = -ENOMEM;
	if (!found) {
		if (prev_end << UHARDDOOM_PAGE_SHIFT > INT_MAX)
			/* We can't fit starting address in signed int. */
			goto out_fput;
		space = global_end - prev_end;
		if (space < page_count)
			/* We don't have enough space even at the end. */
			goto out_fput;
	}

	ret = add_mapping(
		ctx, b_filp, pos, prev_end << UHARDDOOM_PAGE_SHIFT,
		page_count, readonly
	);
	if (ret < 0)
		goto out_fput;

out_unlock:
	mutex_unlock(&ctx->vm_lock);
	return ret;
out_fput:
	fput(b_filp);
	goto out_unlock;
}

static long ioctl_map_buffer(struct file *filp, unsigned long arg)
{
	struct udoomdev_ioctl_map_buffer arg_struct;
	if (copy_from_user(&arg_struct, (void __user *)arg, sizeof(arg_struct)))
		return -EFAULT;
	return map_buffer(filp, arg_struct.buf_fd, arg_struct.map_rdonly);
}

static void clean_page_tables(struct uharddoom_context *ctx, uharddoom_va addr,
	unsigned page_count)
{
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_pagetable *entry;
	unsigned *pagetable_entry_p;
	unsigned *pagedir_p;

	unsigned i;
	unsigned long flags;
	unsigned start_pdi = UHARDDOOM_VA_PDI(addr);
	unsigned start_pti = UHARDDOOM_VA_PTI(addr);
	struct list_head deleted;

	INIT_LIST_HEAD(&deleted);

	pagedir_p = ctx->user_pagedir.data_cpu;

	entry = get_pagetable(&ctx->user_pagedir, start_pdi);
	pos = &entry->lh;
	n = pos->next;

	pagetable_entry_p = entry->data_cpu;
	pagetable_entry_p += start_pti;

	for (i = 0; i < page_count; ++i) {
		*pagetable_entry_p = 0U;
		entry->used--;

		start_pti++;

		if (!entry->used) {
			list_del(pos);
			list_add_tail(pos, &deleted);
			pagedir_p[entry->idx] = 0U;
		}

		if (start_pti >= 1024) {
			start_pti = 0;
			start_pdi++;
			pos = n;
			n = pos->next;
			entry = list_entry(pos, struct uharddoom_pagetable, lh);
			pagetable_entry_p = entry->data_cpu;
		} else {
			pagetable_entry_p++;
		}
	}

	/* Flush device TLB. */
	spin_lock_irqsave(&ctx->dev->slock, flags);
	uharddoom_iow(ctx->dev, UHARDDOOM_RESET, UHARDDOOM_RESET_TLB_USER);
	spin_unlock_irqrestore(&ctx->dev->slock, flags);

	/* Free deleted pagetables. */
	delete_pagetables(ctx->dev, &deleted);
}

static void remove_mapping(struct uharddoom_context *ctx,
	struct uharddoom_mapping* mapping)
{
	struct list_head *pos = &mapping->lh;
	uharddoom_va addr = mapping->start;
	unsigned page_count = mapping->page_count;
	struct file *file = mapping->file;

	list_del(pos);
	kfree(mapping);
	clean_page_tables(ctx, addr, page_count);
	fput(file);
}

static int unmap_buffer(struct file *filp, uharddoom_va addr)
{
	int ret = -ENOENT;
	struct uharddoom_mapping *mapping;
	struct list_head *pos;
	struct uharddoom_context *ctx = filp->private_data;

	if (mutex_lock_interruptible(&ctx->vm_lock))
		return -ERESTARTSYS;

	/* Find mapping. */
	list_for_each(pos, &ctx->user_mappings) {
		mapping = list_entry(pos, struct uharddoom_mapping, lh);
		if (mapping->start == addr)
			break;
	}

	if (pos == &ctx->user_mappings)
		goto out_unlock;

	remove_mapping(ctx, mapping);

	ret = 0;
out_unlock:
	mutex_unlock(&ctx->vm_lock);
	return ret;
}

static long ioctl_unmap_buffer(struct file *filp, unsigned long arg)
{
	struct udoomdev_ioctl_unmap_buffer arg_struct;
	if (copy_from_user(&arg_struct, (void __user *)arg, sizeof(arg_struct)))
		return -EFAULT;
	return unmap_buffer(filp, arg_struct.addr);
}

static inline unsigned buffer_full(struct uharddoom_device *dev,
	uharddoom_va get, uharddoom_va put)
{
	uharddoom_va after_put = (put + 16) % UHARDDOOM_PAGE_SIZE;
	unsigned after_put_idx = after_put / 16;

	/* Hardware buffer is full. */
	if (after_put == get)
		return 1;

	/* We need to let oldest waiters wake up. */
	return !list_empty(&dev->waitlist) && after_put_idx == list_first_entry(
		&dev->waitlist, struct uharddoom_waitlist_entry, lh)->job_idx;
}

static int run(struct file *filp, uharddoom_va addr, unsigned size)
{
	int ret = -EIO;

	unsigned long flags;
	uharddoom_va get;
	uharddoom_va put;
	uharddoom_va after_put;

	unsigned put_word;
	unsigned put_idx;
	unsigned *task_buffer;

	struct uharddoom_context *ctx = filp->private_data;
	struct uharddoom_device *dev = ctx->dev;

	if (addr & 3 || size & 3)
		return -EINVAL;

	spin_lock_irqsave(&dev->slock, flags);

	if (ctx->error)
		goto out_unlock;

	get = uharddoom_ior(dev, UHARDDOOM_BATCH_GET);
	put = uharddoom_ior(dev, UHARDDOOM_BATCH_PUT);
	after_put = (put + 16) % UHARDDOOM_PAGE_SIZE;

	while (buffer_full(dev, get, put)) {
		ret = wait_for_addr(
			ctx, (after_put + 16) % UHARDDOOM_PAGE_SIZE, &flags, 1
		);
		if (ret)
			goto out_unlock;

		get = uharddoom_ior(dev, UHARDDOOM_BATCH_GET);
		put = uharddoom_ior(dev, UHARDDOOM_BATCH_PUT);
		after_put = (put + 16) % UHARDDOOM_PAGE_SIZE;
	}

	put_word = put / 4;
	put_idx = put / 16;

	task_buffer = dev->kernel_pagedir.page_cpu;
	task_buffer[put_word] =
		ctx->user_pagedir.data_dma >> UHARDDOOM_PDP_SHIFT;
	task_buffer[put_word + 1] = addr;
	task_buffer[put_word + 2] = size;
	dev->job_context[put_idx] = ctx;
	uharddoom_iow(dev, UHARDDOOM_BATCH_PUT, after_put);

	ret = 0;
out_unlock:
	spin_unlock_irqrestore(&dev->slock, flags);
	return ret;
}

static long ioctl_run(struct file *filp, unsigned long arg)
{
	struct udoomdev_ioctl_run arg_struct;
	if (copy_from_user(&arg_struct, (void __user *)arg, sizeof(arg_struct)))
		return -EFAULT;
	return run(filp, arg_struct.addr, arg_struct.size);
}

static int wait(struct file *filp, unsigned num_back, unsigned interruptible)
{
	int ret = -EIO;
	unsigned long flags;
	uharddoom_va i;
	unsigned i_idx;
	unsigned found = 0;

	uharddoom_va put;
	uharddoom_va get;
	uharddoom_va waitpoint;
	struct uharddoom_context *ctx = filp->private_data;
	struct uharddoom_device *dev = ctx->dev;

	spin_lock_irqsave(&dev->slock, flags);

	if (ctx->error)
		goto out_unlock;

	if (num_back >= UHARDDOOM_PAGE_SIZE / 16)
		goto out_correct;

	put = uharddoom_ior(dev, UHARDDOOM_BATCH_PUT);
	get = uharddoom_ior(dev, UHARDDOOM_BATCH_GET);

	i = put;
	while (i != get) {
		i = i ? i : UHARDDOOM_PAGE_SIZE;
		i -= 16;
		i_idx = i / 16;

		if (dev->job_context[i_idx] == ctx) {
			if (!num_back) {
				found = 1;
				break;
			}
			num_back--;
		}
	}

	if (!found)
		goto out_correct;

	/* We want to wait for buffer head to reach job after the found one. */
	waitpoint = (i + 16) % UHARDDOOM_PAGE_SIZE;
	ret = wait_for_addr(ctx, waitpoint, &flags, interruptible);

out_unlock:
	spin_unlock_irqrestore(&dev->slock, flags);
	return ret;
out_correct:
	ret = 0;
	goto out_unlock;
}

static long ioctl_wait(struct file *filp, unsigned long arg)
{
	struct udoomdev_ioctl_wait arg_struct;
	if (copy_from_user(&arg_struct, (void __user *)arg, sizeof(arg_struct)))
		return -EFAULT;
	return wait(filp, arg_struct.num_back, 1);
}

static long udoomdev_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	switch (cmd) {
	case UDOOMDEV_IOCTL_CREATE_BUFFER:
		return ioctl_create_buffer(filp, arg);
	case UDOOMDEV_IOCTL_MAP_BUFFER:
		return ioctl_map_buffer(filp, arg);
	case UDOOMDEV_IOCTL_UNMAP_BUFFER:
		return ioctl_unmap_buffer(filp, arg);
	case UDOOMDEV_IOCTL_RUN:
		return ioctl_run(filp, arg);
	case UDOOMDEV_IOCTL_WAIT:
		return ioctl_wait(filp, arg);
	default:
		return -ENOTTY;
	}
}

static int udoomdev_release(struct inode *inode, struct file *file)
{
	struct uharddoom_context *ctx = file->private_data;
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_mapping *mapping;

	/* Wait uninterruptively for context's last task to finish. */
	wait(file, 0, 0);

	mutex_lock(&ctx->vm_lock);

	list_for_each_safe(pos, n, &ctx->user_mappings) {
		mapping = list_entry(pos, struct uharddoom_mapping, lh);
		remove_mapping(ctx, mapping);
	}

	/* Removing mappings should cover all pagetables. */
	BUG_ON(!list_empty(&ctx->user_pagedir.pagetables));

	mutex_unlock(&ctx->vm_lock);

	kfree(ctx);
	return 0;
}
