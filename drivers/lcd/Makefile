VERSION=$(shell uname -r)
VERSION_DIGIT=$(shell echo $(VERSION) | cut -c 1)

obj-y += fbtft-$(VERSION_DIGIT)/
obj-m +=  wpi_fb_st7796.o
obj-m +=  wpi_fb_st7789_15.o

