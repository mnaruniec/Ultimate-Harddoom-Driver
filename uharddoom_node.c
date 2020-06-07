/* Main device node handling. */
#include <linux/module.h>

#include "uharddoom_common.h"

#include "uharddoom_node.h"

static int uharddoom_open(struct inode *inode, struct file *file);
static int uharddoom_release(struct inode *inode, struct file *file);

const struct file_operations uharddoom_file_ops = {
	.owner = THIS_MODULE,
	.open = uharddoom_open,
	.release = uharddoom_release,
};

static int uharddoom_open(struct inode *inode, struct file *file)
{
	struct uharddoom_device *dev = container_of(inode->i_cdev, struct uharddoom_device, cdev);
	struct uharddoom_context *ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	printk(KERN_ALERT "uharddoom open\n");
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int uharddoom_release(struct inode *inode, struct file *file)
{
	struct uharddoom_context *ctx = file->private_data;
	struct uharddoom_device *dev = ctx->dev;
	unsigned long flags;
	printk(KERN_ALERT "uharddoom release\n");
	spin_lock_irqsave(&dev->slock, flags);
	// TODO wait for job finish
	spin_unlock_irqrestore(&dev->slock, flags);
	kfree(ctx);
	return 0;
}
