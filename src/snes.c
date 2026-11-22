/* SNES memory map, CPU-side registers, DMA/HDMA (HiROM cart: EarthBound). */
#include "snes.h"

#include <stdarg.h>

#include "ppu.h"
#include <stdlib.h>

uint8_t g_rom[0x400000];
uint32_t g_rom_size;
uint8_t g_wram[0x20000];
uint8_t g_sram[0x2000];
int g_sram_dirty;
SnesRegs g_regs;
DmaChan g_dma[8];
uint16_t g_pad_state[2];
uint8_t *mem_rmap[2048];
uint8_t *mem_wmap[2048];
int g_log_level = 1;

static uint8_t s_open_page[0x2000]; /* reads of unmapped ROM-space pages */

void eb_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

int snes_load_rom(const char *path, char *err, size_t errlen) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(err, errlen, "cannot open %s", path);
    return 0;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  long skip = (n % 1024 == 512) ? 512 : 0; /* copier header */
  if (n - skip != 0x300000) {
    fclose(f);
    snprintf(err, errlen, "%s: expected a 3 MB EarthBound (USA) ROM, got %ld bytes", path, n);
    return 0;
  }
  fseek(f, skip, SEEK_SET);
  size_t got = fread(g_rom, 1, (size_t)(n - skip), f);
  fclose(f);
  g_rom_size = (uint32_t)got;
  if (memcmp(g_rom + 0xFFC0, "EARTH BOUND", 11) != 0) {
    snprintf(err, errlen, "%s is not EarthBound (USA) (header title differs)", path);
    return 0;
  }
  /* The recompiled code was generated from the retail US ROM, so only that exact dump works. */
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < got; i++) {
    crc ^= g_rom[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  crc = ~crc;
  if (crc != 0xDC9BB451u) {
    snprintf(err, errlen,
             "%s is not the retail EarthBound (USA) ROM this port was built from "
             "(CRC32 %08X, expected DC9BB451). Hacked or translated ROMs are not supported.",
             path, crc);
    return 0;
  }
  /* 24 Mbit: mirror the last MB into the 4 MB address space ($F0-$FF) */
  memcpy(g_rom + 0x300000, g_rom + 0x200000, 0x100000);
  return 1;
}

void snes_map_memory(void) {
  for (int bank = 0; bank < 256; bank++) {
    for (int page = 0; page < 8; page++) {
      int idx = bank * 8 + page;
      uint8_t *r = NULL, *w = NULL;
      uint32_t off = (uint32_t)page * 0x2000;
      if (bank == 0x7E || bank == 0x7F) {
        r = w = g_wram + (uint32_t)(bank - 0x7E) * 0x10000 + off;
      } else if ((bank & 0x7F) < 0x40) {
        if (page == 0) {
          r = w = g_wram + off; /* low 8 KB mirror */
        } else if (page == 3) {
          /* 8 KB SRAM at $6000-$7FFF, repeated in every bank $20-$3F / $A0-$BF like the real
           * cart. The game relies on it: CHECK_HARDWARE writes $31:7FF0 and expects to read
           * the value back at $30:7FF0 (a copier with separate SRAM banks fails that test and
           * gets the anti-piracy screen), and sram.asm keeps data at $31:6000 / $32:6000. */
          if ((bank & 0x7F) >= 0x20) r = w = g_sram;
        } else if (page >= 4) {
          r = g_rom + (uint32_t)(bank & 0x3F) * 0x10000 + off;
        }
      } else {
        r = g_rom + (uint32_t)(bank & 0x3F) * 0x10000 + off; /* $40-$7D, $C0-$FF */
      }
      mem_rmap[idx] = r;
      mem_wmap[idx] = w;
    }
  }
  (void)s_open_page;
}

static void mul_div_write(uint8_t reg, uint8_t v) {
  switch (reg) {
    case 0x02: g_regs.wrmpya = v; break;
    case 0x03:
      g_regs.wrmpyb = v;
      g_regs.rdmpy = (uint16_t)(g_regs.wrmpya * v);
      g_regs.rddiv = v;
      break;
    case 0x04: g_regs.wrdiv = (uint16_t)((g_regs.wrdiv & 0xFF00) | v); break;
    case 0x05: g_regs.wrdiv = (uint16_t)((g_regs.wrdiv & 0x00FF) | (v << 8)); break;
    case 0x06:
      g_regs.wrdivb = v;
      if (v == 0) {
        g_regs.rddiv = 0xFFFF;
        g_regs.rdmpy = g_regs.wrdiv;
      } else {
        g_regs.rddiv = (uint16_t)(g_regs.wrdiv / v);
        g_regs.rdmpy = (uint16_t)(g_regs.wrdiv % v);
      }
      break;
  }
}

static uint8_t s_joy_latch, s_joy_shift[2];

uint8_t io_rd8(uint32_t a) {
  uint32_t bank = (a >> 16) & 0xFF;
  uint32_t off = a & 0xFFFF;
  uint8_t v = g_regs.open_bus;
  if ((bank & 0x7F) >= 0x40 || off >= 0x8000) return v; /* unmapped (SRAM mirrors etc.) */
  if (off >= 0x2100 && off < 0x2200) {
    uint8_t r = (uint8_t)off;
    if (r >= 0x40 && r < 0x80) {
      v = apu_read_port(r & 3);
    } else if (r == 0x80) {
      v = g_wram[g_regs.wram_addr & 0x1FFFF];
      g_regs.wram_addr = (g_regs.wram_addr + 1) & 0x1FFFF;
    } else if (r < 0x40) {
      v = ppu_read(r);
    }
  } else if (off == 0x4016 || off == 0x4017) {
    int p = off & 1;
    v = (uint8_t)((g_regs.open_bus & 0xFC) | (s_joy_shift[p] >> 7 & 1));
    s_joy_shift[p] <<= 1;
    (void)s_joy_latch;
  } else if (off >= 0x4210 && off < 0x4220) {
    switch (off & 0xFF) {
      case 0x10: v = (uint8_t)((g_regs.rdnmi_flag ? 0x80 : 0) | 0x02); g_regs.rdnmi_flag = 0; break;
      case 0x11: v = g_regs.timeup; g_regs.timeup = 0; break;
      case 0x12: {
        static uint8_t hb;
        hb ^= 0x40; /* toggling hblank so hblank waits finish */
        v = (uint8_t)((cpu.cyc < SNES_VBLANK_CYCLES ? 0x80 : 0) | hb);
        break;
      }
      case 0x13: v = g_regs.wrio; break;
      case 0x14: v = (uint8_t)g_regs.rddiv; break;
      case 0x15: v = (uint8_t)(g_regs.rddiv >> 8); break;
      case 0x16: v = (uint8_t)g_regs.rdmpy; break;
      case 0x17: v = (uint8_t)(g_regs.rdmpy >> 8); break;
      default: {
        int j = ((off & 0xFF) - 0x18) >> 1;
        v = (off & 1) ? (uint8_t)(g_regs.joy[j] >> 8) : (uint8_t)g_regs.joy[j];
        break;
      }
    }
  } else if (off >= 0x4300 && off < 0x4380) {
    DmaChan *c = &g_dma[(off >> 4) & 7];
    switch (off & 0xF) {
      case 0x0: v = c->dmap; break;
      case 0x1: v = c->bbad; break;
      case 0x2: v = (uint8_t)c->a1t; break;
      case 0x3: v = (uint8_t)(c->a1t >> 8); break;
      case 0x4: v = c->a1b; break;
      case 0x5: v = (uint8_t)c->das; break;
      case 0x6: v = (uint8_t)(c->das >> 8); break;
      case 0x7: v = c->dasb; break;
      case 0x8: v = (uint8_t)c->a2a; break;
      case 0x9: v = (uint8_t)(c->a2a >> 8); break;
      case 0xA: v = c->ntrl; break;
      default: v = c->unused; break;
    }
  }
  g_regs.open_bus = v;
  return v;
}

void io_wr8(uint32_t a, uint8_t v) {
  uint32_t bank = (a >> 16) & 0xFF;
  uint32_t off = a & 0xFFFF;
  if ((bank & 0x7F) >= 0x40 || off >= 0x8000) return; /* ROM / unmapped */
  if (off >= 0x2100 && off < 0x2200) {
    uint8_t r = (uint8_t)off;
    if (r >= 0x40 && r < 0x80) {
      apu_write_port(r & 3, v);
    } else if (r >= 0x80 && r <= 0x83) {
      switch (r) {
        case 0x80:
          g_wram[g_regs.wram_addr & 0x1FFFF] = v;
          g_regs.wram_addr = (g_regs.wram_addr + 1) & 0x1FFFF;
          break;
        case 0x81: g_regs.wram_addr = (g_regs.wram_addr & 0x1FF00) | v; break;
        case 0x82: g_regs.wram_addr = (g_regs.wram_addr & 0x100FF) | ((uint32_t)v << 8); break;
        case 0x83: g_regs.wram_addr = (g_regs.wram_addr & 0x0FFFF) | ((uint32_t)(v & 1) << 16); break;
      }
    } else if (r < 0x40) {
      ppu_write(r, v);
    }
  } else if (off == 0x4016) {
    if (v & 1) {
      s_joy_shift[0] = (uint8_t)(g_pad_state[0] >> 8);
      s_joy_shift[1] = (uint8_t)(g_pad_state[1] >> 8);
    }
    s_joy_latch = v & 1;
  } else if (off >= 0x4200 && off < 0x4210) {
    switch (off & 0xFF) {
      case 0x00: g_regs.nmitimen = v; break;
      case 0x01:
        if ((g_regs.wrio & 0x80) && !(v & 0x80)) ppu_latch_counters();
        g_regs.wrio = v;
        break;
      case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: mul_div_write(off & 0xFF, v); break;
      case 0x07: g_regs.htime = (uint16_t)((g_regs.htime & 0x100) | v); break;
      case 0x08: g_regs.htime = (uint16_t)((g_regs.htime & 0xFF) | ((v & 1) << 8)); break;
      case 0x09: g_regs.vtime = (uint16_t)((g_regs.vtime & 0x100) | v); break;
      case 0x0A: g_regs.vtime = (uint16_t)((g_regs.vtime & 0xFF) | ((v & 1) << 8)); break;
      case 0x0B: dma_run(v); break;
      case 0x0C: g_regs.hdmaen = v; break;
      case 0x0D: g_regs.memsel = v; break;
    }
  } else if (off >= 0x4300 && off < 0x4380) {
    DmaChan *c = &g_dma[(off >> 4) & 7];
    switch (off & 0xF) {
      case 0x0: c->dmap = v; break;
      case 0x1: c->bbad = v; break;
      case 0x2: c->a1t = (uint16_t)((c->a1t & 0xFF00) | v); break;
      case 0x3: c->a1t = (uint16_t)((c->a1t & 0x00FF) | (v << 8)); break;
      case 0x4: c->a1b = v; break;
      case 0x5: c->das = (uint16_t)((c->das & 0xFF00) | v); break;
      case 0x6: c->das = (uint16_t)((c->das & 0x00FF) | (v << 8)); break;
      case 0x7: c->dasb = v; break;
      case 0x8: c->a2a = (uint16_t)((c->a2a & 0xFF00) | v); break;
      case 0x9: c->a2a = (uint16_t)((c->a2a & 0x00FF) | (v << 8)); break;
      case 0xA: c->ntrl = v; break;
      default: c->unused = v; break;
    }
  }
}

/* B-bus access for DMA (registers $2100-$21FF) */
static void bbus_write(uint8_t reg, uint8_t v) { io_wr8(0x002100u | reg, v); }
static uint8_t bbus_read(uint8_t reg) { return io_rd8(0x002100u | reg); }

static const uint8_t k_dma_pattern[8][4] = {
    {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
    {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}};
static const uint8_t k_dma_unit[8] = {1, 2, 2, 4, 4, 4, 2, 4};

static int dma_abus_ok(uint32_t a) {
  /* DMA cannot access B-bus registers or the CPU's own registers through the A-bus */
  uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
  if ((bank & 0x7F) < 0x40 && ((off >= 0x2100 && off < 0x2200) || (off >= 0x4000 && off < 0x4400)))
    return 0;
  return 1;
}

/* ring log of recent DMA transfers (bug reports, F12) */
DmaLogEntry g_dma_log[256];
int g_dma_log_pos;

void dma_run(uint8_t mask) {
  for (int ch = 0; ch < 8; ch++) {
    if (!(mask & (1 << ch))) continue;
    DmaChan *c = &g_dma[ch];
    {
      DmaLogEntry *le = &g_dma_log[g_dma_log_pos++ & 255];
      le->frame = (uint32_t)g_frame_count;
      le->src = ((uint32_t)c->a1b << 16) | c->a1t;
      le->bbad = c->bbad;
      le->size = c->das ? c->das : 0x10000;
      le->vmadd = g_ppu.vmadd;
      le->in_nmi = (uint8_t)g_in_interrupt;
    }
    int mode = c->dmap & 7;
    int step = (c->dmap & 0x08) ? 0 : ((c->dmap & 0x10) ? -1 : 1);
    int to_a = (c->dmap & 0x80) != 0;
    uint32_t count = c->das ? c->das : 0x10000;
    uint32_t i = 0;
    cpu.cyc += 3 + count * 8 / 6;
    if (!to_a && (c->bbad == 0x18 || c->bbad == 0x19)) {
      extern void eb_obj_dma_log(uint32_t src, uint32_t size, uint16_t vmadd, uint8_t dmap);
      eb_obj_dma_log(((uint32_t)c->a1b << 16) | c->a1t, count, g_ppu.vmadd, c->dmap); /* EB_ENT_LOG only */
    }
    if (!to_a && c->bbad == 0x04) {
      extern void eb_oam_dma_source(uint32_t abus);
      eb_oam_dma_source(((uint32_t)c->a1b << 16) | c->a1t); /* widescreen sprite side table */
    }
    while (count) {
      uint8_t breg = (uint8_t)(c->bbad + k_dma_pattern[mode][i & 3]);
      uint32_t abus = ((uint32_t)c->a1b << 16) | c->a1t;
      if (to_a) {
        uint8_t v = bbus_read(breg);
        if (dma_abus_ok(abus)) wr8(abus, v);
      } else {
        uint8_t v = dma_abus_ok(abus) ? rd8(abus) : g_regs.open_bus;
        bbus_write(breg, v);
      }
      c->a1t = (uint16_t)(c->a1t + step);
      i++;
      count--;
    }
    c->das = 0;
  }
}

static void hdma_load_entry(DmaChan *c) {
  uint32_t base = (uint32_t)c->a1b << 16;
  c->ntrl = rd8(base | c->a2a);
  c->a2a++;
  if (c->dmap & 0x40) {
    c->das = rd16(base | c->a2a);
    c->a2a += 2;
  }
  c->hdma_terminated = (c->ntrl == 0);
  c->hdma_do_transfer = 1;
}

void hdma_frame_init(void) {
  for (int ch = 0; ch < 8; ch++) {
    DmaChan *c = &g_dma[ch];
    c->hdma_terminated = 1;
    if (!(g_regs.hdmaen & (1 << ch))) continue;
    c->a2a = c->a1t;
    hdma_load_entry(c);
  }
}

void hdma_line(void) {
  for (int ch = 0; ch < 8; ch++) {
    DmaChan *c = &g_dma[ch];
    if (!(g_regs.hdmaen & (1 << ch)) || c->hdma_terminated) continue;
    if (c->hdma_do_transfer) {
      int mode = c->dmap & 7;
      int n = k_dma_unit[mode];
      for (int i = 0; i < n; i++) {
        uint32_t src;
        if (c->dmap & 0x40) {
          src = ((uint32_t)c->dasb << 16) | c->das;
          c->das++;
        } else {
          src = ((uint32_t)c->a1b << 16) | c->a2a;
          c->a2a++;
        }
        bbus_write((uint8_t)(c->bbad + k_dma_pattern[mode][i]), rd8(src));
      }
    }
    c->ntrl--;
    c->hdma_do_transfer = (c->ntrl & 0x80) != 0;
    if ((c->ntrl & 0x7F) == 0) hdma_load_entry(c);
  }
}

void snes_reset(void) {
  memset(&g_regs, 0, sizeof g_regs);
  memset(g_dma, 0, sizeof g_dma);
  memset(g_wram, 0, sizeof g_wram);
  g_regs.wrio = 0xFF;
  snes_map_memory();
  ppu_reset();
  apu_reset();
}
