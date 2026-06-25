ifeq ($(BOARD_MODEL),walnutpi-1b)
# walnutpi-1b 不需要此驱动
else ifeq ($(BOARD_MODEL),walnutpi-2b)
# walnutpi-2b 不需要此驱动
else ifeq ($(BOARD_MODEL),cybercam)
obj-m +=  qma6100.o
else
# 未知板子，全量编译
obj-m +=  qma6100.o
endif
