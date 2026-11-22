/* SNES PPU: registers, VRAM/CGRAM/OAM, scanline renderer with widescreen extension.
 *
 * The renderer works line by line (HDMA runs before each line) and draws columns
 * -ext .. 256+ext-1. Layers that are not marked as "wide" (ppu_wide_mask) are transparent
 * outside 0..255, windows clamp their x to 0..255, and sprites use g_obj_wide_x when the game
 * hooks supplied a full screen position for them.
 */
#include "ppu.h"

#include "snes.h"

uint8_t g_vram[0x10000];
uint16_t g_cgram[256];
uint8_t g_oam[544];
Ppu g_ppu;
int16_t g_obj_wide_x[128];
uint8_t g_obj_wide_valid[128];
uint8_t ppu_wide_mask = 0x3F; /* bit0-3 BG1-4, bit4 OBJ, bit5 backdrop */
int ppu_sprite_limit = 0;

static uint16_t vram_word(uint16_t a) { return (uint16_t)(g_vram[(a & 0x7FFF) * 2] | (g_vram[(a & 0x7FFF) * 2 + 1] << 8)); }

void ppu_reset(void) {
  memset(&g_ppu, 0, sizeof g_ppu);
  memset(g_vram, 0, sizeof g_vram);
  memset(g_cgram, 0, sizeof g_cgram);
  memset(g_oam, 0, sizeof g_oam);
  g_ppu.inidisp = 0x80;
}

static uint16_t vram_remap(uint16_t a) {
  switch ((g_ppu.vmain >> 2) & 3) {
    case 1: return (uint16_t)((a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7));
    case 2: return (uint16_t)((a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7));
    case 3: return (uint16_t)((a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7));
  }
  return a;
}
static uint16_t vram_inc(void) {
  static const uint16_t k[4] = {1, 32, 128, 128};
  return k[g_ppu.vmain & 3];
}

void ppu_latch_counters(void) {
  uint32_t line = (uint32_t)((uint64_t)cpu.cyc * 262 / SNES_FRAME_CYCLES + 225) % 262;
  g_ppu.vcounter = (uint16_t)line;
  g_ppu.hcounter = (uint16_t)((cpu.cyc * 7) % 340);
  g_ppu.counter_latched = 1;
}

uint8_t ppu_read(uint8_t reg) {
  Ppu *p = &g_ppu;
  uint8_t v = p->ppu1_open;
  switch (reg) {
    case 0x34: case 0x35: case 0x36: {
      int32_t r = (int32_t)p->m7a * (int8_t)(p->m7b >> 8);
      v = (uint8_t)(r >> (8 * (reg - 0x34)));
      p->ppu1_open = v;
      break;
    }
    case 0x37: ppu_latch_counters(); break;
    case 0x38: {
      uint16_t a = p->oam_addr;
      v = a < 0x200 ? g_oam[a] : g_oam[0x200 | (a & 0x1F)];
      p->oam_addr = (uint16_t)((a + 1) & 0x3FF);
      p->ppu1_open = v;
      break;
    }
    case 0x39: case 0x3A: {
      v = (reg == 0x39) ? (uint8_t)p->vram_prefetch : (uint8_t)(p->vram_prefetch >> 8);
      int after_high = (p->vmain & 0x80) != 0;
      if ((reg == 0x3A) == after_high) {
        p->vram_prefetch = vram_word(vram_remap(p->vmadd));
        p->vmadd = (uint16_t)(p->vmadd + vram_inc());
      }
      p->ppu1_open = v;
      break;
    }
    case 0x3B: {
      uint16_t c = g_cgram[p->cgadd];
      if (!p->cg_read_high) {
        v = (uint8_t)c;
      } else {
        v = (uint8_t)((c >> 8) & 0x7F);
        p->cgadd++;
      }
      p->cg_read_high ^= 1;
      break;
    }
    case 0x3C:
      v = p->hc_high ? (uint8_t)(p->hcounter >> 8) : (uint8_t)p->hcounter;
      p->hc_high ^= 1;
      break;
    case 0x3D:
      v = p->vc_high ? (uint8_t)(p->vcounter >> 8) : (uint8_t)p->vcounter;
      p->vc_high ^= 1;
      break;
    case 0x3E: v = 0x01; break; /* PPU1 version 1 */
    case 0x3F:
      v = (uint8_t)(0x03 | (p->counter_latched ? 0x40 : 0)); /* NTSC, version 3 */
      p->counter_latched = 0;
      p->hc_high = p->vc_high = 0;
      break;
  }
  return v;
}

/* BGnHOFS = (new << 8) | (prev & ~7) | ((reg >> 8) & 7). The low byte's bits 0-2 reach the
 * final value through bits 8-10 of the register after the first write, so the register must keep
 * them: masking it to 10 bits there dropped bit 2 and put the background 4 px off on every frame
 * whose scroll had bit 2 set (sprites out of step with the map when scrolling sideways). */
static void write_hofs(int bg, uint8_t v) {
  Ppu *p = &g_ppu;
  p->hofs_raw[bg] = (uint16_t)((v << 8) | (p->bgofs_latch & ~7) | ((p->hofs_raw[bg] >> 8) & 7));
  p->hofs[bg] = (uint16_t)(p->hofs_raw[bg] & 0x3FF);
  p->bgofs_latch = v;
}
static void write_vofs(int bg, uint8_t v) {
  Ppu *p = &g_ppu;
  p->vofs[bg] = (uint16_t)(((v << 8) | p->bgofs_latch) & 0x3FF);
  p->bgofs_latch = v;
}

void ppu_write(uint8_t reg, uint8_t v) {
  Ppu *p = &g_ppu;
  switch (reg) {
    case 0x00: p->inidisp = v; break;
    case 0x01: p->obsel = v; break;
    case 0x02: p->oamaddl = v; p->oam_addr = (uint16_t)(((p->oamaddh & 1) << 9) | (v << 1)); break;
    case 0x03: p->oamaddh = v; p->oam_addr = (uint16_t)(((v & 1) << 9) | (p->oamaddl << 1)); break;
    case 0x04: {
      uint16_t a = p->oam_addr;
      if (a < 0x200) {
        if (!(a & 1)) {
          p->oam_latch = v;
        } else {
          g_oam[a - 1] = p->oam_latch;
          g_oam[a] = v;
        }
      } else {
        g_oam[0x200 | (a & 0x1F)] = v;
      }
      p->oam_addr = (uint16_t)((a + 1) & 0x3FF);
      break;
    }
    case 0x05: p->bgmode = v; break;
    case 0x06: p->mosaic = v; break;
    case 0x07: case 0x08: case 0x09: case 0x0A: p->bgsc[reg - 7] = v; break;
    case 0x0B: p->bgnba[0] = v; break;
    case 0x0C: p->bgnba[1] = v; break;
    case 0x0D:
      p->m7hofs = (int16_t)(((v << 8) | p->m7_latch) & 0x1FFF);
      p->m7_latch = v;
      write_hofs(0, v);
      break;
    case 0x0E:
      p->m7vofs = (int16_t)(((v << 8) | p->m7_latch) & 0x1FFF);
      p->m7_latch = v;
      write_vofs(0, v);
      break;
    case 0x0F: write_hofs(1, v); break;
    case 0x10: write_vofs(1, v); break;
    case 0x11: write_hofs(2, v); break;
    case 0x12: write_vofs(2, v); break;
    case 0x13: write_hofs(3, v); break;
    case 0x14: write_vofs(3, v); break;
    case 0x15: p->vmain = v; break;
    case 0x16:
      p->vmadd = (uint16_t)((p->vmadd & 0xFF00) | v);
      p->vram_prefetch = vram_word(vram_remap(p->vmadd));
      break;
    case 0x17:
      p->vmadd = (uint16_t)((p->vmadd & 0x00FF) | (v << 8));
      p->vram_prefetch = vram_word(vram_remap(p->vmadd));
      break;
    case 0x18: case 0x19: {
      uint16_t a = vram_remap(p->vmadd) & 0x7FFF;
      g_vram[a * 2 + (reg - 0x18)] = v;
      if ((reg == 0x19) == ((p->vmain & 0x80) != 0)) p->vmadd = (uint16_t)(p->vmadd + vram_inc());
      p->vram_dirty = 1;
      break;
    }
    case 0x1A: p->m7sel = v; break;
    case 0x1B: p->m7a = (int16_t)((v << 8) | p->m7_latch); p->m7_latch = v; break;
    case 0x1C: p->m7b = (int16_t)((v << 8) | p->m7_latch); p->m7_latch = v; break;
    case 0x1D: p->m7c = (int16_t)((v << 8) | p->m7_latch); p->m7_latch = v; break;
    case 0x1E: p->m7d = (int16_t)((v << 8) | p->m7_latch); p->m7_latch = v; break;
    case 0x1F: p->m7x = (int16_t)(((v << 8) | p->m7_latch) & 0x1FFF); p->m7_latch = v; break;
    case 0x20: p->m7y = (int16_t)(((v << 8) | p->m7_latch) & 0x1FFF); p->m7_latch = v; break;
    case 0x21: p->cgadd = v; p->cg_second = 0; p->cg_read_high = 0; break;
    case 0x22:
      if (!p->cg_second) {
        p->cg_latch = v;
      } else {
        g_cgram[p->cgadd] = (uint16_t)(((v & 0x7F) << 8) | p->cg_latch);
        p->cgadd++;
      }
      p->cg_second ^= 1;
      break;
    case 0x23: p->wsel[0] = v; break;  /* BG1/BG2 */
    case 0x24: p->wsel[1] = v; break;  /* BG3/BG4 */
    case 0x25: p->wsel[2] = v; break;  /* OBJ/color */
    case 0x26: p->wh[0] = v; break;
    case 0x27: p->wh[1] = v; break;
    case 0x28: p->wh[2] = v; break;
    case 0x29: p->wh[3] = v; break;
    case 0x2A: p->wbglog = v; break;
    case 0x2B: p->wobjlog = v; break;
    case 0x2C: p->tm = v; break;
    case 0x2D: p->ts = v; break;
    case 0x2E: p->tmw = v; break;
    case 0x2F: p->tsw = v; break;
    case 0x30: p->cgwsel = v; break;
    case 0x31: p->cgadsub = v; break;
    case 0x32: {
      int c = v & 0x1F;
      if (v & 0x20) p->coldata = (uint16_t)((p->coldata & ~0x001F) | c);
      if (v & 0x40) p->coldata = (uint16_t)((p->coldata & ~0x03E0) | (c << 5));
      if (v & 0x80) p->coldata = (uint16_t)((p->coldata & ~0x7C00) | (c << 10));
      break;
    }
    case 0x33: p->setini = v; break;
  }
}

/* ---------------------------------------------------------------------------------------- */
/* Rendering                                                                                 */
/* ---------------------------------------------------------------------------------------- */
#define LAYER_OBJ 4
#define LAYER_BACK 5

typedef struct LinePix {
  uint16_t color[PPU_MAX_W];
  uint8_t opaque[PPU_MAX_W]; /* 0 transparent, else 1 + priority (BG: 0/1, OBJ: 0..3) */
  uint8_t objpal[PPU_MAX_W]; /* OBJ: palette >= 4 (color math allowed) */
} LinePix;

static LinePix s_layer[5];
static int s_transparent_backdrop; /* overlay rendering: backdrop pixels come out transparent */
static int s_w, s_ext;

static int bg_bpp(int mode, int bg) {
  static const int8_t k[8][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
                                 {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}};
  return k[mode][bg];
}

static int mosaic_size(int bg) {
  if (!(g_ppu.mosaic & (1 << bg))) return 1;
  return ((g_ppu.mosaic >> 4) & 15) + 1;
}

static inline int floor_mod(int a, int m) {
  int r = a % m;
  return r < 0 ? r + m : r;
}

/* fetch one tilemap entry for BG at tile coords (tx, ty) in its own tile units */
static inline uint16_t bg_map_entry(int bg, int tx, int ty) {
  uint8_t sc = g_ppu.bgsc[bg];
  uint16_t base = (uint16_t)((sc & 0xFC) << 8);
  int wide = sc & 1, tall = sc & 2;
  int ntx = tx & 63, nty = ty & 63;
  uint16_t a = (uint16_t)(base + ((nty & 31) << 5) + (ntx & 31));
  if (ntx & 32 && wide) a = (uint16_t)(a + 0x400);
  if (nty & 32 && tall) a = (uint16_t)(a + (wide ? 0x800 : 0x400));
  return vram_word(a);
}

static void render_bg(int bg, int y, int mode) {
  Ppu *p = &g_ppu;
  LinePix *L = &s_layer[bg];
  int bpp = bg_bpp(mode, bg);
  int big = (p->bgmode >> (4 + bg)) & 1;
  int tsz = big ? 16 : 8;
  int msz = mosaic_size(bg);
  int my = msz > 1 ? y - y % msz : y;
  uint16_t chr = (uint16_t)(((p->bgnba[bg >> 1] >> ((bg & 1) * 4)) & 15) << 12);
  int palbase = (mode == 0) ? bg * 32 : 0;
  int wide_ok = (ppu_wide_mask >> bg) & 1;
  int opt = (mode == 2 || mode == 4 || mode == 6) && bg < 2;
  for (int i = 0; i < s_w; i++) {
    int x = i - s_ext;
    if (!wide_ok && (x < 0 || x > 255)) {
      L->opaque[i] = 0;
      continue;
    }
    int mx = msz > 1 ? x - floor_mod(x, msz) : x;
    int hofs = p->hofs[bg], vofs = p->vofs[bg];
    if (opt && mx >= 8) {
      /* offset-per-tile: BG3's tilemap row 0 (H) / row 1 (V) at the column's tile */
      int col = ((mx - 8 + (p->hofs[2] & 7)) >> 3) + (p->hofs[2] >> 3);
      int row0 = p->vofs[2] >> 3;
      uint16_t eh = bg_map_entry(2, col, row0);
      uint16_t ev = (mode == 4) ? eh : bg_map_entry(2, col, row0 + 1);
      uint16_t bit = (uint16_t)(bg == 0 ? 0x2000 : 0x4000);
      if (mode == 4) {
        if (eh & bit) {
          if (eh & 0x8000) vofs = eh & 0x3FF;
          else hofs = (hofs & 7) | (eh & 0x3F8);
        }
      } else {
        if (eh & bit) hofs = (hofs & 7) | (eh & 0x3F8);
        if (ev & bit) vofs = ev & 0x3FF;
      }
    }
    int sx = mx + hofs, sy = my + vofs;
    int tx = big ? (sx >> 4) : (sx >> 3);
    int ty = big ? (sy >> 4) : (sy >> 3);
    uint16_t e = bg_map_entry(bg, tx, ty);
    int px = floor_mod(sx, tsz), py = floor_mod(sy, tsz);
    if (e & 0x4000) px = tsz - 1 - px;
    if (e & 0x8000) py = tsz - 1 - py;
    int tile = e & 0x3FF;
    if (big) tile += ((px >> 3) & 1) + (((py >> 3) & 1) << 4);
    int fx = px & 7, fy = py & 7;
    int c = 0;
    uint16_t ta = (uint16_t)(chr + tile * (bpp * 4) + fy);
    uint16_t w0 = vram_word(ta);
    int sh = 7 - fx;
    c = ((w0 >> sh) & 1) | (((w0 >> (sh + 8)) & 1) << 1);
    if (bpp >= 4) {
      uint16_t w1 = vram_word((uint16_t)(ta + 8));
      c |= (((w1 >> sh) & 1) << 2) | (((w1 >> (sh + 8)) & 1) << 3);
    }
    if (bpp == 8) {
      uint16_t w2 = vram_word((uint16_t)(ta + 16)), w3 = vram_word((uint16_t)(ta + 24));
      c |= (((w2 >> sh) & 1) << 4) | (((w2 >> (sh + 8)) & 1) << 5) | (((w3 >> sh) & 1) << 6) |
           (((w3 >> (sh + 8)) & 1) << 7);
    }
    if (!c) {
      L->opaque[i] = 0;
      continue;
    }
    int pal = (e >> 10) & 7;
    uint16_t col;
    if (bpp == 8 && (p->cgwsel & 1)) {
      /* direct color: BBGGGRRR + palette bits */
      col = (uint16_t)(((c & 7) << 2) | ((pal & 1) << 1) | ((((c >> 3) & 7) << 2 | (pal & 2)) << 5) |
                       ((((c >> 6) & 3) << 3 | (pal & 4)) << 10));
    } else {
      int idx = bpp == 2 ? palbase + pal * 4 + c : bpp == 4 ? pal * 16 + c : c;
      col = g_cgram[idx & 0xFF];
    }
    L->color[i] = col;
    L->opaque[i] = (uint8_t)(1 + ((e >> 13) & 1));
  }
}

static void render_mode7(int y, int extbg_layer) {
  Ppu *p = &g_ppu;
  LinePix *L = &s_layer[extbg_layer];
  int sel = p->m7sel;
  int yy = (sel & 2) ? 255 - y : y;
  int msz = mosaic_size(extbg_layer);
  if (msz > 1) yy -= yy % msz;
#define CLIP13(n) (((n) & 0x2000) ? ((n) | ~0x3FF) : ((n) & 0x3FF))
  int hofs = (int16_t)(p->m7hofs << 3) >> 3, vofs = (int16_t)(p->m7vofs << 3) >> 3;
  int cx = (int16_t)(p->m7x << 3) >> 3, cy = (int16_t)(p->m7y << 3) >> 3;
  int a = p->m7a, b = p->m7b, c = p->m7c, d = p->m7d;
  int ox = ((a * CLIP13(hofs - cx)) & ~63) + ((b * CLIP13(vofs - cy)) & ~63) + ((b * yy) & ~63) + (cx << 8);
  int oy = ((c * CLIP13(hofs - cx)) & ~63) + ((d * CLIP13(vofs - cy)) & ~63) + ((d * yy) & ~63) + (cy << 8);
  int wide_ok = (ppu_wide_mask >> extbg_layer) & 1;
  for (int i = 0; i < s_w; i++) {
    int x = i - s_ext;
    if (!wide_ok && (x < 0 || x > 255)) {
      L->opaque[i] = 0;
      continue;
    }
    int xx = (sel & 1) ? 255 - x : x;
    if (msz > 1) xx -= floor_mod(xx, msz);
    int X = (ox + a * xx) >> 8, Y = (oy + c * xx) >> 8;
    int out = (X & ~1023) || (Y & ~1023);
    int colidx;
    if (out && (sel & 0x80)) {
      if (sel & 0x40) {
        colidx = g_vram[((Y & 7) * 8 + (X & 7)) * 2 + 1]; /* tile 0 */
      } else {
        L->opaque[i] = 0;
        continue;
      }
    } else {
      int tile = g_vram[(((Y >> 3) & 127) * 128 + ((X >> 3) & 127)) * 2];
      colidx = g_vram[(tile * 64 + (Y & 7) * 8 + (X & 7)) * 2 + 1];
    }
    if (extbg_layer == 1) {
      int prio = colidx >> 7;
      colidx &= 0x7F;
      if (!colidx) {
        L->opaque[i] = 0;
        continue;
      }
      L->color[i] = g_cgram[colidx];
      L->opaque[i] = (uint8_t)(1 + prio);
    } else {
      if (!colidx) {
        L->opaque[i] = 0;
        continue;
      }
      L->color[i] = (p->cgwsel & 1)
                        ? (uint16_t)(((colidx & 7) << 2) | (((colidx >> 3) & 7) << 7) | (((colidx >> 6) & 3) << 13))
                        : g_cgram[colidx];
      L->opaque[i] = 1;
    }
  }
#undef CLIP13
}

/* the widescreen side table is only trusted while it agrees with the OAM bytes (x low byte and
 * the 9th bit); any OAM entry the game wrote without the hooked writer falls back to OAM */
static inline int obj_wide_ok(int i, int x9) {
  if (!g_obj_wide_valid[i]) return 0;
  int wx = g_obj_wide_x[i];
  return (wx & 0xFF) == (x9 & 0xFF) && ((wx < 0 || wx >= 256) ? 1 : 0) == ((x9 >> 8) & 1);
}

static const uint8_t k_obj_w[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
static const uint8_t k_obj_h[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {32, 64}, {32, 32}};

static void render_obj(int y) {
  Ppu *p = &g_ppu;
  LinePix *L = &s_layer[LAYER_OBJ];
  memset(L->opaque, 0, (size_t)s_w);
  int sizesel = (p->obsel >> 5) & 7;
  uint16_t base = (uint16_t)((p->obsel & 7) << 13);
  uint16_t gap = (uint16_t)((((p->obsel >> 3) & 3) + 1) << 12);
  int wide_ok = (ppu_wide_mask >> LAYER_OBJ) & 1;
  int count = 0, tiles = 0;
  int first = (p->oamaddh & 0x80) ? (p->oam_addr >> 2) & 127 : 0;
  /* draw from lowest priority index last: iterate in reverse so index order wins */
  int list[128], n = 0;
  for (int k = 0; k < 128; k++) {
    int i = (first + k) & 127;
    int hi = (g_oam[0x200 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int large = (hi >> 1) & 1;
    int h = k_obj_h[sizesel][large];
    int sy = g_oam[i * 4 + 1];
    int dy = (y - sy) & 0xFF;
    if (dy >= h) continue;
    if (ppu_sprite_limit && ++count > 32) break;
    list[n++] = i;
  }
  for (int k = n - 1; k >= 0; k--) {
    int i = list[k];
    int hi = (g_oam[0x200 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int large = (hi >> 1) & 1;
    int w = k_obj_w[sizesel][large], h = k_obj_h[sizesel][large];
    int x9 = g_oam[i * 4] | ((hi & 1) << 8);
    int x;
    if (obj_wide_ok(i, x9)) {
      x = g_obj_wide_x[i];
    } else {
      x = x9 >= 256 + (wide_ok ? s_ext : 0) ? x9 - 512 : x9;
    }
    int sy = g_oam[i * 4 + 1];
    int dy = (y - sy) & 0xFF;
    uint8_t attr = g_oam[i * 4 + 3];
    int tile = g_oam[i * 4 + 2];
    int vflip = attr >> 7, hflip = (attr >> 6) & 1;
    int prio = (attr >> 4) & 3, pal = (attr >> 1) & 7;
    int row = vflip ? h - 1 - dy : dy;
    if (ppu_sprite_limit) {
      tiles += w / 8;
      if (tiles > 34) continue;
    }
    for (int col = 0; col < w; col++) {
      int sx = x + col;
      int idx = sx + s_ext;
      if (idx < 0 || idx >= s_w) continue;
      if (!wide_ok && (sx < 0 || sx > 255)) continue;
      int cx = hflip ? w - 1 - col : col;
      int t = (tile & 0xF0) | ((tile + (cx >> 3)) & 0x0F);
      t = (t + ((row >> 3) << 4)) & 0xFF;
      uint16_t ta = (uint16_t)(base + ((attr & 1) ? gap : 0) + t * 16 + (row & 7));
      uint16_t w0 = vram_word(ta), w1 = vram_word((uint16_t)(ta + 8));
      int sh = 7 - (cx & 7);
      int c = ((w0 >> sh) & 1) | (((w0 >> (sh + 8)) & 1) << 1) | (((w1 >> sh) & 1) << 2) |
              (((w1 >> (sh + 8)) & 1) << 3);
      if (!c) continue;
      L->color[idx] = g_cgram[128 + pal * 16 + c];
      L->opaque[idx] = (uint8_t)(1 + prio);
      L->objpal[idx] = pal >= 4;
    }
  }
}

/* windows: returns 1 if pixel x (screen coords) is inside the window region for selector nib */
static inline int window_test(int nib, int logic, int x) {
  Ppu *p = &g_ppu;
  int cx = x < 0 ? 0 : x > 255 ? 255 : x;
  int e1 = nib & 2, e2 = nib & 8;
  if (!e1 && !e2) return 0;
  int w1 = (cx >= p->wh[0] && cx <= p->wh[1]) ^ (nib & 1);
  int w2 = (cx >= p->wh[2] && cx <= p->wh[3]) ^ ((nib >> 2) & 1);
  if (e1 && !e2) return w1;
  if (e2 && !e1) return w2;
  switch (logic & 3) {
    case 0: return w1 | w2;
    case 1: return w1 & w2;
    case 2: return w1 ^ w2;
    default: return !(w1 ^ w2);
  }
}

static int layer_wnib(int layer) {
  Ppu *p = &g_ppu;
  switch (layer) {
    case 0: return p->wsel[0] & 15;
    case 1: return p->wsel[0] >> 4;
    case 2: return p->wsel[1] & 15;
    case 3: return p->wsel[1] >> 4;
    case 4: return p->wsel[2] & 15;
    default: return p->wsel[2] >> 4; /* color window */
  }
}
static int layer_wlog(int layer) {
  Ppu *p = &g_ppu;
  if (layer < 4) return (p->wbglog >> (layer * 2)) & 3;
  return (p->wobjlog >> ((layer - 4) * 2)) & 3;
}

/* z order tables: z[layer][prio], higher = in front; 0 = layer absent in this mode */
static void z_table(int mode, uint8_t z[5][4]) {
  memset(z, 0, 20);
  int bg3hi = (g_ppu.bgmode >> 3) & 1;
  switch (mode) {
    case 0:
      z[4][3] = 12; z[0][1] = 11; z[1][1] = 10; z[4][2] = 9; z[0][0] = 8; z[1][0] = 7;
      z[4][1] = 6; z[2][1] = 5; z[3][1] = 4; z[4][0] = 3; z[2][0] = 2; z[3][0] = 1;
      break;
    case 1:
      if (bg3hi) {
        z[2][1] = 10; z[4][3] = 9; z[0][1] = 8; z[1][1] = 7; z[4][2] = 6; z[0][0] = 5;
        z[1][0] = 4; z[4][1] = 3; z[4][0] = 2; z[2][0] = 1;
      } else {
        z[4][3] = 10; z[0][1] = 9; z[1][1] = 8; z[4][2] = 7; z[0][0] = 6; z[1][0] = 5;
        z[4][1] = 4; z[2][1] = 3; z[4][0] = 2; z[2][0] = 1;
      }
      break;
    case 7:
      if (g_ppu.setini & 0x40) {
        z[4][3] = 7; z[4][2] = 6; z[1][1] = 5; z[4][1] = 4; z[0][0] = 3; z[0][1] = 3;
        z[1][0] = 2; z[4][0] = 1;
      } else {
        z[4][3] = 5; z[4][2] = 4; z[4][1] = 3; z[0][0] = 2; z[0][1] = 2; z[4][0] = 1;
      }
      break;
    default:
      z[4][3] = 8; z[0][1] = 7; z[4][2] = 6; z[1][1] = 5; z[4][1] = 4; z[0][0] = 3;
      z[4][0] = 2; z[1][0] = 1;
      if (mode == 6) z[1][0] = z[1][1] = 0;
      break;
  }
}

static inline uint32_t bgr555_to_argb(uint16_t c, int bright) {
  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  r = (r * bright) / 15;
  g = (g * bright) / 15;
  b = (b * bright) / 15;
  return 0xFF000000u | (uint32_t)(((r << 3) | (r >> 2)) << 16) | (uint32_t)(((g << 3) | (g >> 2)) << 8) |
         (uint32_t)((b << 3) | (b >> 2));
}

PpuLineHook g_ppu_line_hook;

static void render_line(int y, uint32_t *out) {
  Ppu *p = &g_ppu;
  int mode = p->bgmode & 7;
  int bright = p->inidisp & 15;
  if (p->inidisp & 0x80) {
    for (int i = 0; i < s_w; i++) out[i] = 0xFF000000u;
    return;
  }
  uint8_t used = p->tm | p->ts;
  for (int bg = 0; bg < 4; bg++) {
    if (!bg_bpp(mode, bg) || !(used & (1 << bg))) {
      memset(s_layer[bg].opaque, 0, (size_t)s_w);
      continue;
    }
    if (mode == 7) {
      if (bg == 0) render_mode7(y, 0);
      else memset(s_layer[bg].opaque, 0, (size_t)s_w);
    } else {
      render_bg(bg, y, mode);
    }
  }
  if (mode == 7 && (p->setini & 0x40) && (used & 2)) render_mode7(y, 1);
  if (used & 0x10) render_obj(y);
  else memset(s_layer[LAYER_OBJ].opaque, 0, (size_t)s_w);
  if (g_ppu_line_hook) g_ppu_line_hook(y, s_layer[0].color, s_layer[0].opaque, s_w, s_ext);

  uint8_t z[5][4];
  z_table(mode, z);
  uint16_t back = g_cgram[0];
  uint16_t fixed = p->coldata;
  int wide_back = (ppu_wide_mask >> LAYER_BACK) & 1;
  for (int i = 0; i < s_w; i++) {
    int x = i - s_ext;
    if (!wide_back && (x < 0 || x > 255)) {
      out[i] = 0xFF000000u;
      continue;
    }
    /* main screen */
    uint16_t mc = back, sc = fixed;
    int ml = LAYER_BACK, sl = LAYER_BACK, mz = 0, sz = 0, mobjpal = 0;
    for (int l = 0; l < 5; l++) {
      uint8_t o = s_layer[l].opaque[i];
      if (!o) continue;
      int zz = z[l][o - 1];
      if (!zz) continue;
      if ((p->tm >> l) & 1) {
        int masked = ((p->tmw >> l) & 1) && window_test(layer_wnib(l), layer_wlog(l), x);
        if (!masked && zz > mz) {
          mz = zz;
          mc = s_layer[l].color[i];
          ml = l;
          mobjpal = (l == LAYER_OBJ) ? s_layer[l].objpal[i] : 0;
        }
      }
      if ((p->ts >> l) & 1) {
        int masked = ((p->tsw >> l) & 1) && window_test(layer_wnib(l), layer_wlog(l), x);
        if (!masked && zz > sz) {
          sz = zz;
          sc = s_layer[l].color[i];
          sl = l;
        }
      }
    }
    int cw = window_test(layer_wnib(5), layer_wlog(5), x);
    int clip_reg = (p->cgwsel >> 6) & 3, prev_reg = (p->cgwsel >> 4) & 3;
    int clip = clip_reg == 3 || (clip_reg == 1 && !cw) || (clip_reg == 2 && cw);
    int nomath = prev_reg == 3 || (prev_reg == 1 && !cw) || (prev_reg == 2 && cw);
    if (clip) mc = 0;
    int math_on = (p->cgadsub >> (ml == LAYER_BACK ? 5 : ml)) & 1;
    if (ml == LAYER_OBJ && !mobjpal) math_on = 0;
    uint16_t res = mc;
    if (math_on && !nomath) {
      uint16_t other;
      int half = (p->cgadsub >> 6) & 1;
      if (p->cgwsel & 2) {
        other = sc;
        if (sl == LAYER_BACK) {
          other = fixed;
          half = 0;
        }
      } else {
        other = fixed;
      }
      if (clip) half = 0;
      int r1 = mc & 31, g1 = (mc >> 5) & 31, b1 = (mc >> 10) & 31;
      int r2 = other & 31, g2 = (other >> 5) & 31, b2 = (other >> 10) & 31;
      int r, g, b;
      if (p->cgadsub & 0x80) {
        r = r1 - r2; g = g1 - g2; b = b1 - b2;
        if (half) { r >>= 1; g >>= 1; b >>= 1; }
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;
      } else {
        r = r1 + r2; g = g1 + g2; b = b1 + b2;
        if (half) { r >>= 1; g >>= 1; b >>= 1; }
        if (r > 31) r = 31;
        if (g > 31) g = 31;
        if (b > 31) b = 31;
      }
      res = (uint16_t)(r | (g << 5) | (b << 10));
    }
    out[i] = (s_transparent_backdrop && ml == LAYER_BACK) ? 0u : bgr555_to_argb(res, bright);
  }
}

/* ---- helpers for the 3D renderer ------------------------------------------------------------- */

/* Decode the 8x8 tile at tilemap position (tx, ty) of BG layer bg (4bpp/2bpp per mode).
 * out_rgb gets 0xRRGGBB colours, out_op 1 for opaque pixels. Returns the entry's priority bit. */
int ppu_bg_tile(int bg, int tx, int ty, uint32_t out_rgb[64], uint8_t out_op[64]) {
  Ppu *p = &g_ppu;
  int mode = p->bgmode & 7;
  int bpp = bg_bpp(mode, bg);
  uint16_t e = bpp ? bg_map_entry(bg, tx, ty) : 0;
  uint16_t chr = (uint16_t)(((p->bgnba[bg >> 1] >> ((bg & 1) * 4)) & 15) << 12);
  int tile = e & 0x3FF, pal = (e >> 10) & 7, palbase = (mode == 0) ? bg * 32 : 0;
  for (int py = 0; py < 8; py++) {
    int fy = (e & 0x8000) ? 7 - py : py;
    uint16_t ta = (uint16_t)(chr + tile * (bpp * 4) + fy);
    uint16_t w0 = bpp ? vram_word(ta) : 0, w1 = bpp >= 4 ? vram_word((uint16_t)(ta + 8)) : 0;
    for (int px = 0; px < 8; px++) {
      int fx = (e & 0x4000) ? 7 - px : px;
      int sh = 7 - fx;
      int c = ((w0 >> sh) & 1) | (((w0 >> (sh + 8)) & 1) << 1);
      if (bpp >= 4) c |= (((w1 >> sh) & 1) << 2) | (((w1 >> (sh + 8)) & 1) << 3);
      int k = py * 8 + px;
      out_op[k] = c != 0;
      uint16_t col = g_cgram[(bpp == 2 ? palbase + pal * 4 + c : pal * 16 + c) & 0xFF];
      int r = col & 31, g = (col >> 5) & 31, b = (col >> 10) & 31;
      out_rgb[k] = (uint32_t)(((r << 3) | (r >> 2)) << 16) | (uint32_t)(((g << 3) | (g >> 2)) << 8) |
                   (uint32_t)((b << 3) | (b >> 2));
    }
  }
  return (e >> 13) & 1;
}

/* Decode OAM entry i. Fills screen x/y (full x from the widescreen side table when known),
 * size, priority, and 0xAARRGGBB pixels into rgba (row stride 64). Returns 0 if off screen. */
int ppu_obj_piece(int i, int ext, int *ox, int *oy, int *ow, int *oh, int *oprio, uint32_t *rgba) {
  Ppu *p = &g_ppu;
  int sizesel = (p->obsel >> 5) & 7;
  uint16_t base = (uint16_t)((p->obsel & 7) << 13);
  uint16_t gap = (uint16_t)((((p->obsel >> 3) & 3) + 1) << 12);
  int hi = (g_oam[0x200 + (i >> 2)] >> ((i & 3) * 2)) & 3;
  int large = (hi >> 1) & 1;
  int w = k_obj_w[sizesel][large], h = k_obj_h[sizesel][large];
  int x9 = g_oam[i * 4] | ((hi & 1) << 8);
  int x = obj_wide_ok(i, x9) ? g_obj_wide_x[i] : (x9 >= 256 + ext ? x9 - 512 : x9);
  int y = g_oam[i * 4 + 1];
  if (y >= 224 && y + h <= 256) return 0; /* parked below the screen */
  if (y >= 224) y -= 256;
  if (x + w <= -ext || x >= 256 + ext) return 0;
  uint8_t attr = g_oam[i * 4 + 3];
  int tile = g_oam[i * 4 + 2];
  int vflip = attr >> 7, hflip = (attr >> 6) & 1, pal = (attr >> 1) & 7;
  for (int row = 0; row < h; row++) {
    int r = vflip ? h - 1 - row : row;
    for (int col = 0; col < w; col++) {
      int cx = hflip ? w - 1 - col : col;
      int t = (tile & 0xF0) | ((tile + (cx >> 3)) & 0x0F);
      t = (t + ((r >> 3) << 4)) & 0xFF;
      uint16_t ta = (uint16_t)(base + ((attr & 1) ? gap : 0) + t * 16 + (r & 7));
      uint16_t w0 = vram_word(ta), w1 = vram_word((uint16_t)(ta + 8));
      int sh = 7 - (cx & 7);
      int c = ((w0 >> sh) & 1) | (((w0 >> (sh + 8)) & 1) << 1) | (((w1 >> sh) & 1) << 2) |
              (((w1 >> (sh + 8)) & 1) << 3);
      uint32_t px = 0;
      if (c) {
        uint16_t col16 = g_cgram[128 + pal * 16 + c];
        int rr = col16 & 31, gg = (col16 >> 5) & 31, bb = (col16 >> 10) & 31;
        px = 0xFF000000u | (uint32_t)(((rr << 3) | (rr >> 2)) << 16) |
             (uint32_t)(((gg << 3) | (gg >> 2)) << 8) | (uint32_t)((bb << 3) | (bb >> 2));
      }
      rgba[row * 64 + col] = px;
    }
  }
  *ox = x;
  *oy = y;
  *ow = w;
  *oh = h;
  *oprio = (attr >> 4) & 3;
  return 1;
}

/* Render only the layers in mask (BG1-4 = bits 0-3, OBJ = bit 4) on the main screen, with
 * transparent (alpha 0) pixels where none of them is drawn and no colour math. The PPU state is
 * left as it was. Used for the 2D overlay (text windows) on top of the 3D view. */
void ppu_render_layers(uint32_t *out, int pitch_px, int ext, uint8_t mask) {
  Ppu saved = g_ppu;
  DmaChan dsaved[8];
  memcpy(dsaved, g_dma, sizeof dsaved);
  g_ppu.tm &= mask;
  g_ppu.ts = 0;
  g_ppu.cgadsub = 0;
  g_ppu.cgwsel &= 0x0F; /* no clip-to-black */
  s_transparent_backdrop = 1;
  ppu_render_frame(out, pitch_px, ext);
  s_transparent_backdrop = 0;
  uint8_t tm = g_ppu.tm;
  g_ppu = saved;
  (void)tm;
  memcpy(g_dma, dsaved, sizeof dsaved);
}

void ppu_render_frame(uint32_t *out, int pitch_px, int ext) {
  if (ext < 0) ext = 0;
  if (ext > PPU_MAX_EXT) ext = PPU_MAX_EXT;
  s_ext = ext;
  s_w = 256 + 2 * ext;
  hdma_frame_init();
  for (int y = 0; y < PPU_H; y++) {
    hdma_line();
    render_line(y, out + (size_t)y * pitch_px);
  }
}
