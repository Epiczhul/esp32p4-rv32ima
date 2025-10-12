# esp32p4-rv32ima
Run linux on various MCUs with the help of RISC-V emulator. This project uses [CNLohr's mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) RISC-V emulator core to run Linux on various MCUs such as ESP32P4. Although, only ESP32P4 is tested now.

## Why
- Just for fun

## What's missing if we want to run linux on ESP32P4
- A single RV32-IMC cpu core, well, this can be solved by patching the linux kernel to remove the 'A' extension usage
- No MMU, well, this can be solved by using NOMMU
- No enough memory, only 16MB flash and 768KB sram, well, this can be solved by using the 32MB PSRAM chip. However,
- I don't know how to make ESP32P4 directly execute code on PSRAM like the ESP32S3 does. If anyone knows the howto, kindly tell me, I really appreciate it;) Then we can directly run linux on ESP32P4!

So far, the idea solution is to use a RISC-V IMA emulator, and use the PSRAM as the emulator's main system memory.

## How it works
It uses one 32MB PSRAM chip as the system memory. On startup, it initializes the PSRAM, and load linux kernel Image(an initramfs is embedded which is used as rootfs) and device tree binary from flash to PSRAM, then start the booting.

- To improve the performance, a simple cache is implemented, it turns out we acchieved 95.1% cache hit during linux booting:
    - 4KB cache
    - two way set associative
    - 64B cacheline
    - LRU

## Difference from [tvlad1234's pico-rv32ima](https://github.com/tvlad1234/pico-rv32ima)
- esp32p4 VS rp2040
- a simple cache mechanism is implemented, thus much better performance
- no need sdcard (maybe implementing booting from sdcard in future)

## Requirements
- one ESP32-P4 development board. (this one was used to test this project: [Guition JC-ESP32P4-M3 DEV Board](https://www.surenoo.com/collections/258652/products/27872758?data_from=collection_detail))
- a usb-c cable to debug

## How to use
- build esp32p4-rv32ima with esp idf env ```idf.py build```
- flash using idf.py ```idf.py -p /dev/ttyUSB0 -b 921600 flash```
- flash Image using esptool just to make sure its flashed correctly ```esptool.py --chip esp32p4 -p /dev/ttyUSB0 -b 921600 write_flash 0x110000 main/Image```

- In no less than 1 sec, Linux kernel messages starts printing on the USB CDC console. The boot process from pressing reset button to linux shell takes about 17.9s (psram at 80mhz).


- still couldnt get command input working... uart wont work for input, only output.

