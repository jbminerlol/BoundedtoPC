/* Widescreen support inside the game code: native versions of the sprite-to-OAM writer and the
 * entity on-screen test, plus the OAM side table that tells the PPU the full X of every sprite
 * piece (the SNES's 9-bit OAM X cannot tell x=300 from x=-212).
 *
 * g_ws_ext (pixels) / g_ws_cols (8 px columns) are the extra margin on each side; game.c sets
 * them per frame (0 when widescreen is off or the scene is not the overworld). The recompiled
 * DRAW_ENTITIES and REFRESH_MAP_AT_POSITION read them through PATCH_IMM / PATCH_CODE.
 */
#include "enhance.h"

#include <SDL3/SDL.h>

#include "eb_syms.h"
#include "ppu.h"
#include "snes.h"

uint16_t g_ws_ext, g_ws_cols;

/* side table per OAM buffer (OAM1 at $0500, OAM2 at $0800) */
typedef struct ObjSide {
  int16_t x[128];
  int16_t group_y[128]; /* the spritemap's base Y (entity anchor) for the 3D mode */
  int16_t group_x[128];
  uint16_t group[128];  /* spritemap call counter: pieces of one entity share it */
  int8_t slot[128];     /* entity slot that drew the piece (-1: not an entity) */
  uint8_t valid[128];
} ObjSide;
static ObjSide s_side[2];
static uint16_t s_group_counter;
ObjFrameInfo g_obj_frame; /* side info for the OAM that is on screen */

static inline uint16_t w16(uint32_t a) { return (uint16_t)(g_wram[a & 0x1FFFF] | (g_wram[(a + 1) & 0x1FFFF] << 8)); }
static inline void s16(uint32_t a, uint16_t v) {
  g_wram[a & 0x1FFFF] = (uint8_t)v;
  g_wram[(a + 1) & 0x1FFFF] = (uint8_t)(v >> 8);
}

void eb_hook_oam_clear(void) {
  /* OAM_CLEAR fills the buffer that NEXT_FRAME_BUF_ID selects (1 = OAM1) */
  int buf = (w16(SYM_NEXT_FRAME_BUF_ID) == 1) ? 0 : 1;
  memset(s_side[buf].valid, 0, sizeof s_side[buf].valid);
}

/* called by dma_run when a DMA to OAMDATA ($2104) starts */
void eb_oam_dma_source(uint32_t abus) {
  uint16_t off = (uint16_t)abus;
  int buf = -1;
  if (off == SYM_OAM1) buf = 0;
  else if (off == SYM_OAM2) buf = 1;
  if (buf < 0) {
    memset(g_obj_wide_valid, 0, sizeof g_obj_wide_valid);
    g_obj_frame.valid_count = 0;
    return;
  }
  ObjSide *s = &s_side[buf];
  int n = 0;
  for (int i = 0; i < 128; i++) {
    g_obj_wide_valid[i] = s->valid[i];
    g_obj_wide_x[i] = s->x[i];
    g_obj_frame.group[i] = s->group[i];
    g_obj_frame.group_x[i] = s->group_x[i];
    g_obj_frame.group_y[i] = s->group_y[i];
    g_obj_frame.slot[i] = s->slot[i];
    g_obj_frame.valid[i] = s->valid[i];
    n += s->valid[i];
  }
  g_obj_frame.valid_count = n;
}

/* UNKNOWN_C08CD5 (C0/C08CD5.asm): A = spritemap (in SPRITEMAP_BANK), X/Y = screen base.
 * Writes one OAM entry per spritemap piece into the current OAM buffer. Same logic as the
 * original, except that in widescreen pieces up to 256+g_ws_ext are kept and every piece's full
 * X goes into the side table. Returns with P, D, DB as on entry (PHP/PLP in the original). */
void F_UNKNOWN_C08CD5(void) {
  uint8_t p_saved = get_p();
  uint16_t base_x = cpu.X, base_y = cpu.Y, sm = cpu.A;
  s16(SYM_CURRENT_SPRITE_BASE_X, base_x);
  s16(SYM_CURRENT_SPRITE_BASE_Y, base_y);
  uint16_t x = w16(SYM_OAM_ADDR);
  uint16_t yreg = sm;
  if (x >= w16(SYM_OAM_END_ADDR)) goto done;
  {
    uint32_t bank = (uint32_t)g_wram[SYM_SPRITEMAP_BANK] << 16;
    int buf = (x >= SYM_OAM2) ? 1 : 0;
    uint16_t oam_base = buf ? SYM_OAM2 : SYM_OAM1;
    uint16_t group = ++s_group_counter;
    int wide_limit = 256 + g_ws_ext; /* first x that is off the right edge */
    /* which entity is this (for the 3D view's camera-relative sprites)? Entity spritemaps are
     * queued with the entity's screen position and its spritemap (or the mirrored one after it) */
    int8_t slot = -1;
    for (int s = 0; s < 30; s++) {
      uint16_t lo = w16(SYM_ENTITY_SPRITEMAP_POINTER_LOW + 2 * s);
      if (lo != sm && (uint16_t)(lo + w16(SYM_ENTITY_SPRITEMAP_SIZES + 2 * s)) != sm) continue;
      if (w16(SYM_ENTITY_SCREEN_X_TABLE + 2 * s) != base_x || w16(SYM_ENTITY_SCREEN_Y_TABLE + 2 * s) != base_y) continue;
      slot = (int8_t)s;
      break;
    }
    for (;;) {
      uint8_t yo = rd8(bank | yreg);
      if (yo == 0x80) { /* chained spritemap: tile field holds the next address */
        yreg = rd16(bank | (uint16_t)(yreg + 1));
        continue;
      }
      int16_t sy = (int16_t)(int8_t)yo + (int16_t)base_y - 1;
      uint8_t special = rd8(bank | (uint16_t)(yreg + 4));
      uint16_t ycheck = (uint16_t)sy;
      int visible_y = ycheck < 0x00E0 || ycheck >= 0xFFE0;
      int16_t sx = (int16_t)((int16_t)(int8_t)rd8(bank | (uint16_t)(yreg + 3)) + (int16_t)base_x);
      int keep = 0;
      if (visible_y) {
        uint8_t hi = (uint8_t)((uint16_t)sx >> 8);
        keep = (hi == 0x00 || hi == 0xFF);
        if (!keep && g_ws_ext && sx >= 256 && sx < wide_limit) keep = 1;
      }
      if (keep) {
        /* tile + attributes, x low byte, y */
        uint16_t tile = rd16(bank | (uint16_t)(yreg + 1));
        s16(x + 2u, tile);
        g_wram[x] = (uint8_t)sx;
        int x9 = (sx < 0 || sx >= 256) ? 1 : 0;
        uint8_t hb = g_wram[SYM_OAM_HIGH_TABLE_BUFFER];
        int c = hb & 1;
        hb = (uint8_t)((hb >> 1) | (x9 << 7));
        int c2 = hb & 1;
        hb = (uint8_t)((hb >> 1) | ((special & 1) << 7));
        (void)c;
        if (c2) {
          uint32_t hta = rd24_b0((uint16_t)SYM_OAM_HIGH_TABLE_ADDR);
          wr8(hta, hb);
          s16(SYM_OAM_HIGH_TABLE_ADDR, (uint16_t)(w16(SYM_OAM_HIGH_TABLE_ADDR) + 1));
          hb = 0x80;
        }
        g_wram[SYM_OAM_HIGH_TABLE_BUFFER] = hb;
        g_wram[x + 1u] = (uint8_t)sy;
        s16(SYM_CURRENT_SPRITE_Y, (uint16_t)sy);
        int idx = (x - oam_base) >> 2;
        if (idx >= 0 && idx < 128) {
          ObjSide *s = &s_side[buf];
          s->x[idx] = sx;
          s->valid[idx] = 1;
          s->group[idx] = group;
          s->group_x[idx] = (int16_t)base_x;
          s->group_y[idx] = (int16_t)base_y;
          s->slot[idx] = slot;
        }
        x = (uint16_t)(x + 4);
        if (special & 0x80) break;
        if (x >= w16(SYM_OAM_END_ADDR)) break;
      } else {
        if (special & 0x80) break;
      }
      yreg = (uint16_t)(yreg + 5);
    }
    s16(SYM_OAM_ADDR, x);
  }
done:
  cpu.X = x;
  cpu.Y = yreg;
  set_p(p_saved);
  cpu.cyc += 40;
  NATIVE_RTL();
}

/* UNKNOWN_C01B96: sprite VRAM cell allocator (only CREATE_ENTITY -> UNKNOWN_C01C52 calls it).
 * A = cells needed, X = owner tag; returns the first cell or -253. SPRITE_VRAM_TABLE has 88 cells
 * of 2x2 tiles, 8 per band of two tile rows. The original takes the first run of free cells, but
 * the frame upload (UPDATE_ENTITY_SPRITE_OFFSET / UNKNOWN_C0A56E) writes each pair of frame rows
 * as one straight run over ceil(width/2) cells, so a group that straddles the end of a band spills
 * into the start of the next tile row - another sprite's cells (one sprite's piece shows up inside
 * another: Ness in the attract demo, the mailbox near Ness's house). Prefer the first free run
 * whose groups each stay inside one band; otherwise the original first fit. */
void F_UNKNOWN_C01B96(void) {
  int need = cpu.A & 0xFF, owner = (cpu.X | 0x80) & 0xFF;
  int g = (w16(SYM_NEW_SPRITE_TILE_WIDTH) + 1) / 2;
  uint8_t *tab = &g_wram[SYM_SPRITE_VRAM_TABLE];
  int first_fit = -1, pick = -1, scanned = 0;
  if (g < 1) g = 1;
  for (int c = 0; need > 0 && c + need <= 88 && pick < 0; c++) {
    int k = 0;
    while (k < need && !tab[c + k]) k++;
    scanned += k + 1;
    if (k < need) {
      c += k; /* skip past the used cell, like the original */
      continue;
    }
    if (first_fit < 0) first_fit = c;
    int ok = 1;
    if (g > 1 && g <= 8 && need % g == 0)
      for (k = 0; k < need && ok; k += g) ok = ((c + k) & 7) + g <= 8;
    if (ok) pick = c;
  }
  if (pick < 0) pick = first_fit;
  if (need == 0) pick = first_fit = 0; /* original: zero cells "fit" at cell 0 */
  {
    static int log_on = -1;
    if (log_on < 0) log_on = SDL_getenv("EB_ENT_LOG") != NULL;
    if (log_on && pick != first_fit)
      eb_log("vram alloc f%llu: %d cells (groups of %d) at cell %d instead of %d (keeps rows inside a band)",
             (unsigned long long)g_frame_count, need, g, pick, first_fit);
  }
  for (int k = 0; pick >= 0 && k < need; k++) tab[pick + k] = (uint8_t)owner;
  op_rep(0x30);
  cpu.A = pick >= 0 ? (uint16_t)pick : (uint16_t)-253;
  cpu.Y = cpu.A;
  NZ16(cpu.A);
  cpu.cyc += 60 + scanned * 30;
  NATIVE_RTL();
}

static int sprite_box_onscreen(uint16_t sx, uint16_t sy, uint16_t sy2) {
  if (g_ws_ext) {
    int16_t xs = (int16_t)sx;
    return xs >= -(int)g_ws_ext && xs < 256 + (int)g_ws_ext && !((sy | sy2) & 0xFF00);
  }
  return !((sx | sy | sy2) & 0xFF00);
}

/* IS_ENTITY_SPRITE_ONSCREEN: -1 if the current entity's sprite box is on screen. The original
 * requires the box's left x, top y and y+8 to lie in 0..255; widescreen allows -ext..255+ext.
 *
 * Fix for sprites shown with another sprite's graphics: an entity that asks for its first frame
 * on its first tick still has INIT_ENTITY's screen position, which is its world position, so the
 * original answers "off screen" and skips the upload. Emote bubbles (the "!" over King when Pokey
 * calls; script 785 = UPDATE_ENTITY_SPRITE_CURRENT_FRAME0 then HALT) never upload again, and draw
 * whatever an earlier sprite left in their VRAM cells. While screen == world, also try the
 * position the screen-position callback is about to compute (world - BG1 scroll). Saying "on
 * screen" too often only costs one extra upload into the entity's own cells. */
void F_IS_ENTITY_SPRITE_ONSCREEN(void) {
  uint16_t slot2 = (uint16_t)(w16(SYM_CURRENT_ENTITY_SLOT) * 2);
  uint16_t size2 = (uint16_t)(w16(SYM_ENTITY_SIZES + slot2) * 2);
  uint16_t ox = rd16(0xC42A1Fu + size2), oy = rd16(0xC42A41u + size2);
  uint16_t sx = (uint16_t)(w16(SYM_ENTITY_SCREEN_X_TABLE + slot2) - ox);
  uint16_t syv = w16(SYM_ENTITY_SCREEN_Y_TABLE + slot2);
  uint16_t sy = (uint16_t)(syv - oy);
  uint16_t sy2 = (uint16_t)(syv + 8);
  int on = sprite_box_onscreen(sx, sy, sy2), fresh = 0;
  uint16_t ax = w16(SYM_ENTITY_ABS_X_TABLE + slot2), ay = w16(SYM_ENTITY_ABS_Y_TABLE + slot2);
  if (!on && w16(SYM_ENTITY_SCREEN_X_TABLE + slot2) == ax && syv == ay) {
    uint16_t cx = (uint16_t)(ax - w16(SYM_BG1_X_POS)), cy = (uint16_t)(ay - w16(SYM_BG1_Y_POS));
    on = fresh = sprite_box_onscreen((uint16_t)(cx - ox), (uint16_t)(cy - oy), (uint16_t)(cy + 8));
  }
  {
    static int log_on = -1;
    if (log_on < 0) log_on = SDL_getenv("EB_ENT_LOG") != NULL;
    if (log_on)
      eb_log("onscreen? f%llu slot %d sprite %d screen %d,%d abs %d,%d -> %s", (unsigned long long)g_frame_count, slot2 / 2,
             w16(SYM_ENTITY_SPRITE_IDS + slot2), (int16_t)w16(SYM_ENTITY_SCREEN_X_TABLE + slot2), (int16_t)syv,
             w16(SYM_ENTITY_ABS_X_TABLE + slot2), w16(SYM_ENTITY_ABS_Y_TABLE + slot2), on ? (fresh ? "yes (new entity, from world - scroll)" : "yes") : "no");
  }
  op_rep(0x30);
  cpu.A = on ? 0xFFFF : 0;
  NZ16(cpu.A);
  cpu.cyc += 60;
  NATIVE_RTL();
}
