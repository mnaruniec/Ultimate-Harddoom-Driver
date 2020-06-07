#ifndef UHARDDOOM_COMMON_H
#define UHARDDOOM_COMMON_H

#include <linux/cdev.h>
#include <linux/pci.h>

#define UHARDDOOM_MAX_DEVICES 256

struct uharddoom_context {
	struct uharddoom_device *dev;
};

struct uharddoom_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;
};

#endif  // UHARDDOOM_COMMON_H
