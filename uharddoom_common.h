#ifndef UHARDDOOM_COMMON_H
#define UHARDDOOM_COMMON_H

#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/pci.h>

#define UHARDDOOM_MAX_DEVICES 256

extern dev_t uharddoom_devno;
extern struct class uharddoom_class;

struct uharddoom_page_node {
	void *data_cpu;
	dma_addr_t data_dma;
	struct list_head lh;
};

struct uharddoom_buffer {
	unsigned int size;
	struct list_head page_list;
	struct uharddoom_context  *ctx;
};

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

static inline void uharddoom_iow(struct uharddoom_device *dev,
				uint32_t reg, uint32_t val)
{
	iowrite32(val, dev->bar + reg);
}

static inline uint32_t uharddoom_ior(struct uharddoom_device *dev, uint32_t reg)
{
	uint32_t res = ioread32(dev->bar + reg);
	return res;
}

#endif  // UHARDDOOM_COMMON_H
