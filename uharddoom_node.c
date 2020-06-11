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

	printk(KERN_ALERT "init_pagedir - data_cpu %lx\n", (unsigned long)pagedir->data_cpu);
	return 0;
}

static int udoomdev_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct uharddoom_device *dev = container_of(
		inode->i_cdev, struct uharddoom_device, cdev);
	struct uharddoom_context *ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	printk(KERN_ALERT "uharddoom open\n");
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;
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
	printk(KERN_ALERT "uharddoom ioctl create buffer: %u\n", arg_struct.size);
	return create_buffer_fd(filp, arg_struct.size);
}

static void delete_pagetables(struct uharddoom_device *dev,
	struct list_head *list)
{
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_pagetable *entry;

	list_for_each_safe(pos, n, list) {
		entry = list_entry(pos, struct uharddoom_pagetable, lh);

		list_del(pos);
		dma_free_coherent(
			&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
			entry->data_cpu, entry->data_dma
		);
		kfree(entry);
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

	printk(KERN_ALERT "allocate_pde_range - start: %x, last: %x\n",
		start, last);

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

	printk(KERN_ALERT "allocate_pde_range - skip_start: %u, skip_last: %u\n",
		skip_start, skip_last);

	before = pos;
	//TODO remove
	// mutex_lock_killable(&ctx->vm_lock);

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

		// printk(KERN_ALERT "allocate_pde_range - allocated pagetable %u\n",
			// i);
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

		// printk(KERN_ALERT "allocate_pde_range - linked pagetable %u, entry_addr: %lx\n",
			// i, pagedir_entry_p);
		pagedir_entry_p++;
	}
	printk(KERN_ALERT "allocate_pde_range - success\n");

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

	uharddoom_va last_addr = last_address(start_addr, page_count);
	unsigned start_pdi = UHARDDOOM_VA_PDI(start_addr);
	unsigned last_pdi = UHARDDOOM_VA_PDI(last_addr);
	unsigned start_pti = UHARDDOOM_VA_PTI(start_addr);
	unsigned last_pti = UHARDDOOM_VA_PTI(last_addr);

	struct list_head *pos;
	struct uharddoom_pagetable *entry;

	struct list_head *buffer_pos;
	struct uharddoom_page_node *buffer_entry;

	unsigned *pagetable_entry_p;

	unsigned writable = readonly ? 0 : UHARDDOOM_PTE_WRITABLE;

	printk(KERN_ALERT "buffer_fill_pagetables - start_addr: %lx, last_addr: %lx\n",
		(unsigned long)start_addr, (unsigned long)last_addr);

	printk(KERN_ALERT "buffer_fill_pagetables - start_pdi: %x, start_pti: %x\n",
		start_pdi, start_pti);

	// TODO check
	entry = get_pagetable(pagedir, start_pdi);
	pos = &entry->lh;

	pagetable_entry_p = entry->data_cpu;
	pagetable_entry_p += start_pti;

	printk(KERN_ALERT "buffer_fill_pagetables - initial pagetable entry_addr: %lx\n",
		(unsigned long)pagetable_entry_p);

	buffer_pos = &buffer->page_list;

	for (i = 0; i < page_count; ++i) {
		// printk(KERN_ALERT "buffer_fill_pagetables - page %u\n",
			// i);
		// printk(KERN_ALERT "buffer_fill_pagetables - start_pdi: %x, start_pti: %x, entry_addr: %lx\n",
			// start_pdi, start_pti, pagetable_entry_p);

		buffer_pos = buffer_pos->next;
		buffer_entry =
			list_entry(buffer_pos, struct uharddoom_page_node, lh);
		*pagetable_entry_p =
			(buffer_entry->data_dma >> UHARDDOOM_PTE_PA_SHIFT)
			| UHARDDOOM_PTE_PRESENT | writable;

		entry->used++;

		start_pti++;
		if (start_pti >= 1024) {
			printk(KERN_ALERT "buffer_fill_pagetables - change pdi\n");
			start_pti = 0;
			start_pdi++;
			pos = pos->next;
			entry = list_entry(pos, struct uharddoom_pagetable, lh);
			pagetable_entry_p = entry->data_cpu;
		} else {
			pagetable_entry_p++;
		}
	}
	printk(KERN_ALERT "buffer_fill_pagetables - exit\n");
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

	printk(KERN_ALERT "add_mapping - start_addr: %lx, last_addr: %lx, page_count: %u\n",
		(unsigned long)start_addr, (unsigned long)last_addr, page_count);
	printk(KERN_ALERT "add_mapping - start_pdi: %x, last_pdi: %x\n",
		start_pdi, last_pdi);

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

// TODO check address limit to signed int
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
		return -EACCES;  // Analogous to mmap's EACCES.
	}
	printk(KERN_ALERT "map_buffer - F_COUNT: %ld\n", atomic_long_read(&b_filp->f_count));


	buffer = b_filp->private_data;
	if (buffer->dev != ctx->dev) {
		fput(b_filp);
		return -EACCES;  // TODO right?
	}

	page_count = num_pages(buffer->size);
	printk(KERN_ALERT "map_buffer - page_count: %u\n", page_count);

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

	if (!found) {
		if (prev_end << UHARDDOOM_PAGE_SHIFT > INT_MAX) {
			/* We can't fit starting address in signed int. */
			printk(KERN_ALERT "map_buffer - uint32_t overflow\n");
			ret = -ENOMEM;
			goto out_fput;
		}
		space = global_end - prev_end;
		if (space < page_count) {
			/* We don't have enough space even at the end. */
			printk(KERN_ALERT "map_buffer - too little end space\n");
			ret = -ENOMEM;
			goto out_fput;
		}
		printk(KERN_ALERT "map_buffer - mapping at the end\n");
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
	printk(KERN_ALERT "uharddoom ioctl map buffer fd: %u, readonly %u\n",
		arg_struct.buf_fd, arg_struct.map_rdonly);
	return map_buffer(filp, arg_struct.buf_fd, arg_struct.map_rdonly);
}

// TODO check and test
static void clean_page_tables(struct uharddoom_context *ctx, uharddoom_va addr,
	unsigned page_count)
{
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_pagetable *entry;
	unsigned *pagetable_entry_p;

	unsigned i;
	unsigned start_pdi = UHARDDOOM_VA_PDI(addr);
	unsigned start_pti = UHARDDOOM_VA_PTI(addr);

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
			printk(KERN_ALERT "clean_page_tables - remove pagetable %u\n",
				entry->idx);
			list_del(pos);
		}

		if (start_pti >= 1024) {
			printk(KERN_ALERT "clean_page_tables - change pdi\n");

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

	// TODO reset TLB
}

static void remove_mapping(struct uharddoom_context *ctx,
	struct uharddoom_mapping* mapping)
{
	struct list_head *pos = &mapping->lh;
	uharddoom_va addr = mapping->start;
	unsigned page_count = mapping->page_count;
	struct file *file = mapping->file;

	printk(KERN_ALERT "removing mapping starting at %x\n", mapping->start);
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
	printk(KERN_ALERT "uharddoom ioctl unmap buffer\n");
	return unmap_buffer(filp, arg_struct.addr);
}

static long ioctl_run(struct file *filp, unsigned long arg)
{
	printk(KERN_ALERT "uharddoom ioctl run\n");
	// TODO
	return 0;
}

static long ioctl_wait(struct file *filp, unsigned long arg)
{
	printk(KERN_ALERT "uharddoom ioctl wait\n");
	// TODO
	return 0;
}

static long udoomdev_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	printk(KERN_ALERT "uharddoom ioctl\n");
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
	struct uharddoom_device *dev = ctx->dev;
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_mapping *mapping;

	unsigned long flags;
	printk(KERN_ALERT "uharddoom release\n");

	spin_lock_irqsave(&dev->slock, flags);
	// TODO wait for job finish
	spin_unlock_irqrestore(&dev->slock, flags);

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
