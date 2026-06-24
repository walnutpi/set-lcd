ifeq ($(BOARD_MODEL),walnutpi-1b)
obj-m +=  ads7846.o
else ifeq ($(BOARD_MODEL),walnutpi-2b)
obj-y +=  gt9xx/
obj-m +=  wpi-jd9366-touch.o
obj-m +=  ads7846.o
obj-m +=  rpi-ft5x06.o
else ifeq ($(BOARD_MODEL),cybercam)
# cybercam 在 kernel 5 下不需要触摸驱动
else
# 未知板子，全量编译
obj-y +=  gt9xx/
obj-m +=  wpi-jd9366-touch.o
obj-m +=  ads7846.o
obj-m +=  rpi-ft5x06.o
endif



