# Building the MTD-enabled xenon kernel

```
curl -O https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.11.tar.xz
tar xf linux-6.18.11.tar.xz && cd linux-6.18.11
git apply <libxenon>/linux-kernel-xbox360/patch-6.18-xenon0.30.diff
git apply <this repo>/kernel-patches/0001-xenon-remove-xenon_nand-stub.patch
git apply <this repo>/kernel-patches/0002-xenon-mark-interrupt-controller-populated.patch
cp <running kernel .config> .config      # then merge <this repo>/kernel-patches/mtd.config
export ARCH=powerpc CROSS_COMPILE=<prefix>   # e.g. xenon-, gcc 16.2.0 works
make olddefconfig && make -j2 zImage modules
cp arch/powerpc/boot/zImage.xenon <tftp dir>/vmlinuz-linux-xenon
```
Host needs flex, bison, bc, perl and libelf.
Build the modules of this repo against that tree with `make kmods KTREE=<kernel tree> KCROSS=<prefix>`.

Console: `insmod xenon_sfcx.ko [allow_write=1]`, `insmod xenon_flashfs.ko`, then
`mount -t flashfs /dev/xenonflash0 /mnt` (eMMC) or `mount -t flashfs xenon-nand-raw /mnt` (NAND);
add `-o write` for read-write. See docs/console-testing.md.
