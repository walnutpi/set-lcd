CPU_CORES=$(shell nproc)

VERSION_DIGIT=$(shell ls /usr/lib/modules/ | cut -c 1)
VERSION_DIGIT_SECONDE=$(shell ls /usr/lib/modules/ | cut -c 3)
EXTRA_CFLAGS=-DVERSION_DIGIT=$(VERSION_DIGIT)
EXTRA_CFLAGS+=-DVERSION_DIGIT_SECONDE=$(VERSION_DIGIT_SECONDE)
CFLAGS_MODULE =-DVERSION_DIGIT=$(VERSION_DIGIT)
CFLAGS_MODULE +=-DVERSION_DIGIT_SECONDE=$(VERSION_DIGIT_SECONDE)

# 板级检测：优先读取 /tmp/walnutpi-board_model，否则从设备树获取
# 自动转小写并去除空白字符
BOARD_MODEL := $(shell if [ -f /tmp/walnutpi-board_model ]; then cat /tmp/walnutpi-board_model | tr -d '[:space:]' | tr 'A-Z' 'a-z'; elif [ -f /proc/device-tree/model ]; then tr -d '\0' < /proc/device-tree/model | tr -d '[:space:]' | tr 'A-Z' 'a-z'; fi)
export BOARD_MODEL

obj-y += lcd/
obj-y += touch/
obj-y += power/

# KERNEL_PATH_UNAME := /usr/lib/modules/$(VERSION)/build/
KERNEL_PATH_FIRST := $(shell ls -d /usr/lib/modules/*/build/ | head -n 1)

PWD := $(shell pwd)

all:
	make -C $(KERNEL_PATH_FIRST) M=$(PWD) modules EXTRA_CFLAGS='$(EXTRA_CFLAGS)' CFLAGS_MODULE='$(CFLAGS_MODULE)' -j$(CPU_CORES); \

clean:
	make -C $(KERNEL_PATH_FIRST) M=$(PWD) clean; \
