BUILD_DIR ?= build

ifneq ($(KERNELRELEASE),)
	obj-m := corsair-void.o

else
	KDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

$(BUILD_DIR)/corsair-void.ko: src/*
	$(MAKE) default
default: prepare
	cp -r src/* $(BUILD_DIR)/
	$(MAKE) -C $(KDIR) M=$(PWD)/$(BUILD_DIR) modules
install: prepare
	$(MAKE) -C $(KDIR) M=$(PWD)/$(BUILD_DIR) modules_install
	depmod -A
clean:
	@if [ -d $(BUILD_DIR) ]; then \
	  $(MAKE) -C $(KDIR) M=$(PWD)/$(BUILD_DIR) clean; \
	  rm -rfv $(BUILD_DIR); \
	fi
prepare:
	mkdir -p $(BUILD_DIR)
	rm -f $(BUILD_DIR)/Makefile
	ln -s ../Makefile $(BUILD_DIR)/Makefile

endif
