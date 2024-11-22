VERSION=$(shell uname -r)
VERSION_DIGIT=$(shell echo $(VERSION) | cut -c 1)

EXTRA_CFLAGS=-DVERSION_DIGIT=$(VERSION_DIGIT)

obj-y += fbtft-$(VERSION_DIGIT)/
obj-m +=  wpi_fb_st7796.o
obj-m +=  wpi_fb_st7789_15.o

KERNEL_PATH := /usr/lib/modules/$(VERSION)/build/

PWD := $(shell pwd)

all:
	make -C $(KERNEL_PATH) M=$(PWD) modules EXTRA_CFLAGS=$(EXTRA_CFLAGS)

clean:
	make -C $(KERNEL_PATH) M=$(PWD) clean

output:
	cp fbtft-$(VERSION_DIGIT)/*.ko  ./
