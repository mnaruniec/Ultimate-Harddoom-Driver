# TODO change to /lib/modules/`uname -r`/build
KDIR ?= ./linux-5.5.5

default:
	$(MAKE) -C $(KDIR) M=$$PWD

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
