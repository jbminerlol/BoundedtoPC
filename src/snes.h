/* SNES hardware layer shared declarations (memory map, PPU, DMA, APU, timing). */
#pragma once
#include <stdint.h>
#include <stdio.h>

#include "eb_runtime.h"

/* CPU cycles per frame: 52000 is about what a real SNES gets done in a frame; a bigger budget
 * (cpu_overclock in earthbound.ini) lets busy frames finish instead of slowing down. One frame is
 * always 1/60.1 s of game time and 17,039 SPC700 cycles, whatever the budget. */
extern uint32_t g_frame_cycles;
#define SNES_NOMINAL_FRAME_CYCLES 52000u
#define SNES_FRAME_CYCLES g_frame_cycles
#define SNES_VBLANK_CYCLES (g_frame_cycles * 37u / 262u)
extern uint64_t g_lag_frames; /* map frames where the game had not reached its frame wait */
extern uint64_t g_glitch_nmis; /* NMIs that showed queued tiles with an old sprite list */

#define PPU_MAX_EXT 128       /* widescreen extension per side, pixels */
#define PPU_MAX_W (256 + 2 * PPU_MAX_EXT)
#define PPU_H 224

/* ---- memory ---------------------------------------------------------------------------- */
extern uint8_t g_rom[0x400000];
extern uint32_t g_rom_size;
extern uint8_t g_wram[0x20000];
extern uint8_t g_sram[0x2000];
extern int g_sram_dirty;
int snes_load_rom(const char *path, char *err, size_t errlen);
void snes_reset(void);
void snes_map_memory(void);

/* ---- CPU-side registers ------------------------------------------------------------------ */
typedef struct SnesRegs {
  uint8_t nmitimen, wrio, wrmpya, wrmpyb;
  uint16_t wrdiv, rddiv, rdmpy;
  uint8_t wrdivb;
  uint8_t hdmaen, memsel;
  uint8_t rdnmi_flag, timeup;
  uint16_t joy[4];   /* auto-read results */
  uint32_t wram_addr;
  uint8_t open_bus;
  uint16_t htime, vtime;
} SnesRegs;
extern SnesRegs g_regs;
extern uint16_t g_pad_state[2]; /* current host input, SNES bit layout (B Y Sel St U D L R A X L R) */

/* ---- DMA / HDMA ------------------------------------------------------------------------------ */
typedef struct DmaChan {
  uint8_t dmap, bbad;
  uint16_t a1t;     /* A-bus address */
  uint8_t a1b;      /* A-bus bank */
  uint16_t das;     /* byte count / HDMA indirect address */
  uint8_t dasb;     /* HDMA indirect bank */
  uint16_t a2a;     /* HDMA table address */
  uint8_t ntrl;     /* HDMA line counter */
  uint8_t unused;
  int hdma_do_transfer, hdma_terminated;
} DmaChan;
extern DmaChan g_dma[8];
void dma_run(uint8_t mask);
typedef struct DmaLogEntry {
  uint32_t frame, src, size;
  uint16_t vmadd;
  uint8_t bbad, in_nmi;
} DmaLogEntry;
extern DmaLogEntry g_dma_log[256];
extern int g_dma_log_pos;
void hdma_frame_init(void);
void hdma_line(void);

/* ---- PPU ------------------------------------------------------------------------------------ */
void ppu_reset(void);
uint8_t ppu_read(uint8_t reg);   /* $21xx */
void ppu_write(uint8_t reg, uint8_t v);
void ppu_render_frame(uint32_t *out, int pitch_px, int ext);  /* 32-bit 0xAARRGGBB */
void ppu_latch_counters(void);
extern uint8_t g_vram[0x10000];
extern uint16_t g_cgram[256];
extern uint8_t g_oam[544];

/* ---- APU ------------------------------------------------------------------------------------ */
void apu_reset(void);
uint8_t apu_read_port(int n);
void apu_write_port(int n, uint8_t v);
void apu_catch_up(void);
void apu_end_frame(void);
int apu_take_samples(int16_t *dst, int max_frames); /* stereo frames at 32 kHz */
extern uint64_t g_cpu_total_cycles;                 /* CPU cycles since power-on */

/* ---- frame orchestration ------------------------------------------------------------------- */
typedef struct HostHooks {
  void (*frame)(void);  /* called at each frame boundary after the PPU rendered */
} HostHooks;
extern HostHooks g_host;
extern int g_in_interrupt;
extern uint64_t g_frame_count;
void snes_run(void);   /* runs the game from the reset vector (never returns) */
void snes_interrupt(uint32_t vector_fn_addr);

/* ---- logging ------------------------------------------------------------------------------- */
extern int g_log_level;
void eb_log(const char *fmt, ...);
