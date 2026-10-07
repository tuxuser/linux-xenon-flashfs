CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
PPC_CC ?= powerpc64-linux-gnu-gcc
PPC_SYSROOT ?=
PPC_CFLAGS ?= -m32 -mcpu=powerpc -mno-altivec -fno-link-libatomic $(if $(PPC_SYSROOT),--sysroot=$(PPC_SYSROOT)) $(CFLAGS)
KTREE ?=
KCROSS ?=

INC := -Ilib -Ihw -Icli
LIB := $(wildcard lib/*.c)
DEPS := $(wildcard lib/*.h hw/*.h cli/*.h)
TOOLS := emmc_probe mtd_dump mtd_nand

.PHONY: all cross test kmods clean
all: flashfs

kmods:
	@test -n "$(KTREE)" -a -n "$(KCROSS)" || { echo "set KTREE= and KCROSS="; exit 1; }
	$(MAKE) -C $(KTREE) ARCH=powerpc CROSS_COMPILE=$(KCROSS) M=$(CURDIR)/kmod modules

clean:
	-$(MAKE) -C $(KTREE) M=$(CURDIR)/kmod clean
