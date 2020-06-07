/* PCI driver. */
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
//
// static irqreturn_t adlerdev_isr(int irq, void *opaque)
// {
// 	struct adlerdev_device *dev = opaque;
// 	unsigned long flags;
// 	uint32_t istatus;
// 	struct adlerdev_buffer *buf;
// 	spin_lock_irqsave(&dev->slock, flags);
// //	printk(KERN_ALERT "adlerdev isr\n");
// 	istatus = adlerdev_ior(dev, ADLERDEV_INTR) & adlerdev_ior(dev, ADLERDEV_INTR_ENABLE);
// 	if (istatus) {
// 		adlerdev_iow(dev, ADLERDEV_INTR, istatus);
// 		BUG_ON(list_empty(&dev->buffers_running));
// 		buf = list_entry(dev->buffers_running.next, struct adlerdev_buffer, lh);
// 		list_del(&buf->lh);
// 		buf->ctx->pending_buffers--;
// 		if (!buf->ctx->pending_buffers)
// 			wake_up(&buf->ctx->wq);
// 		buf->ctx->sum = adlerdev_ior(dev, ADLERDEV_SUM);
// 		buf->ctx = 0;
// 		list_add(&buf->lh, &dev->buffers_free);
// 		wake_up(&dev->free_wq);
// 		if (list_empty(&dev->buffers_running)) {
// 			/* No more buffers to run.  */
// 			wake_up(&dev->idle_wq);
// 		} else {
// 			/* Run the next buffer.  */
// 			buf = list_entry(dev->buffers_running.next, struct adlerdev_buffer, lh);
// 			adlerdev_iow(dev, ADLERDEV_DATA_PTR, buf->data_dma);
// 			adlerdev_iow(dev, ADLERDEV_SUM, buf->ctx->sum);
// 			adlerdev_iow(dev, ADLERDEV_DATA_SIZE, buf->fill_size);
// 		}
// 	}
// 	spin_unlock_irqrestore(&dev->slock, flags);
// 	return IRQ_RETVAL(istatus);
// }

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
// 	if ((err = request_irq(pdev->irq, adlerdev_isr, IRQF_SHARED, "adlerdev", dev)))
// 		goto out_irq;

// 	adlerdev_iow(dev, ADLERDEV_INTR, 1);
// 	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 1);

	/* We're live.  Let's export the cdev.  */
	cdev_init(&dev->cdev, &uharddoom_file_ops);
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
// 	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 0);
// 	free_irq(pdev->irq, dev);
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
	// adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 0);
	// free_irq(pdev->irq, dev);
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
	unsigned long flags;
	struct uharddoom_device *dev = pci_get_drvdata(pdev);
	spin_lock_irqsave(&dev->slock, flags);
	// TODO flush jobs
	spin_unlock_irqrestore(&dev->slock, flags);
// 	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 0);
	printk(KERN_ALERT "uharddoom suspend\n");
	return 0;
}

static int uharddoom_resume(struct pci_dev *pdev)
{
	struct uharddoom_device *dev = pci_get_drvdata(pdev);
// 	adlerdev_iow(dev, ADLERDEV_INTR, 1);
// 	adlerdev_iow(dev, ADLERDEV_INTR_ENABLE, 1);
	printk(KERN_ALERT "uharddoom resume\n");
	return 0;
}
