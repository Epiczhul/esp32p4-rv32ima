# Host build of the emulator, for development and testing on a PC.
#
#   make            embedded Image, M-mode boot (the old nommu kernel)
#   make smode      same, but booting in S-mode
#   make test       build + run the Sv32/SBI self test (see docs/guest-kernel.md)
#
# EMU_GUEST=kernel.bin ./emu-smode   load a guest image from a file
# EMU_DTB=uc-sv32.dtb ./emu-smode    load a dtb from a file

CC ?= gcc
CFLAGS ?= -O2 -g -I. -I../main -Wall
SMODEFLAGS = -DCONFIG_EMU_BOOT_SMODE -DCONFIG_EMU_RAM_MB=16

all: emu

emu: ../main/uc-rv32ima.c ../main/port.h ../main/mini-rv32ima.h ../main/image.S ../main/Image host-main.c sdkconfig.h
	$(CC) $(CFLAGS) -o $@ ../main/uc-rv32ima.c host-main.c ../main/image.S

smode: ../main/uc-rv32ima.c host-main.c sdkconfig.h ../main/image.S ../main/Image ../main/mini-rv32ima.h
	$(CC) $(CFLAGS) $(SMODEFLAGS) -o emu-smode ../main/uc-rv32ima.c host-main.c ../main/image.S

test: ../main/uc-rv32ima.c host-main.c sdkconfig.h make-test.py ../main/mini-rv32ima.h
	python3 make-test.py
	$(CC) $(CFLAGS) $(SMODEFLAGS) -o emu-test ../main/uc-rv32ima.c host-main.c ../main/image.S
	@EMU_GUEST=../main/test-sv32.bin ./emu-test < /dev/null 2>/dev/null | tr -d '\n' | tail -c 40

clean:
	rm -f emu emu-smode emu-test

.PHONY: all clean smode test
