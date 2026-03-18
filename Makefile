KDIR ?= /home/liu/桌面/linux-6.6.6
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-
USER_CC ?= $(CROSS_COMPILE)gcc
HOST_CC ?= gcc

obj-m += t113_io_spi_drv.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) modules

user:
	$(USER_CC) -Wall -Wextra -O2 -o t113_io_spi_test t113_io_spi_test.c

user-host:
	$(HOST_CC) -Wall -Wextra -O2 -o t113_io_spi_test_host t113_io_spi_test.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) clean
	$(RM) t113_io_spi_test
	$(RM) t113_io_spi_test_host
