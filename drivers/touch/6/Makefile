VERSION_DIGIT=$(shell ls /usr/lib/modules/ | cut -c 1)
VERSION_DIGIT_SECONDE=$(shell ls /usr/lib/modules/ | cut -c 3)

# 如果VERSION_DIGIT等于6且VERSION_DIGIT_SECONDE大于5，则说明内核版本大于6.5，则使用obj-y += 6.6/	

ifeq ($(shell test $(VERSION_DIGIT) -eq 6 && test $(VERSION_DIGIT_SECONDE) -gt 5 && echo ok), ok)
obj-m +=  ads7846-6.6.o
obj-m +=  cts128.o
else
obj-m +=  ads7846.o
endif

