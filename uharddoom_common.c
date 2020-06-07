#include "uharddoom_common.h"

dev_t uharddoom_devno;

struct class uharddoom_class = {
	.name = "uharddoom",
	.owner = THIS_MODULE,
};
