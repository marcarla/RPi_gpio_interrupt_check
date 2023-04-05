LINUX ?= /lib/modules/`uname -r`/build

all:
	$(MAKE) -C $(LINUX) M=`pwd` modules

clean:
	rm -rf *.o *.ko *~ core .depend *.mod.c *.cmd .*.cmd .tmp_versions
	
obj-m += irqdes.o
