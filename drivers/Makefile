CPU_CORES=$(shell nproc)

VERSION=$(shell uname -r)
VERSION_DIGIT=$(shell echo $(VERSION) | cut -c 1)
EXTRA_CFLAGS=-DVERSION_DIGIT=$(VERSION_DIGIT)

obj-y += lcd/
obj-y += touch/

KERNEL_PATH_UNAME := /usr/lib/modules/$(VERSION)/build/
KERNEL_PATH_FIRST := $(shell ls -d /usr/lib/modules/*/build/ | head -n 1)

PWD := $(shell pwd)

all:
	if [ -d "$(KERNEL_PATH_UNAME)" ]; then \
		make -C $(KERNEL_PATH_UNAME) M=$(PWD) modules EXTRA_CFLAGS=$(EXTRA_CFLAGS) -j$(CPU_CORES); \
	else \
		make -C $(KERNEL_PATH_FIRST) M=$(PWD) modules EXTRA_CFLAGS=$(EXTRA_CFLAGS) -j$(CPU_CORES); \
	fi
clean:
	if [ -d "$(KERNEL_PATH_UNAME)" ]; then \
		make -C $(KERNEL_PATH_UNAME) M=$(PWD) clean; \
	else \
		make -C $(KERNEL_PATH_FIRST) M=$(PWD) clean; \
	fi
