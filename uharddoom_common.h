#ifndef UHARDDOOM_COMMON_H
#define UHARDDOOM_COMMON_H

#include <linux/cdev.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/pci.h>

#include "uharddoom.h"

#define UHARDDOOM_MAX_DEVICES 256

extern dev_t uharddoom_devno;
extern struct class uharddoom_class;

typedef unsigned int uharddoom_va;

/* Buffer file descriptors. */
struct uharddoom_page_node {
	void *data_cpu;
	dma_addr_t data_dma;
	struct list_head lh;
};

struct uharddoom_buffer {
	unsigned int size;
	struct list_head page_list;
	struct uharddoom_device *dev;
};

/* Pagetables. */
struct uharddoom_pagedir {
	void *data_cpu;
	dma_addr_t data_dma;
	struct list_head pagetables;
};

// TODO unify int types (uint32_t etc.)
struct uharddoom_pagetable {
	unsigned int idx;
	void *data_cpu;
	dma_addr_t data_dma;
	struct list_head lh;
};

/* Memory mappings */
struct uharddoom_mapping {
	uharddoom_va start;
	unsigned int page_count;
	unsigned int readonly;
	struct fd file;
	struct list_head lh;
};

/* File descriptor context */
struct uharddoom_context {
	struct uharddoom_device *dev;
	struct mutex vm_lock;
	struct uharddoom_pagedir user_pagedir;
	struct list_head user_mappings;
};

/* Device. */
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

static inline uint32_t num_pages(uint32_t size)
{
	uint32_t over = 0;
	uint32_t last_page = round_down(UINT_MAX, PAGE_SIZE);
	if (size > last_page) {
		size = last_page;
		over = 1;
	}
	return (round_up(size, PAGE_SIZE) / PAGE_SIZE) + over;
}

static inline uharddoom_va last_address(uharddoom_va start_addr,
	unsigned page_count)
{
	unsigned long last_addr =
		start_addr + page_count * UHARDDOOM_PAGE_SIZE - 1;
	return (uharddoom_va)last_addr;
}

#endif  // UHARDDOOM_COMMON_H
