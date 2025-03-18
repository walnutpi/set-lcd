CPU_CORES=$(shell nproc)

VERSION_DIGIT=$(shell ls /usr/lib/modules/ | cut -c 1)
EXTRA_CFLAGS=-DVERSION_DIGIT=$(VERSION_DIGIT)

obj-y += lcd/
obj-y += touch/
obj-y += power/

# KERNEL_PATH_UNAME := /usr/lib/modules/$(VERSION)/build/
KERNEL_PATH_FIRST := $(shell ls -d /usr/lib/modules/*/build/ | head -n 1)

PWD := $(shell pwd)

all:
	make -C $(KERNEL_PATH_FIRST) M=$(PWD) modules EXTRA_CFLAGS=$(EXTRA_CFLAGS) -j$(CPU_CORES); \

clean:
	make -C $(KERNEL_PATH_FIRST) M=$(PWD) clean; \
