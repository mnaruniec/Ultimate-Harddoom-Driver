#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "uharddoom_common.h"
#include "uharddoom_buffer.h"

static int buffer_mmap(struct file *filp, struct vm_area_struct *vma);
static int buffer_release(struct inode *inode, struct file *file);
static vm_fault_t buffer_fault(struct vm_fault *vmf);

static const struct file_operations buffer_file_ops = {
	.owner = THIS_MODULE,
	.mmap = buffer_mmap,
	.release = buffer_release,
};

static const struct vm_operations_struct buffer_vm_ops = {
	.fault = buffer_fault,
};

int is_buffer(struct file *filp)
{
	return filp->f_op == &buffer_file_ops;
}

static void delete_buffer(struct uharddoom_device *dev,
	struct uharddoom_buffer *buf)
{
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_page_node *node;

	list_for_each_safe(pos, n, &buf->page_list) {
		list_del(pos);

		node = list_entry(pos, struct uharddoom_page_node, lh);

		dma_free_coherent(&dev->pdev->dev, PAGE_SIZE,
			node->data_cpu, node->data_dma);
		kfree(node);
	}

	kfree(buf);
}

long create_buffer_fd(struct file *filp, unsigned int size)
{
	long ret = 0;
	unsigned page_count;
	unsigned i;

	struct uharddoom_page_node *page_node;
	struct uharddoom_buffer *buffer;

	struct uharddoom_context *ctx = filp->private_data;
	struct uharddoom_device *dev = ctx->dev;

	printk(KERN_ALERT "create buffer\n");

	page_count = num_pages(size);
	if (!page_count)
		return -EINVAL;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	INIT_LIST_HEAD(&buffer->page_list);
	buffer->size = size;
	buffer->dev = dev;

	ret = -ENOMEM;
	for (i = 0; i < page_count; ++i) {
		page_node = kmalloc(sizeof(*page_node), GFP_KERNEL);
		if (!page_node)
			goto out_delete_buf;

		INIT_LIST_HEAD(&page_node->lh);
		page_node->data_cpu = dma_alloc_coherent(
			&dev->pdev->dev, PAGE_SIZE, &page_node->data_dma,
			GFP_KERNEL | __GFP_ZERO
		);
		if (!page_node->data_cpu)
			goto out_free_node;

		list_add_tail(&page_node->lh, &buffer->page_list);
	}

	ret = anon_inode_getfd(
		"uharddoom_buffer", &buffer_file_ops, buffer, O_RDWR);

	if (ret < 0)
		goto out_delete_buf;

out:
	return ret;
out_free_node:
	kfree(page_node);
out_delete_buf:
	delete_buffer(dev, buffer);
	goto out;
}

static int buffer_mmap(struct file *filp, struct vm_area_struct *vma)
{
	vma->vm_ops = &buffer_vm_ops;
	return 0;
}

static vm_fault_t buffer_fault(struct vm_fault *vmf)
{
	unsigned i;
	struct file *filp = vmf->vma->vm_file;
	struct uharddoom_buffer *buffer = filp->private_data;
	struct list_head *pos = &buffer->page_list;
	struct uharddoom_page_node *node;
	struct page *page;
	printk(KERN_ALERT "buffer fault, pgoff = %lu\n", vmf->pgoff);

	if (!filp) {
		BUG();
		return VM_FAULT_SIGBUS;
	}
	if (vmf->pgoff >= num_pages(buffer->size))
		return VM_FAULT_SIGBUS;

	for (i = 0; i <= vmf->pgoff; ++i)
		pos = pos->next;

	BUG_ON(pos == &buffer->page_list);
	node = list_entry(pos, struct uharddoom_page_node, lh);
	page = virt_to_page(node->data_cpu);
	get_page(page);
	vmf->page = page;
	return 0;
}

static int buffer_release(struct inode *inode, struct file *filp)
{
	struct uharddoom_buffer *buffer = filp->private_data;
	struct uharddoom_device *dev = buffer->dev;
	delete_buffer(dev, filp->private_data);
	printk(KERN_ALERT "release buffer\n");
	return 0;
}
