#pragma once
#include <stdint.h>

/* Writes a 24-bit PNG from 32-bit BGRA pixels (rows `stride` bytes apart, bottom_up = rows
 * stored last-to-first as OpenGL reads them). Returns 1 on success. */
int png_write_bgra(const char *path, const uint8_t *bgra, int w, int h, int stride, int bottom_up);
