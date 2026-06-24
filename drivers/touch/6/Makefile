VERSION_DIGIT=$(shell ls /usr/lib/modules/ | cut -c 1)
VERSION_DIGIT_SECONDE=$(shell ls /usr/lib/modules/ | cut -c 3)

# 内核 >= 6.5 时使用 ads7846-6.6.o，否则使用 ads7846.o
ifeq ($(BOARD_MODEL),walnutpi-1b)
ifeq ($(shell test $(VERSION_DIGIT) -eq 6 && test $(VERSION_DIGIT_SECONDE) -gt 5 && echo ok), ok)
obj-m +=  ads7846-6.6.o
else
obj-m +=  ads7846.o
endif
else ifeq ($(BOARD_MODEL),walnutpi-2b)
ifeq ($(shell test $(VERSION_DIGIT) -eq 6 && test $(VERSION_DIGIT_SECONDE) -gt 5 && echo ok), ok)
obj-m +=  ads7846-6.6.o
else
obj-m +=  ads7846.o
endif
else ifeq ($(BOARD_MODEL),cybercam)
ifeq ($(shell test $(VERSION_DIGIT) -eq 6 && test $(VERSION_DIGIT_SECONDE) -gt 5 && echo ok), ok)
obj-m +=  cts128.o
else
# cts128 需要内核 >= 6.5
endif
else
# 未知板子，全量编译
ifeq ($(shell test $(VERSION_DIGIT) -eq 6 && test $(VERSION_DIGIT_SECONDE) -gt 5 && echo ok), ok)
obj-m +=  ads7846-6.6.o
obj-m +=  cts128.o
else
obj-m +=  ads7846.o
endif
endif

