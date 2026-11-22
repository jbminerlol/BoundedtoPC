#pragma once
#include <stdint.h>

typedef struct SpcTimer {
  uint8_t target, stage, out;
} SpcTimer;

typedef struct Spc {
  uint8_t ram[0x10000];
  uint8_t a, x, y, sp, psw;
  uint16_t pc;
  uint8_t control, dsp_addr;
  uint8_t in[4];  /* written by the CPU */
  uint8_t out[4]; /* written by the SPC700 */
  SpcTimer timer[3];
  uint64_t cycles, timer_cycles;
  uint32_t timer_div;
  int dsp_cycles;
  int ipl_state;
  uint16_t ipl_dest;
  uint8_t ipl_y;
} Spc;

extern Spc g_spc;

void dsp_reset(void);
uint8_t dsp_read(uint8_t reg);
void dsp_write(uint8_t reg, uint8_t v);
void dsp_sample(void); /* one 32 kHz output sample */
extern float g_audio_volume;
