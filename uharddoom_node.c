/* Main device node handling. */
#include <linux/module.h>

#include "udoomdev.h"
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

static int udoomdev_open(struct inode *inode, struct file *file)
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

static int udoomdev_release(struct inode *inode, struct file *file)
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

static long ioctl_create_buffer(struct file *filp, unsigned long arg)
{
	struct udoomdev_ioctl_create_buffer arg_struct;
	if (copy_from_user(&arg_struct, (void __user *)arg, sizeof(arg_struct)))
		return -EFAULT;
	printk(KERN_ALERT "uharddoom ioctl create buffer: %u\n", arg_struct.size);
	return create_buffer_fd(filp, arg_struct.size);
}

static long ioctl_map_buffer(struct file *filp, unsigned long arg)
{
	printk(KERN_ALERT "uharddoom ioctl map buffer\n");
	// TODO
	return 0;
}

static long ioctl_unmap_buffer(struct file *filp, unsigned long arg)
{
	printk(KERN_ALERT "uharddoom ioctl unmap buffer\n");
	// TODO
	return 0;
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
