#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/module.h>

#include "uharddoom_common.h"
#include "uharddoom_buffer.h"

static const struct file_operations buffer_file_ops = {
	.owner = THIS_MODULE,
};

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

	if (size > round_down(UINT_MAX, PAGE_SIZE) || !size)
		return -EINVAL;

	page_count = round_up(size, PAGE_SIZE) / PAGE_SIZE;

	buffer = kmalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	INIT_LIST_HEAD(&buffer->page_list);
	buffer->size = size;
	buffer->ctx = ctx;

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
