obj-m :=  fb_st7796.o

VERSION=$(shell uname -r)
VERSION_digit=$(shell echo $(VERSION) | cut -c 1)

# KERNEL_PATH := /usr/lib/modules/6.1.21+/build/
KERNEL_PATH := /usr/lib/modules/$(VERSION)/build/

PWD := $(shell pwd)



all:
	make -C $(KERNEL_PATH) M=$(PWD) modules
	make -C $(VERSION_digit)/

clean:
	make -C $(KERNEL_PATH) M=$(PWD) clean
	make -C $(VERSION_digit)/ clean

output:
	make -C $(VERSION_digit)/ output
	-rm  /lib/modules/$(VERSION)/kernel/drivers/staging/fbtft/fbtft*
	cp fbtft.ko /lib/modules/$(VERSION)/kernel/drivers/staging/fbtft/
	-rm  /lib/modules/$(VERSION)/kernel/drivers/staging/fbtft/fb_st7796*
	cp fb_st7796.ko /lib/modules/$(VERSION)/kernel/drivers/staging/fbtft/
	depmod -a