#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/kmod.h>

#include "uharddoom.h"
#include "uharddoom_common.h"
#include "uharddoom_node.h"
#include "uharddoom_driver.h"

MODULE_LICENSE("GPL");


/* Init & exit. */

static int uharddoom_init(void)
{
	int err;
	if ((err = alloc_chrdev_region(
		&uharddoom_devno, 0, UHARDDOOM_MAX_DEVICES, "uharddoom"
	)))
		goto err_chrdev;
	if ((err = class_register(&uharddoom_class)))
		goto err_class;
	if ((err = pci_register_driver(&uharddoom_pci_driver)))
		goto err_pci;

	printk(KERN_ALERT "uharddoom init\n");
	return 0;

err_pci:
	class_unregister(&uharddoom_class);
err_class:
	unregister_chrdev_region(uharddoom_devno, UHARDDOOM_MAX_DEVICES);
err_chrdev:
	return err;
}

static void uharddoom_exit(void)
{
	pci_unregister_driver(&uharddoom_pci_driver);
	class_unregister(&uharddoom_class);
	unregister_chrdev_region(uharddoom_devno, UHARDDOOM_MAX_DEVICES);
	printk(KERN_ALERT "uharddoom exit\n");
}

module_init(uharddoom_init);
module_exit(uharddoom_exit);
