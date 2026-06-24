ifeq ($(BOARD_MODEL),walnutpi-1b)
# walnutpi-1b 不需要此驱动
else ifeq ($(BOARD_MODEL),walnutpi-2b)
obj-m +=  rpi-panel-attiny-regulator.o
else ifeq ($(BOARD_MODEL),cybercam)
# cybercam 不需要此驱动
else
# 未知板子，全量编译
obj-m +=  rpi-panel-attiny-regulator.o
endif
