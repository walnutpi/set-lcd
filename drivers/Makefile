

VERSION=$(shell uname -r)
VERSION_DIGIT=$(shell echo $(VERSION) | cut -c 1)
EXTRA_CFLAGS=-DVERSION_DIGIT=$(VERSION_DIGIT)

obj-y += lcd/
obj-y += touch/

KERNEL_PATH := /usr/lib/modules/$(VERSION)/build/

PWD := $(shell pwd)

all:
	make -C $(KERNEL_PATH) M=$(PWD) modules EXTRA_CFLAGS=$(EXTRA_CFLAGS)
clean:
	make -C $(KERNEL_PATH) M=$(PWD) clean
