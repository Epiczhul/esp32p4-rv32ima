# esp32p4-rv32ima
Run linux on various MCUs with the help of RISC-V emulator. This project uses [CNLohr's mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) RISC-V emulator core to run Linux on various MCUs such as ESP32P4. Although, only ESP32P4 is tested now.

## Why
- Just for fun

## What's missing if we want to run linux on ESP32P4
- A single RV32-IMAC cpu core, well, this can be solved by patching the linux kernel to remove the 'A' extension usage
- No MMU, well, this can be solved by using NOMMU, or by emulating Sv32 in the emulator (now implemented, see below)
- Not enough memory, only 16MB flash and 768KB sram, well, this can be solved by using the 32MB PSRAM chip. However,
- I don't know how to make ESP32P4 directly execute code on PSRAM like the ESP32S3 does. If anyone knows the howto, kindly tell me, I really appreciate it ;) Then we can directly run linux on ESP32P4!

So far, the idea solution is to use a RISC-V IMA emulator, and use the PSRAM as the emulator's main system memory.

## How it works
It uses one 32MB PSRAM chip as the system memory. On startup, it initializes the PSRAM, and load linux kernel Image(an initramfs is embedded which is used as rootfs) and device tree binary from flash to PSRAM, then start the booting.

## What's new
- **MMU support (Sv32)**: with `CONFIG_EMU_BOOT_SMODE` (default on) the kernel is booted in S-mode instead of M-mode. The emulator acts as the machine level firmware: two level Sv32 page table walks with a small TLB, sfence.vma, SBI 0.2 (BASE + TIME + legacy console putchar/getchar/shutdown), mideleg/medeleg delegation and a minimal PLIC. This is enough for a generic rv32 MMU linux kernel (6.6+), see `docs/guest-kernel.md` for the kernel config and busybox rootfs
- **serial input finally works**: console input (UART0 and/or USB serial JTAG) is fed through a 16550 rx ring buffer and raised as a PLIC interrupt, so the kernel tty driver gets every keystroke and a getty on ttyS0 gives a working login shell
- **performance**: the old 4KB software cache in front of the PSRAM is gone (the PSRAM on the P4 is already hardware cached, the software cache was pure overhead), guest memory is accessed directly, the interpreter is compiled with -O3 and placed in IRAM, output goes through a tx ring flushed in chunks instead of a printf per char, and bigger instruction batches per timer check. The software cache can be re-enabled with `CONFIG_EMU_SOFTWARE_CACHE` for comparison
- **host build**: `cd host && make` builds the emulator for your PC with the same core, handy for debugging guest images without hardware. `make test` runs a hand written Sv32/SBI self test

## Device tree
- `main/uc.dts` is the old NOMMU reference (the kernel Image in this repo has it built in)
- `main/uc-sv32.dts` is the MMU variant: sv32 mmu-type, PLIC node, 16550 rx interrupt, 16MB RAM. Compile it with `./scripts/build-dtb.sh` and flash the result to the `dtb` partition (0xF10000). Without a valid dtb partition the kernel falls back to its builtin dtb

## Difference from [tvlad1234's pico-rv32ima](https://github.com/tvlad1234/pico-rv32ima)
- esp32p4 VS rp2040
- MMU/Sv32 emulation, PLIC and SBI firmware layer
- no need sdcard (maybe implementing booting from sdcard in future)

## Requirements
- one ESP32-P4 development board. (this one was used to test this project: [Guition JC-ESP32P4-M3 DEV Board](https://www.surenoo.com/collections/258652/products/27872758?data_from=collection_detail))
- a usb-c cable to debug

## How to use
- build esp32p4-rv32ima with esp idf env ```idf.py build``` (set the `EMU_*` options in menuconfig first, they live under "RV32IMA emulator")
- flash using idf.py ```idf.py -p /dev/ttyUSB0 -b 921600 flash```
- flash Image using esptool just to make sure its flashed correctly ```esptool.py --chip esp32p4 -p /dev/ttyUSB0 -b 921600 write_flash 0x110000 main/Image```
- for an MMU kernel, flash the dtb too: ```esptool.py --chip esp32p4 -p /dev/ttyUSB0 -b 921600 write_flash 0xF10000 dtb.bin```

- In no less than 1 sec, Linux kernel messages starts printing on the serial console. The boot process from pressing reset button to linux shell takes about 17.9s (psram at 80mhz) with the old NOMMU image, faster with the new flags.

- serial input works now, both over UART0 and USB-CDC/JTAG. Type at the shell prompt.

for issues, discord: epiczhul
