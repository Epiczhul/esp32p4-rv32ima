# Building an MMU Linux guest (busybox)

The emulator boots the kernel in S-mode with Sv32 translation, SBI 0.2
(BASE + TIME + legacy console) and a PLIC wired to the 16550 rx interrupt.
Any mainline Linux 6.6+ works, the generic riscv platform supports rv32
with sv32 since then.

## What you need

- a riscv gcc that can target ilp32, e.g. `riscv64-unknown-linux-gnu-gcc`
  with multilib, or the bootlin `riscv32 glibc/musl` toolchains
- busybox built static against musl or uclibc-ng (glibc static also works)
- `dtc` for the device tree

## Kernel

```sh
make ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- \
     CC="riscv64-unknown-linux-gnu-gcc -march=rv32ima_zicsr_zifencei -mabi=ilp32" \
     defconfig
./scripts/config -e 32BIT -e NONPORTABLE -e MMU \
                 -d SMP -d CPU_FREQ -d FPU \
                 -e SERIAL_8250=y -e SERIAL_8250_CONSOLE=y \
                 -e RISCV_SBI=y \
                 -e BLK_DEV_INITRD
make ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- \
     CC="riscv64-unknown-linux-gnu-gcc -march=rv32ima_zicsr_zifencei -mabi=ilp32" \
     -j$(nproc)
```

Key config bits the emulator expects:

- `CONFIG_32BIT=y` + `CONFIG_NONPORTABLE=y` + `CONFIG_MMU=y`
- `CONFIG_SMP=n` (single hart)
- no FPU, the DTB advertises rv32ima only, the kernel stays soft-float
- `CONFIG_SERIAL_8250_CONSOLE=y` for the 16550 at 0x10000000
- `CONFIG_INITRAMFS_SOURCE="<path to busybox rootfs>"` to embed the shell

For an initramfs, a minimal busybox rootfs is enough:

```
/init        # #!/bin/sh, mount -t proc none /proc, exec /bin/sh
/bin/busybox + symlinks (sh, mount, ps, ...)
/dev/console
/etc/passwd
```

## Device tree

```sh
./scripts/build-dtb.sh
```

This compiles `main/uc-sv32.dts` to `dtb.bin`. The emulator loads it from
the `dtb` flash partition into the top of guest RAM and hands its address
to the kernel in a1. Without a valid dtb partition the kernel falls back
to whatever DTB is built into the image.

The DTB advertises 16MB of RAM at 0x80000000, keep `CONFIG_EMU_RAM_MB=16`
in menuconfig in sync (or edit both).

## Flash

```sh
idf.py build flash monitor
esptool.py --port /dev/ttyUSB0 write_flash 0x110000 Image 0xF10000 dtb.bin
```

## Serial input

Typed input reaches the guest through the 16550 rx interrupt (PLIC source
10), so a getty on ttyS0 gives a working login shell. Both UART0 and the
USB serial JTAG fifo are polled, see `EMU_CONSOLE_*` in menuconfig.
