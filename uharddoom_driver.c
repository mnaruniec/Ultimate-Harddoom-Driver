/* PCI driver. */
#include <linux/kernel.h>

#include "udoomfw.h"
#include "uharddoom.h"
#include "uharddoom_common.h"
#include "uharddoom_node.h"

#include "uharddoom_driver.h"

static struct pci_device_id uharddoom_pciids[] = {
	{ PCI_DEVICE(UHARDDOOM_VENDOR_ID, UHARDDOOM_DEVICE_ID) },
	{ 0 }
};

static int uharddoom_probe(struct pci_dev *pdev,
	const struct pci_device_id *pci_id);
static void uharddoom_remove(struct pci_dev *pdev);
static int uharddoom_suspend(struct pci_dev *pdev, pm_message_t state);
static int uharddoom_resume(struct pci_dev *pdev);


struct pci_driver uharddoom_pci_driver = {
	.name = "uharddoom",
	.id_table = uharddoom_pciids,
	.probe = uharddoom_probe,
	.remove = uharddoom_remove,
	.suspend = uharddoom_suspend,
	.resume = uharddoom_resume,
};

static struct uharddoom_device *uharddoom_devices[UHARDDOOM_MAX_DEVICES];
static DEFINE_MUTEX(uharddoom_devices_lock);

static void cancel_ctx_tasks(struct uharddoom_context *ctx)
{
	unsigned i;
	struct list_head *pos;
	struct list_head *n;
	struct uharddoom_waitlist_entry *entry;
	struct uharddoom_context *entry_ctx;

	struct uharddoom_device *dev = ctx->dev;
	unsigned *buffer_p = dev->kernel_pagedir.page_cpu;

	list_for_each_safe(pos, n, &dev->waitlist) {
		entry = list_entry(pos, struct uharddoom_waitlist_entry, lh);
		entry_ctx = dev->job_context[entry->job_idx];

		if (ctx == entry_ctx)
			wake_waiter(entry);
	}

	for (i = 0; i < ARRAY_SIZE(dev->job_context); ++i) {
		entry_ctx = dev->job_context[i];

		if (ctx == entry_ctx)
			buffer_p[i * 4 + 2] = 0U;
	}
}

/* IRQ handler. */
static irqreturn_t uharddoom_isr(int irq, void *opaque)
{
	struct uharddoom_device *dev = opaque;
	struct uharddoom_context *ctx;
	unsigned long flags;
	unsigned istatus;
	unsigned err_status;

	uharddoom_va get;
	uharddoom_va put;
	unsigned get_idx;

	printk(KERN_ALERT "isr LOCK BEFORE ACQ\n");
	spin_lock_irqsave(&dev->slock, flags);
	printk(KERN_ALERT "isr LOCK AFTER ACQ\n");

	printk(KERN_ALERT "uharddoom INTERRUPT\n");
	istatus = uharddoom_ior(dev, UHARDDOOM_INTR)
		& uharddoom_ior(dev, UHARDDOOM_INTR_ENABLE);
	printk(KERN_ALERT "INTERRUPT step 0, istatus: %x\n", istatus);

	if (istatus) {
		printk(KERN_ALERT "uharddoom INTERRUPT: %x\n", istatus);
		err_status = istatus & (~UHARDDOOM_INTR_BATCH_WAIT);

		printk(KERN_ALERT "INTERRUPT step 1\n");
		/* Pause the device. */
		uharddoom_iow(dev, UHARDDOOM_ENABLE, 0U);
		printk(KERN_ALERT "INTERRUPT step 2\n");

		/* Get job pointers. */
		get = uharddoom_ior(dev, UHARDDOOM_BATCH_GET);
		put = uharddoom_ior(dev, UHARDDOOM_BATCH_PUT);
		get_idx = get / 16;
		printk(KERN_ALERT "INTERRUPT step 3\n");

		if (err_status) {
			printk(KERN_ALERT "INTERRUPT step 4\n");

			ctx = dev->job_context[get_idx];
			printk(KERN_ALERT "INTERRUPT step 5\n");

			/* Mark context corrupted. */
			ctx->error = 1;
			printk(KERN_ALERT "INTERRUPT step 6\n");

			/* Cancel it's tasks. */
			cancel_ctx_tasks(ctx);
			printk(KERN_ALERT "INTERRUPT step 7\n");

			/* Move ptr to the next task. */
			get = (get + 16) % UHARDDOOM_PAGE_SIZE;
			uharddoom_iow(dev, UHARDDOOM_BATCH_GET, get);
			printk(KERN_ALERT "INTERRUPT step 8\n");

			/* Reset the device state. */
			uharddoom_iow(dev, UHARDDOOM_RESET,
				UHARDDOOM_RESET_ALL);
			printk(KERN_ALERT "INTERRUPT step 9\n");

		}

		uharddoom_iow(dev, UHARDDOOM_INTR, UHARDDOOM_INTR_MASK);
		printk(KERN_ALERT "INTERRUPT step 10\n");

		/* Wake up processes with reached jobs. */
		wake_waiters(dev, get, put);
		printk(KERN_ALERT "INTERRUPT step 11\n");

		/* Move BATCH_WAIT. */
		set_next_waitpoint(dev);
		printk(KERN_ALERT "INTERRUPT step 12\n");

		/* Resume the device. */
		uharddoom_iow(dev, UHARDDOOM_ENABLE, UHARDDOOM_ENABLE_ALL);
		printk(KERN_ALERT "INTERRUPT step 13\n");
	}

	printk(KERN_ALERT "isr LOCK BEFORE REL\n");
	spin_unlock_irqrestore(&dev->slock, flags);
	printk(KERN_ALERT "isr LOCK AFTER REL\n");
	return IRQ_RETVAL(istatus);
}

static int init_batch_buffer(struct uharddoom_device *dev)
{
	int ret = -ENOMEM;
	struct uharddoom_compact_pagedir *pagedir = &dev->kernel_pagedir;
	unsigned *pagedir_entry_p;
	unsigned *pagetable_entry_p;

	pagedir->data_cpu = dma_alloc_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		&pagedir->data_dma, GFP_KERNEL | __GFP_ZERO
	);
	if (!pagedir->data_cpu)
		goto out;

	pagedir->pagetable_cpu = dma_alloc_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		&pagedir->pagetable_dma, GFP_KERNEL | __GFP_ZERO
	);
	if (!pagedir->pagetable_cpu)
		goto out_free_pagedir;

	pagedir->page_cpu = dma_alloc_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		&pagedir->page_dma, GFP_KERNEL | __GFP_ZERO
	);
	if (!pagedir->page_cpu)
		goto out_free_pagetable;

	pagedir_entry_p = pagedir->data_cpu;
	pagetable_entry_p = pagedir->pagetable_cpu;

	*pagedir_entry_p =
		(pagedir->pagetable_dma >> UHARDDOOM_PDE_PA_SHIFT)
		| UHARDDOOM_PDE_PRESENT;
	*pagetable_entry_p =
		(pagedir->page_dma >> UHARDDOOM_PTE_PA_SHIFT)
		| UHARDDOOM_PTE_PRESENT;

	ret = 0;
out:
	return ret;
out_free_pagetable:
	dma_free_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		pagedir->pagetable_cpu, pagedir->pagetable_dma
	);
out_free_pagedir:
	dma_free_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		pagedir->data_cpu, pagedir->data_dma
	);
	goto out;
}

static void delete_batch_buffer(struct uharddoom_device *dev)
{
	struct uharddoom_compact_pagedir *pagedir = &dev->kernel_pagedir;

	dma_free_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		pagedir->page_cpu, pagedir->page_dma
	);

	dma_free_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		pagedir->pagetable_cpu, pagedir->pagetable_dma
	);

	dma_free_coherent(
		&dev->pdev->dev, UHARDDOOM_PAGE_SIZE,
		pagedir->data_cpu, pagedir->data_dma
	);
}

static void load_firmware(struct uharddoom_device *dev)
{
	unsigned i;
	uharddoom_iow(dev, UHARDDOOM_FE_CODE_ADDR, 0);
	printk(KERN_ALERT "firmware_size: %lu\n", ARRAY_SIZE(udoomfw));
	for (i = 0; i < ARRAY_SIZE(udoomfw); ++i)
		uharddoom_iow(dev, UHARDDOOM_FE_CODE_WINDOW, udoomfw[i]);
}

static void turn_on_device(struct uharddoom_device *dev)
{
	uharddoom_iow(dev, UHARDDOOM_RESET, UHARDDOOM_RESET_ALL);

	uharddoom_iow(dev, UHARDDOOM_BATCH_PDP,
		dev->kernel_pagedir.data_dma >> UHARDDOOM_PDP_SHIFT);
	uharddoom_iow(dev, UHARDDOOM_BATCH_GET, 0U);
	uharddoom_iow(dev, UHARDDOOM_BATCH_PUT, 0U);
	uharddoom_iow(dev, UHARDDOOM_BATCH_WRAP_TO, 0U);
	uharddoom_iow(dev, UHARDDOOM_BATCH_WRAP_FROM, UHARDDOOM_PAGE_SIZE);
	uharddoom_iow(dev, UHARDDOOM_BATCH_WAIT, UHARDDOOM_PAGE_SIZE);

	uharddoom_iow(dev, UHARDDOOM_INTR, UHARDDOOM_INTR_MASK);
	uharddoom_iow(dev, UHARDDOOM_INTR_ENABLE,
		UHARDDOOM_INTR_MASK & (~UHARDDOOM_INTR_JOB_DONE));
	uharddoom_iow(dev, UHARDDOOM_ENABLE, UHARDDOOM_ENABLE_ALL);
}

static void turn_off_device(struct uharddoom_device *dev)
{
	uharddoom_iow(dev, UHARDDOOM_ENABLE, 0);
	uharddoom_iow(dev, UHARDDOOM_RESET, UHARDDOOM_RESET_ALL);
	uharddoom_iow(dev, UHARDDOOM_INTR_ENABLE, 0);
	uharddoom_ior(dev, UHARDDOOM_INTR_ENABLE);  // Just a trigger.
}

static int uharddoom_probe(struct pci_dev *pdev,
	const struct pci_device_id *pci_id)
{
	int err, i;

	/* Allocate our structure. */
	struct uharddoom_device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
	printk(KERN_ALERT "uharddoom probe\n");
	if (!dev) {
		err = -ENOMEM;
		goto out_alloc;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	/* Locks, lists etc. */
	spin_lock_init(&dev->slock);
	INIT_LIST_HEAD(&dev->waitlist);

	/* Allocate a free index. */
	mutex_lock(&uharddoom_devices_lock);
	for (i = 0; i < UHARDDOOM_MAX_DEVICES; i++)
		if (!uharddoom_devices[i])
			break;
	if (i == UHARDDOOM_MAX_DEVICES) {
		err = -ENOSPC;
		mutex_unlock(&uharddoom_devices_lock);
		goto out_slot;
	}
	uharddoom_devices[i] = dev;
	dev->idx = i;
	mutex_unlock(&uharddoom_devices_lock);

	/* Enable hardware resources. */
	if ((err = pci_enable_device(pdev)))
		goto out_enable;

	if ((err = pci_set_dma_mask(pdev, DMA_BIT_MASK(40))))
		goto out_mask;
	if ((err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(40))))
		goto out_mask;
	pci_set_master(pdev);

	if ((err = pci_request_regions(pdev, "uharddoom")))
		goto out_regions;

	/* Map the BAR. */
	if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
		err = -ENOMEM;
		goto out_bar;
	}

	/* Allocate kernel batch buffer. */
	if ((err = init_batch_buffer(dev)))
		goto out_batch_buffer;

	/* Connect the IRQ line. */
	if ((err = request_irq(
		pdev->irq, uharddoom_isr, IRQF_SHARED, "uharddoom", dev
	)))
		goto out_irq;

	/* Enable the device. */
	load_firmware(dev);
	turn_on_device(dev);

	/* We're live.  Let's export the cdev. */
	cdev_init(&dev->cdev, &udoomdev_file_ops);
	if ((err = cdev_add(&dev->cdev, uharddoom_devno + dev->idx, 1)))
		goto out_cdev;

	/* And register it in sysfs. */
	dev->dev = device_create(&uharddoom_class,
			&dev->pdev->dev, uharddoom_devno + dev->idx, dev,
			"udoom%d", dev->idx);
	if (IS_ERR(dev->dev)) {
		printk(KERN_ERR "uharddoom: failed to register subdevice\n");
		/* too bad. */
		dev->dev = 0;
	}

	return 0;

out_cdev:
	turn_off_device(dev);
	free_irq(pdev->irq, dev);
out_irq:
	delete_batch_buffer(dev);
out_batch_buffer:
	pci_iounmap(pdev, dev->bar);
out_bar:
	pci_release_regions(pdev);
out_regions:
out_mask:
	pci_disable_device(pdev);
out_enable:
	mutex_lock(&uharddoom_devices_lock);
	uharddoom_devices[dev->idx] = 0;
	mutex_unlock(&uharddoom_devices_lock);
out_slot:
	kfree(dev);
out_alloc:
	return err;
}

static void uharddoom_remove(struct pci_dev *pdev)
{
	struct uharddoom_device *dev = pci_get_drvdata(pdev);
	printk(KERN_ALERT "uharddoom remove begin\n");

	if (dev->dev) {
		device_destroy(&uharddoom_class, uharddoom_devno + dev->idx);
	}
	cdev_del(&dev->cdev);

	turn_off_device(dev);
	free_irq(pdev->irq, dev);
	delete_batch_buffer(dev);

	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	mutex_lock(&uharddoom_devices_lock);
	uharddoom_devices[dev->idx] = 0;
	mutex_unlock(&uharddoom_devices_lock);

	kfree(dev);
	printk(KERN_ALERT "uharddoom remove end\n");
}

static int uharddoom_suspend(struct pci_dev *pdev, pm_message_t state)
{
	unsigned long flags;
	uharddoom_va get;
	uharddoom_va put;
	unsigned ctx_idx;
	struct uharddoom_device *dev = pci_get_drvdata(pdev);
	struct uharddoom_context *ctx;

	spin_lock_irqsave(&dev->slock, flags);

	put = uharddoom_ior(dev, UHARDDOOM_BATCH_PUT);
	get = uharddoom_ior(dev, UHARDDOOM_BATCH_GET);

	if (put != get) {
		ctx_idx = (put ? put - 16 : UHARDDOOM_PAGE_SIZE - 16) / 16;
		ctx = dev->job_context[ctx_idx];
		wait_for_addr(ctx, put, &flags, 0);
	}

	spin_unlock_irqrestore(&dev->slock, flags);

	turn_off_device(dev);
	printk(KERN_ALERT "uharddoom suspend\n");
	return 0;
}

static int uharddoom_resume(struct pci_dev *pdev)
{
	struct uharddoom_device *dev = pci_get_drvdata(pdev);
	load_firmware(dev);
	turn_on_device(dev);
	printk(KERN_ALERT "uharddoom resume\n");
	return 0;
}
