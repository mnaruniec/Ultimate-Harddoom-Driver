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

/* IRQ handler.  */

static irqreturn_t uharddoom_isr(int irq, void *opaque)
{
	struct uharddoom_device *dev = opaque;
	unsigned long flags;
	unsigned istatus;

	spin_lock_irqsave(&dev->slock, flags);
	printk(KERN_ALERT "uharddoom INTERRUPT\n");
	istatus = uharddoom_ior(dev, UHARDDOOM_INTR)
		& uharddoom_ior(dev, UHARDDOOM_INTR_ENABLE);
	if (istatus) {
		uharddoom_iow(dev, UHARDDOOM_INTR, istatus);
		printk(KERN_ALERT "uharddoom INTERRUPT: %x\n", istatus);
	}
	spin_unlock_irqrestore(&dev->slock, flags);
	return IRQ_RETVAL(istatus);
}

static void load_firmware(struct uharddoom_device *dev)
{
	unsigned i;
	uharddoom_iow(dev, UHARDDOOM_FE_CODE_ADDR, 0);
	for (i = 0; i < ARRAY_SIZE(udoomfw); ++i)
		uharddoom_iow(dev, UHARDDOOM_FE_CODE_WINDOW, udoomfw[i]);
}

static void turn_on_device(struct uharddoom_device *dev)
{
	uharddoom_iow(dev, UHARDDOOM_RESET, UHARDDOOM_RESET_ALL);
	// TODO initialize batch block
	uharddoom_iow(dev, UHARDDOOM_INTR, UHARDDOOM_INTR_MASK);  // TODO do in resume
	uharddoom_iow(dev, UHARDDOOM_INTR_ENABLE,
		UHARDDOOM_INTR_MASK & (~UHARDDOOM_INTR_BATCH_WAIT)); // TODO disable one of job exceptions
	uharddoom_iow(dev, UHARDDOOM_ENABLE,
		UHARDDOOM_ENABLE_ALL & (~UHARDDOOM_ENABLE_BATCH)); // TODO disable one of job blocks
}

static void turn_off_device(struct uharddoom_device *dev)
{
	uharddoom_iow(dev, UHARDDOOM_ENABLE, 0);
	uharddoom_iow(dev, UHARDDOOM_INTR_ENABLE, 0);
	uharddoom_ior(dev, UHARDDOOM_INTR_ENABLE);  // Just a trigger.
	uharddoom_iow(dev, UHARDDOOM_RESET, UHARDDOOM_RESET_ALL);  // TODO can we do it at the end
}

static int uharddoom_probe(struct pci_dev *pdev,
	const struct pci_device_id *pci_id)
{
	int err, i;

	/* Allocate our structure.  */
	struct uharddoom_device *dev = kzalloc(sizeof *dev, GFP_KERNEL);
	printk(KERN_ALERT "uharddoom probe\n");
	if (!dev) {
		err = -ENOMEM;
		goto out_alloc;
	}
	pci_set_drvdata(pdev, dev);
	dev->pdev = pdev;

	/* Locks etc.  */
	spin_lock_init(&dev->slock);

	/* Allocate a free index.  */
	mutex_lock(&uharddoom_devices_lock);
	for (i = 0; i < UHARDDOOM_MAX_DEVICES; i++)
		if (!uharddoom_devices[i])
			break;
	if (i == UHARDDOOM_MAX_DEVICES) {
		err = -ENOSPC; // TODO right?
		mutex_unlock(&uharddoom_devices_lock);
		goto out_slot;
	}
	uharddoom_devices[i] = dev;
	dev->idx = i;
	mutex_unlock(&uharddoom_devices_lock);

	/* Enable hardware resources.  */
	if ((err = pci_enable_device(pdev)))
		goto out_enable;

	if ((err = pci_set_dma_mask(pdev, DMA_BIT_MASK(40))))
		goto out_mask;
	if ((err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(40))))
		goto out_mask;
	pci_set_master(pdev);

	if ((err = pci_request_regions(pdev, "uharddoom")))
		goto out_regions;

	/* Map the BAR.  */
	if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
		err = -ENOMEM;
		goto out_bar;
	}

	/* Connect the IRQ line.  */
	if ((err = request_irq(
		pdev->irq, uharddoom_isr, IRQF_SHARED, "uharddoom", dev
	)))
		goto out_irq;

	load_firmware(dev);
	turn_on_device(dev);

	/* We're live.  Let's export the cdev.  */
	cdev_init(&dev->cdev, &udoomdev_file_ops);
	if ((err = cdev_add(&dev->cdev, uharddoom_devno + dev->idx, 1)))
		goto out_cdev;

	/* And register it in sysfs.  */
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
	uharddoom_iow(dev, UHARDDOOM_INTR_ENABLE, 0);
	free_irq(pdev->irq, dev);
out_irq:
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
	if (dev->dev) {
		device_destroy(&uharddoom_class, uharddoom_devno + dev->idx);
	}
	cdev_del(&dev->cdev);

	turn_off_device(dev);
	free_irq(pdev->irq, dev);

	pci_iounmap(pdev, dev->bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	mutex_lock(&uharddoom_devices_lock);
	uharddoom_devices[dev->idx] = 0;
	mutex_unlock(&uharddoom_devices_lock);

	kfree(dev);
	printk(KERN_ALERT "uharddoom remove\n");
}

static int uharddoom_suspend(struct pci_dev *pdev, pm_message_t state)
{
	// TODO should we block new tasks?
	unsigned long flags;
	struct uharddoom_device *dev = pci_get_drvdata(pdev);

	spin_lock_irqsave(&dev->slock, flags);
	// TODO flush jobs
	spin_unlock_irqrestore(&dev->slock, flags);

	turn_off_device(dev);
	printk(KERN_ALERT "uharddoom suspend\n");
	return 0;
}

static int uharddoom_resume(struct pci_dev *pdev)
{
	// TODO - check if need to load firmware
	struct uharddoom_device *dev = pci_get_drvdata(pdev);

	turn_on_device(dev);
	printk(KERN_ALERT "uharddoom resume\n");
	return 0;
}
