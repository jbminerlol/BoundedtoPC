/* Menus drawn in EarthBound's own style (settings menu, ROM picker). */
#pragma once
#include <stdint.h>

/* all drawing goes into an ARGB canvas (0 = transparent) the size of the game picture */
void menu_begin(uint32_t *canvas, int w, int h);
/* a text window; game_style 1 = the game's own frame from VRAM (in game), 0 = a drawn copy of
 * the plain frame (before the game has loaded its window graphics) */
void menu_window(int x, int y, int w, int h, int game_style);
/* the main font from ROM (needs a loaded ROM); returns the x after the text */
int menu_text(int x, int y, const char *s, uint32_t argb);
int menu_text_width(const char *s);
/* the colours text and dimmed text use in that window style */
uint32_t menu_ink(int game_style);
uint32_t menu_dim(int game_style);
/* EarthBound's little cursor triangle, pointing right, its tip at (x, y + 6) */
void menu_cursor(int x, int y, uint32_t argb);
