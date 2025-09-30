KERNEL_ROOT=/lib/modules/$(shell uname -r)/build

PWD=$(shell pwd)

obj-m := tcam-vdo.o tcam-raw.o
# obj-m := tcam-vdo.o

all:
	make -C $(KERNEL_ROOT) M=$(PWD) modules

clean:
	make -C $(KERNEL_ROOT) M=$(PWD) clean
