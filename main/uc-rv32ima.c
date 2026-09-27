/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * Use some code of mini-rv32ima.c from https://github.com/cnlohr/mini-rv32ima
 * Copyright 2022 Charles Lohr
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "sdkconfig.h"

#include "port.h"
#ifdef CONFIG_EMU_SOFTWARE_CACHE
#include "cache.h"
#endif
#include "psram.h"

static int trap_nesting_level = 0;
static uint32_t last_trap_pc[10] = {0};
static int trap_pc_idx = 0;
static uint32_t ram_amt = CONFIG_EMU_RAM_MB * 1024 * 1024;

struct MiniRV32IMAState;
void DumpState(struct MiniRV32IMAState *core);
static uint32_t HandleException(uint32_t ir, uint32_t retval);
static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy );
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value);
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno);
static void MiniSleep();

#define MINIRV32WARN(x...) printf(x);
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval) \
{ \
	if (retval > 0) { \
		/* Track trap nesting */ \
		trap_nesting_level++; \
		last_trap_pc[trap_pc_idx++ % 10] = pc; \
		\
		/* Detect runaway recursion */ \
		if (trap_nesting_level > 10) { \
			printf("\n[FATAL] Trap nesting level = %d\n", trap_nesting_level); \
			printf("Last 10 trap PCs: "); \
			for (int i = 0; i < 10; i++) { \
				printf("0x%08" PRIx32 " ", last_trap_pc[i]); \
			} \
			printf("\n"); \
			printf("This indicates recursive exception handling.\n"); \
			printf("Likely causes:\n"); \
			printf("1. Exception handler accessing invalid memory\n"); \
			printf("2. Exception handler using unimplemented instruction\n"); \
			printf("3. UART/console output failing in panic handler\n"); \
			DumpState(state); \
			trap_nesting_level = 0; \
			return 0x5555; \
		} \
		\
		static int trap_count = 0; \
		if (trap_count++ < 20) { \
			printf("[TRAP #%d nest=%d] code=%" PRIu32 " PC=0x%08" PRIx32 " sp=0x%08" PRIx32 "\n", \
			trap_count, trap_nesting_level, retval, pc, state->regs[2]); \
		} \
		\
		retval = HandleException(ir, retval); \
		trap_nesting_level--; \
	} \
}

#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value) HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value) value = HandleOtherCSRRead(image, csrno);

#ifdef CONFIG_EMU_SOFTWARE_CACHE
#define MINIRV32_CUSTOM_MEMORY_BUS
static void MINIRV32_STORE4(uint32_t ofs, uint32_t val)
{
	cache_write(ofs, &val, 4);
}

static void MINIRV32_STORE2(uint32_t ofs, uint16_t val)
{
	cache_write(ofs, &val, 2);
}

static void MINIRV32_STORE1(uint32_t ofs, uint8_t val)
{
	cache_write(ofs, &val, 1);
}

static uint32_t MINIRV32_LOAD4(uint32_t ofs)
{
	uint32_t val;
	cache_read(ofs, &val, 4);
	return val;
}

static uint16_t MINIRV32_LOAD2(uint32_t ofs)
{
	uint16_t val;
	cache_read(ofs, &val, 2);
	return val;
}

static uint8_t MINIRV32_LOAD1(uint32_t ofs)
{
	uint8_t val;
	cache_read(ofs, &val, 1);
	return val;
}
#endif

/*
 * Firmware hooks the core calls out to.  With CONFIG_EMU_BOOT_SMODE the
 * misa value carries the S letter and the S-mode boot path is armed.
 */
#ifdef CONFIG_EMU_BOOT_SMODE
#define MINIRV32_MISA_VALUE 0x40441101 // IMA + S, XLEN=32
#endif

#define MINIRV32_SBI_ECALL( state ) MiniRV32SBICall( state )
#define MINIRV32_EXT_IRQ_PENDING( state ) plic_irq_pending()

static void MiniRV32SBICall(struct MiniRV32IMAState *state);
static int plic_irq_pending(void);

#include "mini-rv32ima.h"

void DumpState(struct MiniRV32IMAState *core)
{
	unsigned int pc = core->pc;
	unsigned int *regs = (unsigned int *)core->regs;

	printf("PC: %08x ", pc);
	printf("Z:%08x ra:%08x sp:%08x gp:%08x tp:%08x t0:%08x t1:%08x t2:%08x s0:%08x s1:%08x a0:%08x a1:%08x a2:%08x a3:%08x a4:%08x a5:%08x ",
		   regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
		regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15] );
	printf("a6:%08x a7:%08x s2:%08x s3:%08x s4:%08x s5:%08x s6:%08x s7:%08x s8:%08x s9:%08x s10:%08x s11:%08x t3:%08x t4:%08x t5:%08x t6:%08x\n",
		   regs[16], regs[17], regs[18], regs[19], regs[20], regs[21], regs[22], regs[23],
		regs[24], regs[25], regs[26], regs[27], regs[28], regs[29], regs[30], regs[31] );
}

struct MiniRV32IMAState core;

/*
 * S-mode firmware pieces: SBI calls from the kernel, a small PLIC for the
 * uart rx interrupt, and the 16550 rx side.  With CONFIG_EMU_BOOT_SMODE the
 * kernel runs in S-mode and expects the emulator to act as the machine level
 * firmware, same delegation split as OpenSBI uses.
 */
static void uart_poll_input(void);
static void console_flush(void);

static volatile int sbi_shutdown = 0;

/* 16550 register state, needed up here by the PLIC line logic */
static uint8_t uart_scratch = 0;
static uint8_t uart_ier = 0;
static uint16_t uart_divisor = 0;
static uint8_t uart_lcr = 0;
static uint8_t uart_mcr = 0;
static uint8_t uart_fcr = 0;

/* 16550 rx ring, filled from the console by uart_poll_input() */
#define UART_RXBUF_SZ 256
static uint8_t uart_rxbuf[UART_RXBUF_SZ];
static volatile uint16_t uart_rx_head = 0;
static volatile uint16_t uart_rx_tail = 0;
static volatile int uart_in_service = 0;

static int uart_rx_avail(void)
{
	return uart_rx_head != uart_rx_tail;
}

static void uart_rx_push(uint8_t c)
{
	uint16_t next = (uart_rx_head + 1) % UART_RXBUF_SZ;

	if (next == uart_rx_tail)
		return; // full, drop the char
	uart_rxbuf[uart_rx_head] = c;
	uart_rx_head = next;
}

static uint8_t uart_rx_pop(void)
{
	uint8_t c = uart_rxbuf[uart_rx_tail];

	uart_rx_tail = (uart_rx_tail + 1) % UART_RXBUF_SZ;
	return c;
}

static void uart_poll_input(void)
{
	uint8_t tmp[32];
	int n;

	while ((n = console_read_bytes(tmp, sizeof(tmp))) > 0) {
		for (int i = 0; i < n; i++)
			uart_rx_push(tmp[i]);
		if (uart_rx_head == uart_rx_tail)
			break; // ring full
	}
}

/* console tx ring, flushed in chunks instead of a printf per char */
#define CONSOLE_TXBUF_SZ 4096
static char console_txbuf[CONSOLE_TXBUF_SZ];
static volatile uint16_t console_tx_head = 0;
static volatile uint16_t console_tx_tail = 0;

static void console_tx_push(uint8_t c)
{
	uint16_t next = (console_tx_head + 1) % CONSOLE_TXBUF_SZ;

	if (next == console_tx_tail)
		console_flush(); // ring full, drain first
	console_txbuf[console_tx_head] = c;
	console_tx_head = next;
}

void console_flush(void)
{
	uint16_t used;
	char chunk[512];

	while (console_tx_head != console_tx_tail) {
		used = (console_tx_head - console_tx_tail) % CONSOLE_TXBUF_SZ;
		if (used > sizeof(chunk))
			used = sizeof(chunk);
		for (uint16_t i = 0; i < used; i++) {
			chunk[i] = console_txbuf[console_tx_tail];
			console_tx_tail = (console_tx_tail + 1) % CONSOLE_TXBUF_SZ;
		}
		console_write(chunk, used);
	}
}

/*
 * PLIC, source 10 is the uart, context 0 = M, context 1 = S.  Only the
 * uart line is wired up, level triggered, deasserted by draining the rx
 * ring (or by claiming while in service).
 */
#define PLIC_UART_SRC 10
static uint32_t plic_enable[2];
static uint32_t plic_threshold[2];
static uint32_t plic_priority[32];

static int plic_uart_line(void)
{
	return (uart_ier & 1) && uart_rx_avail();
}

static int plic_irq_pending(void)
{
	if (!plic_uart_line() || uart_in_service)
		return 0;
	if (!(plic_enable[1] & (1 << PLIC_UART_SRC)))
		return 0;
	return plic_priority[PLIC_UART_SRC] > plic_threshold[1];
}

static uint32_t plic_claim(int ctx)
{
	if (!plic_irq_pending())
		return 0;
	uart_in_service = 1;
	return PLIC_UART_SRC;
}

static void plic_complete(int ctx, uint32_t src)
{
	if (src == PLIC_UART_SRC)
		uart_in_service = 0;
}

static uint32_t plic_load(uint32_t addy)
{
	if (addy >= 0x0C000000 && addy < 0x0C001000)
		return plic_priority[(addy - 0x0C000000) >> 2];
	if (addy == 0x0C001000)
		return plic_uart_line() ? (1 << PLIC_UART_SRC) : 0;
	if (addy >= 0x0C002000 && addy < 0x0C002100)
		return plic_enable[(addy - 0x0C002000) >> 7];
	if (addy >= 0x0C200000 && addy < 0x0C204000) {
		int ctx = (addy >> 13) & 1;
		if ((addy & 0x1fff) == 0)
			return plic_threshold[ctx];
		if ((addy & 0x1fff) == 4)
			return plic_claim(ctx);
	}
	return 0;
}

static void plic_store(uint32_t addy, uint32_t val)
{
	if (addy >= 0x0C000000 && addy < 0x0C001000) {
		plic_priority[(addy - 0x0C000000) >> 2] = val;
		return;
	}
	if (addy >= 0x0C002000 && addy < 0x0C002100) {
		plic_enable[(addy - 0x0C002000) >> 7] = val;
		return;
	}
	if (addy >= 0x0C200000 && addy < 0x0C204000) {
		int ctx = (addy >> 13) & 1;
		if ((addy & 0x1fff) == 0) {
			plic_threshold[ctx] = val;
			return;
		}
		if ((addy & 0x1fff) == 4)
			plic_complete(ctx, val);
	}
}

static void MiniRV32SBICall(struct MiniRV32IMAState *state)
{
	uint32_t ext = state->regs[17]; // a7
	uint32_t fid = state->regs[16]; // a6
	int32_t error = 0;
	uint32_t value = 0;

	switch (ext) {
	case 0x10: // SBI base
		switch (fid) {
		case 0: value = 2; break; // spec version 0.2
		case 1: break; // implementation id
		case 2: break; // implementation version
		case 3: // probe extension
			if (state->regs[10] == 0x10 || state->regs[10] == 0x54494D45)
				value = 1;
			break;
		case 4: break; // mvendorid
		case 5: break; // marchid
		case 6: break; // mimpid
		default: error = -2; break;
		}
		break;
	case 0x54494D45: // TIME
		if (fid == 0) {
			uint64_t when = ((uint64_t)state->regs[11] << 32) | state->regs[10];

			core.timermatchl = (uint32_t)when;
			core.timermatchh = (uint32_t)(when >> 32);
		} else {
			error = -2;
		}
		break;
	case 0: // legacy set_timer
	{
		uint64_t when = ((uint64_t)state->regs[11] << 32) | state->regs[10];

		core.timermatchl = (uint32_t)when;
		core.timermatchh = (uint32_t)(when >> 32);
		break;
	}
	case 1: // legacy console_putchar
		console_tx_push(state->regs[10] & 0xff);
		break;
	case 2: // legacy console_getchar
		if (uart_rx_avail())
			state->regs[10] = uart_rx_pop();
		else
			state->regs[10] = -1;
		return;
	case 8: // legacy shutdown
		sbi_shutdown = 1;
		break;
	default:
		error = -2;
		break;
	}

	state->regs[10] = (uint32_t)error; // a0
	state->regs[11] = value;           // a1
}


void app_main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("psram init\n");

	if (psram_init() < 0) {
		printf("failed to init psram\n");
		return;
	}

	size_t psram_size = psram_get_size();
	if (ram_amt > psram_size) {
		printf("ram_amt %u exceeds psram, cutting to %zu\n",
			   ram_amt, psram_size);
		ram_amt = psram_size & ~0xFFFU;
	}

	printf("\nLoading kernel from flash...\n");

	restart:

	if (load_images(ram_amt, NULL) < 0)
		return;

	uint32_t dtb_addr = load_dtb(ram_amt);

	core.pc = MINIRV32_RAM_IMAGE_OFFSET;
	core.regs[10] = 0x00;
	core.regs[11] = dtb_addr;
#ifdef CONFIG_EMU_BOOT_SMODE
	core.extraflags = 1; // boot the kernel in S-mode
	core.mideleg = 0x222; // SSI, STI, SEI delegated to S-mode
	core.medeleg = 0xB1FF; // same exception split as OpenSBI
	core.satp = 0; // bare, the kernel brings up Sv32 itself
#else
	core.extraflags |= 3;
#endif

	uint64_t lastTime = GetTimeMicroseconds();
	int instrs_per_flip = 4096;
	printf("RV32IMA starting\n");
	printf("Initial PC: 0x%08" PRIx32 "\n", core.pc);

	int loop_count = 0;
	uint32_t last_pc = 0;
	uint32_t stuck_count = 0;
	uint32_t last_printed_pc = 0;
	int trace_enabled = 0;
	uint32_t wfi_pc = 0;
	bool in_wfi = false;

	while (1) {
		int ret;
		uint64_t *this_ccount = ((uint64_t*)&core.cyclel);
		uint64_t currentTime = GetTimeMicroseconds();
		uint32_t elapsedUs = (uint32_t)(currentTime - lastTime);

		if (elapsedUs > 100000) elapsedUs = 100000;
		lastTime = currentTime;

		/*
		* // Detect stuck PC
		* if (core.pc == last_pc) {
		*	stuck_count++;
		*
		*	if (stuck_count == 1000 && wfi_pc != core.pc) {
		*		printf("\n!!! PC STUCK at 0x%08" PRIx32 " for %" PRIu32 " iterations !!!\n",
		*			   core.pc, stuck_count);
		*		printf("This is NOT the WFI idle loop (WFI is at 0x%08" PRIx32 ")\n", wfi_pc);
		*		printf("Enabling instruction trace for next 20 steps...\n");
		*		trace_enabled = 20;
		*	}
		*	if (stuck_count == 5000 && wfi_pc != core.pc) {
		*		printf("\n!!! STILL STUCK after 5000 iterations !!!\n");
		*		uint32_t stuck_instr = MINIRV32_LOAD4(core.pc - MINIRV32_RAM_IMAGE_OFFSET);
		*		printf("Instruction at stuck PC: 0x%08" PRIx32 "\n", stuck_instr);
		*		printf("This might be a real hang, not just WFI\n");
		*		DumpState(&core);
		*		return;
		*	}
		*} else {
		*	if (stuck_count > 100 && in_wfi) {
		*		wfi_pc = last_pc;
		*		if (loop_count % 10000 == 0) {
		*			printf("WFI idle loop detected at 0x%08" PRIx32 " (normal)\n", wfi_pc);
		*		}
		*	}
		*	stuck_count = 0;
		*}
		*
		*/
		last_pc = core.pc;

		if (core.pc != last_printed_pc && core.pc != wfi_pc &&
		    (core.pc < 0x80000000 || core.pc > 0x81000000)) {
			printf("Unusual PC: 0x%08" PRIx32 "\n", core.pc);
			last_printed_pc = core.pc;
		}

		loop_count++;

		if (trace_enabled > 0) {
			uint32_t ofs = core.pc - MINIRV32_RAM_IMAGE_OFFSET;
			if (ofs < ram_amt) {
				uint32_t instr = 0;
				psram_read(ofs, &instr, 4);
				printf("[TRACE] PC=0x%08" PRIx32 " instr=0x%08" PRIx32 " sp=0x%08" PRIx32 "\n",
					   core.pc, instr, core.regs[2]);
			}
			trace_enabled--;
		}

		uart_poll_input();

		ret = MiniRV32IMAStep(&core, psram_get_base(), 0, elapsedUs, instrs_per_flip);

		console_flush();
		in_wfi = (ret == 1);

		if (sbi_shutdown) {
			printf("POWEROFF via SBI\n");
			DumpState(&core);
			return;
		}

		switch (ret) {
			case 0:
				break;
			case 1:
				MiniSleep();
				*this_ccount += instrs_per_flip;
				break;
			case 3:
				break;
			case 0x7777:
				goto restart;
			case 0x5555:
				printf("POWEROFF@0x%" PRIu32 "%"PRIu32"\n", core.cycleh, core.cyclel);
				DumpState(&core);
				return;
			default:
				printf("Unknown failure: ret=%d\n", ret);
				DumpState(&core);
				return;
		}
	}


	DumpState(&core);
}

static void MiniSleep(void)
{
	uart_poll_input();
	console_flush();
	usleep(10);
}

static int exception_count = 0;

static uint32_t HandleException(uint32_t ir, uint32_t code)
{
	exception_count++;

	const char *exception_names[] = {
		"Instr misaligned", "Instr fault", "Illegal instr", "Breakpoint",
		"Load misaligned", "Load fault", "Store misaligned", "Store fault",
		"Ecall U-mode", "Ecall S-mode", "Reserved", "Ecall M-mode",
		"Instr page fault", "Load page fault", "Reserved", "Store page fault"
	};

	uint32_t cause = code - 1;
	const char *cause_str = (cause < 16) ? exception_names[cause] : "Unknown";

	if (exception_count <= 15) {
		printf("[EXCEPTION #%d] %s (cause=%" PRIu32 ")\n", exception_count, cause_str, cause);
		printf("  PC=0x%08" PRIx32 " mepc=0x%08" PRIx32 " mcause=0x%08" PRIx32 " mtval=0x%08" PRIx32 "\n",
			   core.pc, core.mepc, core.mcause, core.mtval);
		printf("  sp=0x%08" PRIx32 " ra=0x%08" PRIx32 "\n", core.regs[2], core.regs[1]);

		if (cause == 3) {
			printf("  BREAKPOINT/BUG detected - kernel panic likely\n");
			printf("  Check a0-a2 for panic args: a0=0x%08" PRIx32 " a1=0x%08" PRIx32 " a2=0x%08" PRIx32 "\n",
				   core.regs[10], core.regs[11], core.regs[12]);
		}
	}

	return code;
}

static uint32_t HandleControlStore(uint32_t addy, uint32_t val)
{
    if (addy >= 0x0C000000 && addy < 0x0D000000) {
	plic_store(addy, val);
	return 0;
    }

    switch (addy) {
        case 0x10000000:
            if (uart_lcr & 0x80) uart_divisor = (uart_divisor & 0xFF00) | (val & 0xFF);
	    else console_tx_push(val);
            break;
        case 0x10000001:
            if (uart_lcr & 0x80) uart_divisor = (uart_divisor & 0x00FF) | ((val & 0xFF) << 8);
            else uart_ier = val;
            break;
        case 0x10000002: uart_fcr = val; break;
        case 0x10000003: uart_lcr = val; break;
        case 0x10000004: uart_mcr = val; break;
        case 0x10000007: uart_scratch = val; break;
    }
    return 0;
}



static uint32_t HandleControlLoad(uint32_t addy)
{
    if (addy >= 0x0C000000 && addy < 0x0D000000)
	return plic_load(addy);

    switch (addy) {
        case 0x10000000:
            if (uart_lcr & 0x80) return uart_divisor & 0xFF;
	    return uart_rx_avail() ? uart_rx_pop() : 0;
        case 0x10000001:
            if (uart_lcr & 0x80) return (uart_divisor >> 8) & 0xFF;
            return uart_ier;
	case 0x10000002: // IIR, rx interrupt pending when the ring has data
	    if ((uart_ier & 1) && uart_rx_avail()) return 0x04;
	    return 0xC1;
        case 0x10000003: return uart_lcr;
        case 0x10000004: return uart_mcr;
	case 0x10000005: return 0x60 | (uart_rx_avail() ? 1 : 0);
        case 0x10000006: return 0x00;
        case 0x10000007: return uart_scratch;
    }
    return 0;
}
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value)
{
	uint32_t ptrstart, ptrend;

	if (csrno != 0x136 && csrno != 0x137 && csrno != 0x138 &&
		csrno != 0x139 && csrno != 0x3a0 && csrno != 0x3b0 && csrno != 0xf14) {
		static int csr_write_count = 0;
	if (csr_write_count++ < 20) {
		printf("[CSR_WRITE] csr=0x%03x value=0x%08" PRIx32 "\n", csrno, value);
	}
		}

		switch (csrno) {
			case 0x136:
				printf("%d", (int)value);
				fflush(stdout);
				break;
			case 0x137:
				printf("%08" PRIx32, value);
				fflush(stdout);
				break;
			case 0x138:
				ptrstart = value - MINIRV32_RAM_IMAGE_OFFSET;
				ptrend = ptrstart;
				if (ptrstart >= ram_amt)
					printf("DEBUG PASSED INVALID PTR (%"PRIu32")\n", value);
			while (ptrend < ram_amt) {
				uint8_t c = MINIRV32_LOAD1(ptrend);
				if (c == 0)
					break;
				fwrite(&c, 1, 1, stdout);
				ptrend++;
			}
			break;
			case 0x139:
				putchar(value);
				fflush(stdout);
				break;
			default:
				break;
		}
}

static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno)
{
	int32_t result = 0;

	if (csrno != 0x140 && csrno != 0xC00 && csrno != 0x3a0 &&
		csrno != 0x3b0 && csrno != 0xf14 && result == 0) {
		static int csr_warn_count = 0;
	if (csr_warn_count++ < 20) {
		printf("[CSR_READ] Unhandled csr=0x%03x -> 0x%08" PRIx32 "\n",
			   csrno, (uint32_t)result);
	}
		}

		return result;
}
