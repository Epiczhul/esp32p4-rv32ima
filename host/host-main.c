// Host build of the emulator, for development and testing on a PC.
// Not part of the ESP-IDF build.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <errno.h>

extern struct MiniRV32IMAState core;
extern void DumpState(struct MiniRV32IMAState *core);
extern void app_main(void);
extern char kernel_start[], kernel_end[];

static uint8_t *ram;
static size_t ram_size;
static int is_eofd = 0;

static void ResetKeyboardInput(void)
{
	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag |= ICANON | ECHO;
	tcsetattr(0, TCSANOW, &term);
}

static void CtrlC(int sig)
{
	DumpState(&core);
	ResetKeyboardInput();
	exit(0);
}

static void CaptureKeyboardInput(void)
{
	// non blocking stdin, the emulator polls it
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

	if (!isatty(0))
		return;

	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(0, TCSANOW, &term);
}

uint64_t GetTimeMicroseconds()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_usec + ((uint64_t)(tv.tv_sec)) * 1000000LL;
}

int ReadKBByte(void)
{
	uint8_t c;
	int r = read(0, &c, 1);

	if (r > 0)
		return c;
	return -1;
}

int IsKBHit(void)
{
	int byteswaiting = 0;

	if (is_eofd)
		return -1;
	ioctl(0, FIONREAD, &byteswaiting);
	return !!byteswaiting;
}

int console_read_bytes(uint8_t *buf, int len)
{
	if (is_eofd)
		return -1;
	int r = read(0, buf, len);
	if (r > 0)
		return r;
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;
	is_eofd = 1;
	return -1;
}

void console_write(const char *buf, int len)
{
	fwrite(buf, 1, len, stdout);
	fflush(stdout);
}

int psram_init(void)
{
	// guest ram lives in a plain malloc buffer on the host
	ram_size = 32 * 1024 * 1024;
	ram = malloc(ram_size);
	if (!ram) {
		fprintf(stderr, "no ram\n");
		return -1;
	}
	memset(ram, 0, ram_size);
	return 0;
}

int psram_read(uint32_t addr, void *buf, int len)
{
	if (addr + len > ram_size)
		return -1;
	memcpy(buf, ram + addr, len);
	return len;
}

int psram_write(uint32_t addr, void *buf, int len)
{
	if (addr + len > ram_size)
		return -1;
	memcpy(ram + addr, buf, len);
	return len;
}

void *psram_get_base(void)
{
	return ram;
}

size_t psram_get_size(void)
{
	return ram_size;
}

static int load_file(const char *path, uint32_t offset)
{
	FILE *f = fopen(path, "rb");

	if (!f)
		return -1;
	uint8_t tmp[4096];
	size_t n;
	uint32_t addr = offset;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
		psram_write(addr, tmp, n);
		addr += n;
	}
	fclose(f);
	printf("loaded %s at 0x%08x\n", path, offset);
	return 0;
}

int load_images(int ram_size, int *kern_len)
{
	const char *guest = getenv("EMU_GUEST");

	if (guest) {
		if (load_file(guest, 0) < 0) {
			fprintf(stderr, "cannot open %s\n", guest);
			return -1;
		}
	} else {
		psram_write(0, kernel_start, kernel_end - kernel_start);
	}
	if (kern_len)
		*kern_len = ram_size;
	return 0;
}

uint32_t load_dtb(uint32_t ram_size)
{
	const char *dtb = getenv("EMU_DTB");

	if (!dtb)
		return 0;
	uint8_t hdr[4];
	FILE *f = fopen(dtb, "rb");

	if (!f || fread(hdr, 1, 4, f) != 4 ||
	    hdr[0] != 0xd0 || hdr[1] != 0x0d || hdr[2] != 0xfe || hdr[3] != 0xed) {
		printf("no valid dtb at %s\n", dtb);
		if (f)
			fclose(f);
		return 0;
	}
	fclose(f);
	load_file(dtb, ram_size - 0x10000);
	return 0x80000000 + ram_size - 0x10000;
}

int main(int argc, char **argv)
{
	// only grab the terminal when it is actually a tty
	if (isatty(0)) {
		CaptureKeyboardInput();
		app_main();
		ResetKeyboardInput();
	} else {
		app_main();
	}
	return 0;
}
