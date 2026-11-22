/* EarthBound-specific glue: scene detection and enhancement switches. */
#include "game.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "eb_syms.h"
#include "enhance.h"
#include "host.h"
#include "ppu.h"
#include "snes.h"

static inline uint16_t ram16(uint32_t a) { return (uint16_t)(g_wram[a & 0x1FFFF] | (g_wram[(a + 1) & 0x1FFFF] << 8)); }

static int in_battle(void) { return ram16(SYM_BATTLE_MODE_FLAG) != 0 || ram16(SYM_BATTLE_MODE) != 0; }

/* the overworld map is on screen: mode 1 with BG1/BG2 tilemaps at the overworld addresses */
static int overworld_layout(void) {
  return (g_ppu.bgmode & 7) == 1 && (g_ppu.bgsc[0] & 0xFC) == 0x38 && (g_ppu.bgsc[1] & 0xFC) == 0x58;
}

int game_on_map(void) { return overworld_layout() && !in_battle(); }

/* QOL: the battle background fills the screen instead of the original letterbox band */
int eb_skip_letterbox_hdma(void) { return g_cfg.fill_letterbox && in_battle(); }

GameScene game_scene(void) {
  if (in_battle()) return SCENE_BATTLE;
  if (overworld_layout()) return SCENE_OVERWORLD;
  return SCENE_OTHER;
}

void game_init(void) {}

/* battle background layout: BG1/BG2 tilemaps at $5800/$5C00 (32x32 tiles, they tile sideways) */
static int battle_layout(void) {
  return (g_ppu.bgmode & 7) == 1 && (g_ppu.bgsc[0] & 0xFC) == 0x58 && (g_ppu.bgsc[1] & 0xFC) == 0x5C;
}

/* Called before each frame is drawn. ext = widescreen margin in pixels (0 = off). */
void game_before_render(int ext) {
  GameScene sc = game_scene();
  g_ws_ext = 0;
  g_ws_cols = 0;
  /* the voxel view looks past the 4:3 edges too: keep the full margin loaded on the map */
  if (g_cfg.voxel3d && overworld_layout() && ext < 72) ext = 72;
  if (ext > 0 && overworld_layout()) {
    /* map (also during the battle swirl): BG1/BG2, sprites and backdrop extend; BG3 (text
     * windows) stays in the 4:3 area */
    ppu_wide_mask = 0x01 | 0x02 | 0x10 | 0x20;
    g_ws_ext = (uint16_t)ext;
    g_ws_cols = (uint16_t)((ext + 7) / 8);
    if (g_ws_cols > 9) g_ws_cols = 9; /* the map's column buffers reach 9 tiles past the edge */
  } else if (ext > 0 && sc == SCENE_BATTLE && battle_layout()) {
    /* battle: the two background layers, sprites and backdrop extend (letterbox and
     * distortion are per line, so they carry across); command/HP windows (BG3) stay put */
    ppu_wide_mask = 0x01 | 0x02 | 0x10 | 0x20;
  } else {
    ppu_wide_mask = 0;
    g_ws_ext = 0;
    g_ws_cols = 0;
  }
}

/* EB_ENT_LOG=1: log every change to the entity slots' sprite set-up and to the sprite VRAM
 * allocation table, to track down sprites drawn with another entity's graphics */
static void log_entity_changes(void) {
  static int on = -1;
  if (on < 0) on = SDL_getenv("EB_ENT_LOG") != NULL;
  if (!on) return;
  static uint16_t last[30][5];
  static uint8_t last_tab[88];
  static int primed;
  for (int s = 0; s < 30; s++) {
    uint16_t v[5] = {ram16(SYM_ENTITY_SCRIPT_TABLE + 2 * s), ram16(SYM_ENTITY_SPRITE_IDS + 2 * s),
                     ram16(SYM_ENTITY_VRAM_ADDRESS + 2 * s), ram16(SYM_ENTITY_SPRITEMAP_POINTER_LOW + 2 * s),
                     ram16(SYM_ENTITY_CURRENT_DISPLAYED_SPRITES + 2 * s)};
    if (primed && !memcmp(v, last[s], sizeof v)) continue;
    if (primed || v[0] != 0xFFFF)
      eb_log("ent f%llu slot %2d: script %04x sprite %3d vram %04x smap %04x shown %04x  at %d,%d",
             (unsigned long long)g_frame_count, s, v[0], v[1], v[2], v[3], v[4], ram16(SYM_ENTITY_ABS_X_TABLE + 2 * s),
             ram16(SYM_ENTITY_ABS_Y_TABLE + 2 * s));
    memcpy(last[s], v, sizeof v);
  }
  if (!primed || memcmp(last_tab, &g_wram[SYM_SPRITE_VRAM_TABLE], 88)) {
    char line[88 * 3 + 1];
    int n = 0;
    for (int i = 0; i < 88; i++) n += SDL_snprintf(line + n, sizeof line - (size_t)n, "%02x ", g_wram[SYM_SPRITE_VRAM_TABLE + i]);
    eb_log("vramtab f%llu: %s", (unsigned long long)g_frame_count, line);
    memcpy(last_tab, &g_wram[SYM_SPRITE_VRAM_TABLE], 88);
  }
  primed = 1;
}

/* EB_ENT_LOG: also check every frame that no two live entities share sprite VRAM tiles (footprint
 * as UPDATE_ENTITY_SPRITE_OFFSET / UNKNOWN_C0A56E lay the rows out) or spritemap RAM */
static void check_entity_overlaps(void) {
  static int on = -1;
  if (on < 0) on = SDL_getenv("EB_ENT_LOG") != NULL;
  if (!on) return;
  static int8_t owner[512]; /* OBJ tiles 0x4000-0x5FFF */
  static uint32_t last_report;
  memset(owner, -1, sizeof owner);
  char msg[256];
  msg[0] = 0;
  for (int s = 0; s < 30 && !msg[0]; s++) {
    if (ram16(SYM_ENTITY_SCRIPT_TABLE + 2 * s) == 0xFFFF) continue;
    int dest = ram16(SYM_ENTITY_VRAM_ADDRESS + 2 * s), size = ram16(SYM_ENTITY_BYTE_WIDTHS + 2 * s);
    int rows = ram16(SYM_ENTITY_TILE_HEIGHTS + 2 * s);
    if (dest < 0x4000 || dest >= 0x6000 || size <= 0 || size > 512 || rows <= 0 || rows > 16) continue;
    for (int r = 0; r < rows && !msg[0]; r++) {
      for (int w = 0; w < size / 2; w += 16) {
        int t = ((dest + w) - 0x4000) >> 4;
        if (t < 0 || t >= 512) continue;
        if (owner[t] >= 0 && owner[t] != s) {
          SDL_snprintf(msg, sizeof msg, "VRAM tile %03x (word %04x) used by slots %d (sprite %d) and %d (sprite %d)", t,
                       0x4000 + t * 16, owner[t], ram16(SYM_ENTITY_SPRITE_IDS + 2 * owner[t]), s,
                       ram16(SYM_ENTITY_SPRITE_IDS + 2 * s));
          break;
        }
        owner[t] = (int8_t)s;
      }
      if ((dest & 0x100) == 0) {
        dest += 0x100;
      } else {
        int n = dest + (((size + 0x20) & ~0x3F) >> 1);
        dest = ((n ^ dest) & 0x100) ? n : n - 0x100;
      }
    }
  }
  /* spritemap RAM: [ptr, ptr + 2 * size) per entity */
  for (int a = 0; a < 30 && !msg[0]; a++) {
    if (ram16(SYM_ENTITY_SCRIPT_TABLE + 2 * a) == 0xFFFF) continue;
    int pa = ram16(SYM_ENTITY_SPRITEMAP_POINTER_LOW + 2 * a), sa = ram16(SYM_ENTITY_SPRITEMAP_SIZES + 2 * a) * 2;
    for (int b = a + 1; b < 30; b++) {
      if (ram16(SYM_ENTITY_SCRIPT_TABLE + 2 * b) == 0xFFFF) continue;
      int pb = ram16(SYM_ENTITY_SPRITEMAP_POINTER_LOW + 2 * b), sb = ram16(SYM_ENTITY_SPRITEMAP_SIZES + 2 * b) * 2;
      if (pa < pb + sb && pb < pa + sa) {
        SDL_snprintf(msg, sizeof msg, "spritemap RAM %04x+%d (slot %d sprite %d) overlaps %04x+%d (slot %d sprite %d)", pa,
                     sa, a, ram16(SYM_ENTITY_SPRITE_IDS + 2 * a), pb, sb, b, ram16(SYM_ENTITY_SPRITE_IDS + 2 * b));
        break;
      }
    }
  }
  if (msg[0]) {
    uint32_t h = 2166136261u;
    for (const char *p = msg; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (h != last_report) eb_log("OVERLAP f%llu: %s", (unsigned long long)g_frame_count, msg);
    last_report = h;
  } else {
    last_report = 0;
  }
}

/* EB_ENT_LOG: every OAM piece an entity draws must use tiles from its own sprite VRAM cells
 * (SPRITE_VRAM_TABLE: 88 cells of 2x2 tiles, 8 per band of two tile rows). Report the first piece
 * that reads another entity's cells, with the entity set-up, to pin down sprites drawn with
 * someone else's graphics. */
static void check_piece_ownership(void) {
  static int on = -1;
  if (on < 0) on = SDL_getenv("EB_ENT_LOG") != NULL;
  if (!on || game_scene() != SCENE_OVERWORLD) return;
  static uint32_t last_report;
  static uint8_t prev_tab[88];   /* the OAM on screen was built a frame ago, against this table */
  static int streak;
  char msg[400];
  msg[0] = 0;
  int sizesel = (g_ppu.obsel >> 5) & 7;
  static const uint8_t ws[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
  for (int i = 0; i < 128 && !msg[0]; i++) {
    if (!g_obj_frame.valid[i] || g_obj_frame.slot[i] < 0) continue;
    int s = g_obj_frame.slot[i];
    int y = g_oam[i * 4 + 1];
    if (y >= 224 && y < 240) continue;
    int tile = g_oam[i * 4 + 2], attr = g_oam[i * 4 + 3];
    if (attr & 1) continue; /* second name table: not overworld entity tiles */
    int hi = (g_oam[0x200 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int w = ws[sizesel][(hi >> 1) & 1];
    for (int ty = 0; ty < w / 8 && !msg[0]; ty++)
      for (int tx = 0; tx < w / 8; tx++) {
        int t = ((tile & 0xF0) + ty * 16) & 0xF0;
        t |= (tile + tx) & 0x0F;
        int cell = (t >> 5) * 8 + ((t & 0x0F) >> 1);
        if (cell >= 88) continue;
        uint8_t own = prev_tab[cell];
        if (own == (uint8_t)(0x80 | s) || g_wram[SYM_SPRITE_VRAM_TABLE + cell] == (uint8_t)(0x80 | s)) continue;
        SDL_snprintf(msg, sizeof msg,
                     "OAM %d (slot %d sprite %d, smap %04x vram %04x) tile %03x attr %02x size %d at %d,%d reads tile %02x = cell "
                     "%d owned by %02x",
                     i, s, ram16(SYM_ENTITY_SPRITE_IDS + 2 * s), ram16(SYM_ENTITY_SPRITEMAP_POINTER_LOW + 2 * s),
                     ram16(SYM_ENTITY_VRAM_ADDRESS + 2 * s), tile, attr, w, g_obj_frame.group_x[i], g_obj_frame.group_y[i], t,
                     cell, own);
        break;
      }
  }
  memcpy(prev_tab, &g_wram[SYM_SPRITE_VRAM_TABLE], 88);
  /* glitches that matter last: report once a mismatch has held for 3 frames */
  streak = msg[0] ? streak + 1 : 0;
  if (streak == 3) {
    uint32_t h = 2166136261u;
    for (const char *p = msg; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (h != last_report) eb_log("FOREIGN TILES f%llu: %s", (unsigned long long)g_frame_count, msg);
    last_report = h;
  }
}

/* EB_ENT_LOG: every DMA into sprite VRAM ($4000-$5FFF words), with the 2x2-tile cells it
 * touches and their owners in SPRITE_VRAM_TABLE (80|slot, 00 = free) */
void eb_obj_dma_log(uint32_t src, uint32_t size, uint16_t vmadd, uint8_t dmap) {
  static int on = -1;
  if (on < 0) on = SDL_getenv("EB_ENT_LOG") != NULL;
  if (!on) return;
  uint32_t a = vmadd & 0x7FFF;
  if (a < 0x4000 || a >= 0x6000) return;
  uint32_t words = (dmap & 7) == 1 ? size / 2 : size;
  char cells[160];
  int n = 0, last = -1;
  cells[0] = 0;
  for (uint32_t w = 0; w < words && n < 140; w += 16) {
    uint32_t t = (a + w - 0x4000) >> 4;
    int cell = (int)((t >> 5) * 8 + ((t & 15) >> 1));
    if (cell == last || cell >= 88) continue;
    last = cell;
    n += SDL_snprintf(cells + n, sizeof cells - (size_t)n, " c%d=%02x", cell, g_wram[SYM_SPRITE_VRAM_TABLE + cell]);
  }
  eb_log("objdma f%llu: %06X +%u -> %04X%s%s", (unsigned long long)g_frame_count, src, size, a, g_in_interrupt ? " (nmi)" : "",
         cells);
}

void game_after_frame(void) {
  log_entity_changes();
  check_entity_overlaps();
  check_piece_ownership();
  static GameScene last = (GameScene)-1;
  GameScene sc = game_scene();
  if (sc != last && g_log_level >= 1) {
    static const char *k[] = {"other", "overworld", "battle"};
    eb_log("scene: %s (frame %llu, mode %d, bg1sc %02x bg2sc %02x tm %02x)", k[sc],
           (unsigned long long)g_frame_count, g_ppu.bgmode & 7, g_ppu.bgsc[0], g_ppu.bgsc[1], g_ppu.tm);
    last = sc;
  }
}
