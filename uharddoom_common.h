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
	unsigned int used;
	void *data_cpu;
	dma_addr_t data_dma;
	struct list_head lh;
};

struct uharddoom_compact_pagedir {
	void *data_cpu;
	dma_addr_t data_dma;
	void *pagetable_cpu;
	dma_addr_t pagetable_dma;
	void *page_cpu;
	dma_addr_t page_dma;
};

/* Memory mappings */
struct uharddoom_mapping {
	uharddoom_va start;
	unsigned int page_count;
	unsigned int readonly;
	struct file *file;
	struct list_head lh;
};

/* File descriptor context */
struct uharddoom_context {
	struct uharddoom_device *dev;
	unsigned error;
	struct mutex vm_lock;
	struct uharddoom_pagedir user_pagedir;
	struct list_head user_mappings;
};

/* One pending wait request. */
struct uharddoom_waitlist_entry {
	wait_queue_head_t wq;
	unsigned job_idx;
	unsigned complete;
	struct list_head lh;
};

/* Device. */
struct uharddoom_device {
	struct pci_dev *pdev;
	struct cdev cdev;
	int idx;
	struct device *dev;
	void __iomem *bar;
	spinlock_t slock;
	struct uharddoom_compact_pagedir kernel_pagedir;
	struct list_head waitlist;
	struct uharddoom_context *job_context[UHARDDOOM_PAGE_SIZE / 16];
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

static inline unsigned in_buffer(uharddoom_va job_addr, uharddoom_va get,
	uharddoom_va put)
{
	if (put == get)
		return 0;

	if (put > get)
		return job_addr >= get && job_addr < put;

	return job_addr >= get || job_addr < put;
}

/* True if job in buffer or at the put position */
static inline unsigned in_buffer_incl(uharddoom_va job_addr, uharddoom_va get,
       uharddoom_va put)
{
	return job_addr == put || in_buffer(job_addr, get, put);
}

static inline void wake_waiter(struct uharddoom_waitlist_entry *entry)
{
	list_del(&entry->lh);
	entry->complete = 1;
	wake_up(&entry->wq);
}

extern void wake_waiters(struct uharddoom_device *dev, uharddoom_va get,
	uharddoom_va put);

extern void set_next_waitpoint(struct uharddoom_device *dev);

#endif  // UHARDDOOM_COMMON_H
