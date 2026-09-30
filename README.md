# esp32p4-rv32ima
Run linux on various MCUs with the help of RISC-V emulator. This project uses [CNLohr's mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) RISC-V emulator core to run Linux on various MCUs such as ESP32P4. Although, only ESP32P4 is tested now.

## Why
- Just for fun

## How to run Linux on the Esp32-P4?
So far, the ideal solution is to use a RISC-V IMA emulator, and use the PSRAM as the emulator's main system memory.

## How it works
It uses one 32MB PSRAM chip as the system memory. On startup, it initializes the PSRAM, and load linux kernel Image(an initramfs is embedded which is used as rootfs) and device tree binary from flash to PSRAM, then start the booting.

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

- In no less than 1 sec, Linux kernel messages starts printing on the serial console. The boot process from pressing reset button to linux shell takes about 9.28s.

- serial input works now, both over UART0 and USB-CDC/JTAG. Type at the shell prompt.

for issues, disccord: epiczhul or just open an issue here.
