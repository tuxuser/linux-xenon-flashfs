# xenon-flashfs

```
lib/    flashfs core: parse, root discovery, writer (wr_emmc.c, wr_nand.c)
hw/     eMMC and NAND controller drivers (host + kernel)
tools/  console helpers
```

```
make cross                     PPC binaries
make kmods KTREE=<kernel> KCROSS=<prefix>   kernel modules
```

Make variables (all via cmdline, e.g. `make cross PPC_SYSROOT=/path`):
```
CC, CFLAGS      host build (default cc, -O2 -std=c11 -Wall -Wextra -Werror)
PPC_CC          PPC cross compiler (default powerpc64-linux-gnu-gcc)
PPC_SYSROOT     target sysroot, omitted if empty
PPC_CFLAGS      override all PPC flags
KTREE           configured+built kernel tree, required for kmods
KCROSS          kernel CROSS_COMPILE prefix, required for kmods (e.g. xenon-)
```

## Load

```
insmod xenon_sfcx.ko                 read-only
insmod xenon_sfcx.ko allow_write=1   writable (eMMC and NAND); does not persist, reload without it afterwards
insmod xenon_flashfs.ko              only needed for reading files
```

## Read the whole flash

eMMC (48 MiB logical flash, block device):
```
dd if=/dev/xenonflash0 bs=1M count=48 iflag=direct of=emmc.img
```

NAND (16 MiB, 32768 pages of 528 bytes = 512 data + 16 spare, 17,301,504 bytes):
```
mtd_dump /dev/mtd0ro 0 32768 > nand-raw.img
```

## Write the whole flash

eMMC:
```
insmod xenon_sfcx.ko allow_write=1
cat /sys/block/xenonflash0/ro        # 0
dd if=emmc.img of=/dev/xenonflash0 bs=16384 oflag=direct conv=notrunc
sync
```

NAND (per 16 KiB block: 32 raw records of 528 bytes; bad blocks are refused):
```
insmod xenon_sfcx.ko allow_write=1
cat /sys/class/mtd/mtd0/flags        # 0x400
for b in $(seq 0 1023); do
  mtd_nand /dev/mtd0 erase $b || continue
  dd if=nand-raw.img bs=16896 skip=$b count=1 2>/dev/null | mtd_nand /dev/mtd0 put_block $b
done
```

Then reload `xenon_sfcx` without `allow_write` and compare a fresh read with the image (`cmp`).

## Read files

```
insmod xenon_sfcx.ko
insmod xenon_flashfs.ko
mkdir -p /mnt/flash
# eMMC
mount -t flashfs /dev/xenonflash0 /mnt/flash
# NAND
mount -t flashfs xenon-nand-raw /mnt/flash
ls -l /mnt/flash                      flat directory, one file per entry
cat /mnt/flash/NAME
cp /mnt/flash/NAME /root/NAME         small files only
sha256sum /mnt/flash/NAME
umount /mnt/flash
```

Names are at most 22 characters, case-insensitive. The mount is read-only unless `-o write` is given; several read-only mounts of one device may coexist.
