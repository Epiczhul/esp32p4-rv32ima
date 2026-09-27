#!/bin/sh
# compile uc-sv32.dts into a flashable dtb blob (needs dtc)
set -e

cd "$(dirname "$0")/.."
dtc -I dts -O dtb -o dtb.bin main/uc-sv32.dts
echo "dtb.bin written, flash it to the dtb partition:"
echo "  esptool.py --port /dev/ttyUSB0 write_flash 0xF10000 dtb.bin"
