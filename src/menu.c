/* Menus in EarthBound's own style.
 *
 * Text uses the game's main font straight from the ROM: MAIN_FONT_DATA holds one width byte per
 * character and MAIN_FONT_GFX 32 bytes per character, 8-pixel columns of 16 one-byte rows, the
 * same glyphs RENDER_TEXT / RENDER_VWF_CHARACTER_TO_WINDOW draw (index = ASCII - $20). Which bit
 * value is ink is read off the blank space glyph.
 *
 * In game the window frame is the game's own: BG3 tile $10 (corners), $11 (top and bottom), $12
 * (sides) with palette 7, flipped like DRAW_WINDOW does ($3C10/$7C10/$BC10/$FC10...), so it
 * follows the player's window flavour. Before the game has loaded its window graphics (the ROM
 * picker) a drawn copy of the plain frame stands in. */
#include "menu.h"

#include <string.h>

#include "ppu.h"
#include "snes.h"

#define FONT_DATA 0xE10C7Au /* MAIN_FONT_DATA (US ROM) */
#define FONT_GFX 0xE10CDAu  /* MAIN_FONT_GFX */

static uint32_t *s_cv;
static int s_cw, s_ch;
static int s_ink_bit = -1;

static uint8_t rom8(uint32_t a) {
  uint32_t off = ((a >> 16) & 0x3F) << 16 | (a & 0xFFFF);
  return g_rom[off & 0x3FFFFF];
}

static inline void put(int x, int y, uint32_t c) {
  if (x >= 0 && x < s_cw && y >= 0 && y < s_ch) s_cv[y * s_cw + x] = c;
}

static uint32_t cgram_argb(int i) {
  uint16_t c = g_cgram[i & 255];
  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return 0xFF000000u | (uint32_t)((r << 3) | (r >> 2)) << 16 | (uint32_t)((g << 3) | (g >> 2)) << 8 |
         (uint32_t)((b << 3) | (b >> 2));
}

/* colour index (0-3) of pixel (x, y) of a BG3 2bpp tile, with the tilemap's flips */
static int bg3_px(int tile, int x, int y, int hflip, int vflip) {
  if (hflip) x = 7 - x;
  if (vflip) y = 7 - y;
  uint16_t base = (uint16_t)((g_ppu.bgnba[1] & 15) << 12);
  uint16_t a = (uint16_t)((base + tile * 8 + y) & 0x7FFF);
  uint16_t w = (uint16_t)(g_vram[a * 2] | g_vram[a * 2 + 1] << 8);
  int sh = 7 - x;
  return ((w >> sh) & 1) | (((w >> (sh + 8)) & 1) << 1);
}

/* the game's frame is usable when its corner tile has pixels and palette 7 isn't all black */
static int game_frame_ok(void) {
  int px = 0;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) px += bg3_px(0x10, x, y, 0, 0) != 0;
  uint32_t any = (cgram_argb(29) | cgram_argb(30) | cgram_argb(31)) & 0xFFFFFF;
  return px >= 6 && any;
}

void menu_begin(uint32_t *canvas, int w, int h) {
  s_cv = canvas;
  s_cw = w;
  s_ch = h;
  memset(canvas, 0, (size_t)w * h * 4);
}

static void blit_tile(int tile, int x0, int y0, int hflip, int vflip, uint32_t fill) {
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      int c = bg3_px(tile, x, y, hflip, vflip);
      put(x0 + x, y0 + y, c ? cgram_argb(28 + c) : fill);
    }
}

void menu_window(int x, int y, int w, int h, int game_style) {
  w = (w + 7) & ~7;
  h = (h + 7) & ~7;
  if (game_style && game_frame_ok()) {
    /* the inside colour: the corner tile's innermost pixel */
    int ci = bg3_px(0x10, 7, 7, 0, 0);
    uint32_t fill = ci ? cgram_argb(28 + ci) : 0xFF000000u;
    for (int yy = y + 8; yy < y + h - 8; yy++)
      for (int xx = x + 8; xx < x + w - 8; xx++) put(xx, yy, fill);
    for (int tx = x + 8; tx < x + w - 8; tx += 8) {
      blit_tile(0x11, tx, y, 0, 0, fill);
      blit_tile(0x11, tx, y + h - 8, 0, 1, fill);
    }
    for (int ty = y + 8; ty < y + h - 8; ty += 8) {
      blit_tile(0x12, x, ty, 0, 0, fill);
      blit_tile(0x12, x + w - 8, ty, 1, 0, fill);
    }
    blit_tile(0x10, x, y, 0, 0, fill);
    blit_tile(0x10, x + w - 8, y, 1, 0, fill);
    blit_tile(0x10, x, y + h - 8, 0, 1, fill);
    blit_tile(0x10, x + w - 8, y + h - 8, 1, 1, fill);
    return;
  }
  /* the plain frame: black inside, a white line with rounded corners between dark edges */
  const uint32_t black = 0xFF101010u, white = 0xFFF8F8F8u, edge = 0xFF505058u;
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++) {
      int dx = xx - x < x + w - 1 - xx ? xx - x : x + w - 1 - xx;
      int dy = yy - y < y + h - 1 - yy ? yy - y : y + h - 1 - yy;
      int d = dx < dy ? dx : dy;
      if (dx < 3 && dy < 3 && dx + dy < 3) continue; /* rounded corner */
      uint32_t c = d == 0 ? edge : d == 2 ? white : d == 1 || d == 3 ? edge : black;
      if (dx < 4 && dy < 4 && dx + dy == 3) c = white; /* the white line turning the corner */
      put(xx, yy, c);
    }
}

static void font_init(void) {
  if (s_ink_bit >= 0) return;
  int ones = 0;
  for (int i = 0; i < 32; i++) ones += rom8(FONT_GFX + i) == 0xFF;
  s_ink_bit = ones > 16 ? 0 : 1;
}

static int glyph_w(int c) {
  if (c < 0x20 || c > 0x7F) c = '?';
  int w = rom8(FONT_DATA + (uint32_t)(c - 0x20));
  return w > 16 ? 16 : w;
}

int menu_text_width(const char *s) {
  int w = 0;
  for (; *s; s++) w += glyph_w((uint8_t)*s) + 1;
  return w;
}

int menu_text(int x, int y, const char *s, uint32_t argb) {
  font_init();
  for (; *s; s++) {
    int c = (uint8_t)*s;
    if (c < 0x20 || c > 0x7F) c = '?';
    int w = glyph_w(c);
    uint32_t g = FONT_GFX + (uint32_t)(c - 0x20) * 32;
    for (int col = 0; col * 8 < w; col++)
      for (int r = 0; r < 16; r++) {
        uint8_t b = rom8(g + (uint32_t)(col * 16 + r));
        for (int px = 0; px < 8 && col * 8 + px < w; px++)
          if (((b >> (7 - px)) & 1) == s_ink_bit) put(x + col * 8 + px, y + r, argb);
      }
    x += w + 1;
  }
  return x;
}

uint32_t menu_ink(int game_style) {
  if (game_style && game_frame_ok()) {
    /* the brightest colour of the window palette */
    uint32_t best = 0xFFF8F8F8u;
    int bl = -1;
    for (int i = 29; i <= 31; i++) {
      uint32_t c = cgram_argb(i);
      int l = (int)((c >> 16) & 255) * 3 + (int)((c >> 8) & 255) * 6 + (int)(c & 255);
      if (l > bl) {
        bl = l;
        best = c;
      }
    }
    return best;
  }
  return 0xFFF8F8F8u;
}

uint32_t menu_dim(int game_style) {
  uint32_t c = menu_ink(game_style);
  return 0xFF000000u | ((c >> 1) & 0x7F7F7Fu) | 0x202020u;
}

void menu_cursor(int x, int y, uint32_t argb) {
  for (int r = 0; r < 7; r++) {
    int len = r < 4 ? r + 1 : 7 - r;
    for (int i = 0; i < len; i++) put(x - 4 + i, y + 3 + r, argb);
  }
}
