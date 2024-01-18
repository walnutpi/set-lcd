obj-m :=  wpi_fbtft.o
wpi_fbtft-y += fbtft-core.o fbtft-sysfs.o fbtft-bus.o fbtft-io.o


KERNEL_PATH := /usr/lib/modules/$(shell uname -r)/build/

PWD := $(shell pwd)

all:
	make -C $(KERNEL_PATH) M=$(PWD) modules
clean:
	make -C $(KERNEL_PATH) M=$(PWD) clean

output:
	cp wpi_fbtft.ko ../