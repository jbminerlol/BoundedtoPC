#pragma once
#include <stdint.h>

typedef struct Ppu {
  uint8_t inidisp, obsel, oamaddl, oamaddh, bgmode, mosaic;
  uint8_t bgsc[4], bgnba[2];
  uint16_t hofs[4], vofs[4];
  uint16_t hofs_raw[4]; /* full write-twice register: its bits 8-10 feed the next HOFS write */
  uint8_t bgofs_latch;
  uint8_t vmain;
  uint16_t vmadd, vram_prefetch;
  uint8_t m7sel, m7_latch;
  int16_t m7a, m7b, m7c, m7d, m7x, m7y, m7hofs, m7vofs;
  uint8_t cgadd, cg_latch, cg_second, cg_read_high;
  uint8_t wsel[3], wh[4], wbglog, wobjlog;
  uint8_t tm, ts, tmw, tsw, cgwsel, cgadsub;
  uint16_t coldata;
  uint8_t setini;
  uint16_t oam_addr;
  uint8_t oam_latch;
  uint8_t ppu1_open;
  uint16_t hcounter, vcounter;
  uint8_t hc_high, vc_high, counter_latched;
  uint8_t vram_dirty;
} Ppu;

extern Ppu g_ppu;
extern uint8_t ppu_wide_mask;    /* which layers draw outside 0..255 */
extern int ppu_sprite_limit;     /* emulate the 32 sprites / 34 tiles per line limits */
extern int16_t g_obj_wide_x[128];  /* game-supplied full screen x per OAM entry (widescreen) */
extern uint8_t g_obj_wide_valid[128];

typedef void (*PpuLineHook)(int y, const uint16_t *bg1col, const uint8_t *bg1op, int w, int ext);
extern PpuLineHook g_ppu_line_hook;
