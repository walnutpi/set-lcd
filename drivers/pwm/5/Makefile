ifeq ($(BOARD_MODEL),walnutpi-1b)
# walnutpi-1b 不需要此驱动
else ifeq ($(BOARD_MODEL),walnutpi-2b)
obj-m += gp7101.o
else ifeq ($(BOARD_MODEL),cybercam)
# cybercam 暂不使用此驱动
else
# 未知板子，全量编译
obj-m += gp7101.o
endif
