VERSION=$(shell uname -r)
VERSION_DIGIT=$(shell echo $(VERSION) | cut -c 1)

EXTRA_CFLAGS=-DVERSION_DIGIT=$(VERSION_DIGIT)

obj-m :=  wpi_fb_st7789_15.o

# KERNEL_PATH := /usr/lib/modules/6.1.21+/build/
KERNEL_PATH := /usr/lib/modules/$(VERSION)/build/

PWD := $(shell pwd)

all:
	make -C $(KERNEL_PATH) M=$(PWD) modules EXTRA_CFLAGS=$(EXTRA_CFLAGS)
	make -C framebuffer-$(VERSION_DIGIT)/

clean:
	make -C $(KERNEL_PATH) M=$(PWD) clean
	make -C framebuffer-$(VERSION_DIGIT)/ clean

output:
	make -C framebuffer-$(VERSION_DIGIT)/ output
	mkdir -p ../drivers-save/$(VERSION)/
	cp *.ko ../drivers-save/$(VERSION)/
