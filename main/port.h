/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PORT_H
#define PORT_H

#include <stdint.h>

uint64_t GetTimeMicroseconds();
int IsKBHit();
int ReadKBByte();
int load_images(int ram_size, int *kern_len);
uint32_t load_dtb(uint32_t ram_size);
int console_read_bytes(uint8_t *buf, int len);
void console_write(const char *buf, int len);
#endif /* PORT_H */
