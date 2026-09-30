/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "hal/usb_serial_jtag_ll.h"
#include "psram.h"
#include "port.h"

/*
 * Executable memory for the block JIT.  The code buffer comes out of the
 * internal instruction RAM, every finished block gets flushed from the
 * data side of the cache so instruction fetch sees it.
 */
void *jit_exec_alloc(int bytes)
{
        return heap_caps_malloc(bytes, MALLOC_CAP_EXEC);
}

void jit_exec_commit(void *code, int bytes)
{
        esp_cache_msync(code, bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_INSTRAM);
}

uint64_t GetTimeMicroseconds()
{
        return esp_timer_get_time();
}

int ReadKBByte(void)
{
        uint8_t rxchar;
        int rread;

        rread = usb_serial_jtag_ll_read_rxfifo(&rxchar, 1);

        if (rread > 0)
                return rxchar;
        else
                return -1;
}

int IsKBHit(void)
{
        return usb_serial_jtag_ll_rxfifo_data_available();
}

/*
 * Console input, drains the usb serial jtag fifo and the console uart in
 * one go.  Both are non blocking, whoever has data wins.
 */
static int uart0_installed = 0;

int console_read_bytes(uint8_t *buf, int len)
{
        int n = 0;
#if CONFIG_EMU_CONSOLE_USB_JTAG
        n = usb_serial_jtag_ll_read_rxfifo(buf, len);
#endif
#if CONFIG_EMU_CONSOLE_UART0
        if (n == 0) {
                if (!uart0_installed) {
                        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0);
                        uart0_installed = 1;
                }
                int r = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, len, 0);
                if (r > 0)
                        n = r;
        }
#endif
        return n;
}

void console_write(const char *buf, int len)
{
        fwrite(buf, 1, len, stdout);
}

static uint8_t *psram_base = NULL;
static size_t psram_size = 0;

int psram_init(void)
{
        size_t available_psram;
        size_t alloc_size;

        available_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        if (available_psram == 0) {
                printf("ERROR: No PSRAM available!\n");
                printf("Check menuconfig: Component config -> ESP PSRAM\n");
                return -1;
        }

        printf("Available PSRAM: %zu bytes (%.2f MB)\n",
                   available_psram, available_psram / (1024.0 * 1024.0));

        alloc_size = (available_psram * 9) / 10;

        printf("Attempting to allocate %zu bytes (%.2f MB)...\n",
                   alloc_size, alloc_size / (1024.0 * 1024.0));

        psram_base = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);

        if (psram_base == NULL) {
                printf("ERROR: Failed to allocate PSRAM!\n");
                return -1;
        }

        psram_size = alloc_size;
        printf("SUCCESS: PSRAM allocated at %p, size: %zu bytes (%.2f MB)\n",
                   psram_base, psram_size, psram_size / (1024.0 * 1024.0));

        printf("Initializing PSRAM to zero...\n");
        memset(psram_base, 0, psram_size);
        printf("PSRAM initialized successfully!\n");

        return 0;
}

int psram_read(uint32_t addr, void *buf, int len)
{
        if (psram_base == NULL) {
                printf("ERROR: psram_read called before psram_init!\n");
                return -1;
        }

        if (addr + len > psram_size) {
                printf("ERROR: psram_read out of bounds: addr=0x%lx, len=%d, size=%zu\n",
                           (unsigned long)addr, len, psram_size);
                return -1;
        }

        memcpy(buf, psram_base + addr, len);
        return len;
}

int psram_write(uint32_t addr, void *buf, int len)
{
        if (psram_base == NULL) {
                printf("ERROR: psram_write called before psram_init!\n");
                return -1;
        }

        if (addr + len > psram_size) {
                printf("ERROR: psram_write out of bounds: addr=0x%lx, len=%d, size=%zu\n",
                           (unsigned long)addr, len, psram_size);
                return -1;
        }

        memcpy(psram_base + addr, buf, len);
        return len;
}

void *psram_get_base(void)
{
        return psram_base;
}

size_t psram_get_size(void)
{
        return psram_size;
}

void verify_kernel_header(void)
{
        uint8_t header[64];

        printf("\n=== Verifying Kernel Header ===\n");
        psram_read(0, header, 64);

        printf("First 64 bytes of loaded kernel:\n");
        for (int i = 0; i < 64; i++) {
                printf("%02x ", header[i]);
                if ((i + 1) % 16 == 0) printf("\n");
        }
        printf("\n");

        // Check RISC-V magic
        if (header[0x30] == 'R' && header[0x31] == 'I' &&
                header[0x32] == 'S' && header[0x33] == 'C' &&
                header[0x34] == 'V') {
                printf("✓ RISCV magic found at offset 0x30\n");
                } else {
                        printf("✗ RISCV magic NOT found! Expected at 0x30\n");
                }

                uint32_t first_instr = *(uint32_t*)header;
        printf("First instruction: 0x%08lx\n", (unsigned long)first_instr);
        printf("Expected: 0x05c0006f (j 0x5c)\n\n");
}

int load_dtb(uint32_t ram_size)
{
        const esp_partition_t *dtb_partition;
        esp_err_t err;
        uint32_t addr;
        char dmabuf[64];
        size_t remaining;
        uint8_t hdr[4];
        uint32_t guest_addr = 0x80000000 + ram_size - 0x10000;

        dtb_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                                           ESP_PARTITION_SUBTYPE_ANY,
                                                                                           "dtb");
        if (dtb_partition == NULL) {
                printf("no dtb partition, kernel falls back to its builtin dtb\n");
                return 0;
        }

        err = esp_partition_read(dtb_partition, 0, hdr, 4);
        if (err != ESP_OK || hdr[0] != 0xd0 || hdr[1] != 0x0d ||
                hdr[2] != 0xfe || hdr[3] != 0xed) {
                printf("dtb partition has no valid fdt, using builtin dtb\n");
                return 0;
        }

        printf("Found dtb partition, %zu bytes\n", dtb_partition->size);

        addr = 0;
        remaining = dtb_partition->size;

        while (remaining >= 64) {
                err = esp_partition_read(dtb_partition, addr, dmabuf, 64);
                if (err != ESP_OK) {
                        printf("ERROR: Failed to read dtb at offset %lu: %s\n",
                                   (unsigned long)addr, esp_err_to_name(err));
                        return 0;
                }
                psram_write((ram_size - 0x10000) + addr, dmabuf, 64);
                addr += 64;
                remaining -= 64;
        }

        if (remaining > 0) {
                err = esp_partition_read(dtb_partition, addr, dmabuf, remaining);
                if (err != ESP_OK) {
                        printf("ERROR: Failed to read remaining dtb bytes: %s\n",
                                   esp_err_to_name(err));
                        return 0;
                }
                psram_write((ram_size - 0x10000) + addr, dmabuf, remaining);
        }

        printf("dtb loaded at 0x%08" PRIx32 "\n", guest_addr);
        return guest_addr;
}

int load_images(int ram_size, int *kern_len)
{
        const esp_partition_t *kernel_partition;
        esp_err_t err;
        uint32_t addr;
        char dmabuf[64];
        size_t partition_size;

        printf("\n=== Loading Kernel from Flash ===\n");

        kernel_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                                                ESP_PARTITION_SUBTYPE_ANY,
                                                                                         "kernel");
        if (kernel_partition == NULL) {
                printf("ERROR: 'kernel' partition not found!\n");
                printf("Make sure partition table has 'kernel' partition\n");
                return -1;
        }

        partition_size = kernel_partition->size;
        printf("Found kernel partition:\n");
        printf("  Label: %s\n", kernel_partition->label);
        printf("  Address: 0x%lx\n", (unsigned long)kernel_partition->address);
        printf("  Size: %zu bytes (%.2f MB)\n", partition_size,
                   partition_size / (1024.0 * 1024.0));

        if (partition_size > ram_size) {
                printf("WARNING: Partition size (%zu) > RAM size (%d)\n",
                           partition_size, ram_size);
                printf("Will only load first %d bytes\n", ram_size);
                partition_size = ram_size;
        }

        if (partition_size > psram_get_size()) {
                printf("WARNING: Partition size (%zu) > PSRAM size (%zu)\n",
                           partition_size, psram_get_size());
                partition_size = psram_get_size();
        }

        if (kern_len)
                *kern_len = partition_size;

        printf("\nLoading kernel from flash to PSRAM...\n");
        printf("This will take a moment...\n");

        addr = 0;
        size_t remaining = partition_size;

        while (remaining >= 64) {
                err = esp_partition_read(kernel_partition, addr, dmabuf, 64);
                if (err != ESP_OK) {
                        printf("\nERROR: Failed to read from flash at offset %lu: %s\n",
                                   (unsigned long)addr, esp_err_to_name(err));
                        return -1;
                }

                psram_write(addr, dmabuf, 64);
                addr += 64;
                remaining -= 64;

                if ((addr % (64 * 1024)) == 0) {
                        printf(".");
                        fflush(stdout);
                }
        }

        if (remaining > 0) {
                err = esp_partition_read(kernel_partition, addr, dmabuf, remaining);
                if (err != ESP_OK) {
                        printf("\nERROR: Failed to read remaining bytes: %s\n",
                                   esp_err_to_name(err));
                        return -1;
                }
                psram_write(addr, dmabuf, remaining);
        }

        printf("\n✓ Kernel loaded successfully from flash!\n");
        printf("Total loaded: %zu bytes (%.2f MB)\n",
                   partition_size, partition_size / (1024.0 * 1024.0));

        verify_kernel_header();

        return 0;
}
