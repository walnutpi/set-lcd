VERSION_DIGIT=$(shell ls /usr/lib/modules/ | cut -c 1)

ifeq ($(BOARD_MODEL),walnutpi-1b)
obj-y += fbtft-$(VERSION_DIGIT)/
obj-m +=  wpi_fb_st7796.o
obj-m +=  wpi_fb_st7789_15.o
else ifeq ($(BOARD_MODEL),walnutpi-2b)
obj-y += fbtft-$(VERSION_DIGIT)/
obj-m +=  wpi_fb_st7796.o
obj-m +=  wpi_fb_st7789_15.o
else ifeq ($(BOARD_MODEL),cybercam)
# cybercam 使用 DSI 屏幕，不需要 SPI LCD 驱动
else
# 未知板子，全量编译
obj-y += fbtft-$(VERSION_DIGIT)/
obj-m +=  wpi_fb_st7796.o
obj-m +=  wpi_fb_st7789_15.o
endif

