/* S-DSP: 8 BRR voices with ADSR/GAIN envelopes, noise, pitch modulation, 8-tap echo.
 * Output: 32 kHz stereo int16 into a ring buffer drained by the host audio code. */
#include <math.h>

#include "snes.h"
#include "spc.h"

#define RING 16384 /* stereo frames */

typedef struct Voice {
  uint16_t brr_addr;   /* current block */
  int brr_offset;      /* 1..8: next byte pair within the block */
  int16_t buf[12];     /* decoded samples ring (3 groups of 4) */
  int buf_pos;
  uint32_t interp_pos; /* 4.12 fixed point */
  int env;             /* 0..0x7FF */
  int env_mode;        /* 0 attack 1 decay 2 sustain 3 release */
  int kon_delay;
  int16_t out;         /* last output (for PMON) */
  uint8_t header;
  int hidden_env;
} Voice;

static uint8_t s_regs[128];
static Voice s_v[8];
static int s_counter;
static int s_noise = 0x4000;
static int16_t s_echo_hist[8][2];
static int s_echo_pos, s_echo_hist_pos;
static int s_echo_len;
static int16_t s_ring[RING * 2];
static int s_ring_w, s_ring_r;
static int16_t s_gauss[512];
static uint8_t s_kon_pending, s_endx, s_konlatch;
float g_audio_volume = 1.0f;

static const uint16_t k_rate[32] = {0,    2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256,
                                    192,  160,  128,  96,   80,   64,  48,  40,  32,  24,  20,
                                    16,   12,   10,   8,    6,    5,   4,   3,   2,   1};
static const uint16_t k_offset[32] = {0,   0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0,
                                      1040, 536, 0, 1040, 536, 0,   1040, 536, 0, 1040, 536,
                                      0,   1040, 536, 0, 1040, 536, 0,   1040, 0, 0};

static void build_gauss(void) {
  /* 4-tap interpolation kernel, table layout like the hardware's (index by 8-bit fraction);
   * weights sum to ~2048. A Gaussian of sigma 0.56 sample is close to the DSP's soft response. */
  const double sigma = 0.56;
  for (int i = 0; i < 256; i++) {
    double f = i / 256.0;
    double d[4] = {1.0 + f, f, 1.0 - f, 2.0 - f};
    double w[4], s = 0;
    for (int k = 0; k < 4; k++) {
      w[k] = exp(-(d[k] * d[k]) / (2 * sigma * sigma));
      s += w[k];
    }
    for (int k = 0; k < 4; k++) w[k] = w[k] * 2048.0 / s;
    /* hardware order: out = g[255-i]*s0 + g[511-i]*s1 + g[256+i]*s2 + g[i]*s3 */
    s_gauss[255 - i] = (int16_t)lrint(w[0]);
    s_gauss[511 - i] = (int16_t)lrint(w[1]);
    s_gauss[256 + i] = (int16_t)lrint(w[2]);
    s_gauss[i] = (int16_t)lrint(w[3]);
  }
}

void dsp_reset(void) {
  memset(s_regs, 0, sizeof s_regs);
  memset(s_v, 0, sizeof s_v);
  memset(s_echo_hist, 0, sizeof s_echo_hist);
  s_regs[0x6C] = 0xE0;
  s_counter = 0;
  s_noise = 0x4000;
  s_echo_pos = s_echo_hist_pos = 0;
  s_echo_len = 0;
  s_ring_w = s_ring_r = 0;
  s_kon_pending = s_endx = s_konlatch = 0;
  build_gauss();
}

uint8_t dsp_read(uint8_t reg) {
  reg &= 0x7F;
  int vx = reg >> 4;
  switch (reg & 0x0F) {
    case 0x08: return (uint8_t)(s_v[vx].env >> 4);
    case 0x09: return (uint8_t)(s_v[vx].out >> 8);
  }
  if (reg == 0x7C) return s_endx;
  if (reg == 0x4C) return s_konlatch;
  return s_regs[reg];
}

void dsp_write(uint8_t reg, uint8_t v) {
  reg &= 0x7F;
  s_regs[reg] = v;
  if (reg == 0x4C) {
    s_kon_pending |= v;
    s_konlatch = v;
  }
  if (reg == 0x7C) s_endx = 0;
}

static inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static void brr_decode_group(Voice *vc) {
  /* decode 4 samples (2 bytes) of the current block */
  uint8_t *ram = g_spc.ram;
  uint8_t hdr = vc->header;
  int shift = hdr >> 4, filter = (hdr >> 2) & 3;
  for (int k = 0; k < 2; k++) {
    uint8_t byte = ram[(uint16_t)(vc->brr_addr + vc->brr_offset + k)];
    for (int nib = 0; nib < 2; nib++) {
      int s = nib == 0 ? (int8_t)byte >> 4 : (int8_t)(byte << 4) >> 4;
      if (shift <= 12) s = (s << shift) >> 1;
      else s = (s >> 3) << 11; /* -2048 or 0 */
      int p1 = vc->buf[(vc->buf_pos + 11) % 12];
      int p2 = vc->buf[(vc->buf_pos + 10) % 12] >> 1;
      switch (filter) {
        case 1: s += p1 >> 1; s += (-p1) >> 5; break;
        case 2: s += p1; s -= p2; s += p2 >> 4; s += (p1 * -3) >> 6; break;
        case 3: s += p1; s -= p2; s += (p1 * -13) >> 7; s += (p2 * 3) >> 4; break;
      }
      s = clamp16(s);
      s = (int16_t)(s * 2);
      vc->buf[vc->buf_pos] = (int16_t)s;
      vc->buf_pos = (vc->buf_pos + 1) % 12;
    }
  }
}

static uint16_t dir_entry(int srcn, int loop) {
  uint16_t a = (uint16_t)(s_regs[0x5D] * 0x100 + srcn * 4 + (loop ? 2 : 0));
  return (uint16_t)(g_spc.ram[a] | (g_spc.ram[(uint16_t)(a + 1)] << 8));
}

static void voice_advance_brr(Voice *vc, int vidx) {
  vc->brr_offset += 2;
  if (vc->brr_offset >= 9) {
    /* end of block */
    if (vc->header & 1) {
      s_endx |= (uint8_t)(1 << vidx);
      if (vc->header & 2) {
        vc->brr_addr = dir_entry(s_regs[vidx * 16 + 4], 1);
      } else {
        vc->env_mode = 3;
        vc->env = 0;
      }
    } else {
      vc->brr_addr = (uint16_t)(vc->brr_addr + 9);
    }
    vc->header = g_spc.ram[vc->brr_addr];
    vc->brr_offset = 1;
  }
}

static int env_due(int rate) { return rate && ((s_counter + k_offset[rate]) % k_rate[rate]) == 0; }

static void run_envelope(Voice *vc, int vidx) {
  uint8_t adsr1 = s_regs[vidx * 16 + 5], adsr2 = s_regs[vidx * 16 + 6], gain = s_regs[vidx * 16 + 7];
  int env = vc->env;
  if (vc->env_mode == 3) {
    env -= 8;
    if (env < 0) env = 0;
    vc->env = env;
    return;
  }
  int rate;
  if (adsr1 & 0x80) {
    if (vc->env_mode == 0) {
      rate = (adsr1 & 15) * 2 + 1;
      if (env_due(rate)) env += rate == 31 ? 1024 : 32;
      if (env > 0x7FF) {
        env = 0x7FF;
        vc->env_mode = 1;
      }
    } else if (vc->env_mode == 1) {
      rate = ((adsr1 >> 4) & 7) * 2 + 16;
      if (env_due(rate)) env -= ((env - 1) >> 8) + 1;
      if ((env >> 8) <= (adsr2 >> 5)) vc->env_mode = 2;
    } else {
      rate = adsr2 & 31;
      if (env_due(rate)) env -= ((env - 1) >> 8) + 1;
    }
  } else if (!(gain & 0x80)) {
    env = (gain & 0x7F) << 4;
  } else {
    rate = gain & 31;
    if (env_due(rate)) {
      switch ((gain >> 5) & 3) {
        case 0: env -= 32; break;
        case 1: env -= ((env - 1) >> 8) + 1; break;
        case 2: env += 32; break;
        case 3: env += env < 0x600 ? 32 : 8; break;
      }
    }
  }
  if (env < 0) env = 0;
  if (env > 0x7FF) env = 0x7FF;
  vc->env = env;
}

void dsp_sample(void) {
  uint8_t flg = s_regs[0x6C];
  if (--s_counter < 0) s_counter = 0x77FF;
  /* noise */
  if (env_due(flg & 31)) {
    int fb = (s_noise << 13) ^ (s_noise << 14);
    s_noise = (fb & 0x4000) ^ (s_noise >> 1);
  }
  /* key on / off */
  uint8_t koff = s_regs[0x5C];
  if (s_kon_pending) {
    for (int i = 0; i < 8; i++) {
      if (!(s_kon_pending & (1 << i))) continue;
      Voice *vc = &s_v[i];
      vc->brr_addr = dir_entry(s_regs[i * 16 + 4], 0);
      vc->header = g_spc.ram[vc->brr_addr];
      vc->brr_offset = 1;
      vc->buf_pos = 0;
      memset(vc->buf, 0, sizeof vc->buf);
      vc->interp_pos = 0;
      vc->env = 0;
      vc->env_mode = 0;
      vc->kon_delay = 5;
      s_endx &= (uint8_t)~(1 << i);
    }
    s_kon_pending = 0;
  }
  int main_l = 0, main_r = 0, echo_l = 0, echo_r = 0;
  int prev_out = 0;
  for (int i = 0; i < 8; i++) {
    Voice *vc = &s_v[i];
    uint8_t *r = &s_regs[i * 16];
    if ((koff >> i) & 1) vc->env_mode = 3;
    if (flg & 0x80) {
      vc->env_mode = 3;
      vc->env = 0;
    }
    if (vc->kon_delay) {
      if (--vc->kon_delay == 0) {
        /* prime the decoder with the first group */
        brr_decode_group(vc);
        voice_advance_brr(vc, i);
        brr_decode_group(vc);
        voice_advance_brr(vc, i);
        brr_decode_group(vc);
        voice_advance_brr(vc, i);
      }
      vc->out = 0;
      prev_out = 0;
      continue;
    }
    int pitch = (r[2] | (r[3] << 8)) & 0x3FFF;
    if (i > 0 && ((s_regs[0x2D] >> i) & 1)) pitch += ((prev_out >> 5) * pitch) >> 10;
    if (pitch < 0) pitch = 0;
    if (pitch > 0x3FFF) pitch = 0x3FFF;
    /* interpolate */
    int frac = (vc->interp_pos >> 4) & 0xFF;
    int base = (int)(vc->interp_pos >> 12); /* 0..3 within the 4 newest-but-one samples */
    int start = (vc->buf_pos + base) % 12;
    int s0 = vc->buf[start % 12], s1 = vc->buf[(start + 1) % 12];
    int s2 = vc->buf[(start + 2) % 12], s3 = vc->buf[(start + 3) % 12];
    int out;
    if ((s_regs[0x3D] >> i) & 1) {
      out = (int16_t)(s_noise * 2);
    } else {
      out = (s_gauss[255 - frac] * s0) >> 11;
      out += (s_gauss[511 - frac] * s1) >> 11;
      out += (s_gauss[256 + frac] * s2) >> 11;
      out = (int16_t)out;
      out += (s_gauss[frac] * s3) >> 11;
      out = clamp16(out) & ~1;
    }
    run_envelope(vc, i);
    out = (out * vc->env) >> 11;
    vc->out = (int16_t)out;
    prev_out = out;
    int l = (out * (int8_t)r[0]) >> 7, rr = (out * (int8_t)r[1]) >> 7;
    main_l += l;
    main_r += rr;
    if ((s_regs[0x4D] >> i) & 1) {
      echo_l += l;
      echo_r += rr;
    }
    /* advance */
    vc->interp_pos += (uint32_t)pitch;
    while (vc->interp_pos >= 0x4000) {
      vc->interp_pos -= 0x4000;
      brr_decode_group(vc);
      voice_advance_brr(vc, i);
    }
  }
  main_l = clamp16(main_l);
  main_r = clamp16(main_r);
  echo_l = clamp16(echo_l);
  echo_r = clamp16(echo_r);
  /* echo */
  uint16_t esa = (uint16_t)(s_regs[0x6D] << 8);
  int edl = s_regs[0x7D] & 15;
  int len = edl ? edl * 2048 : 4;
  uint16_t ea = (uint16_t)(esa + s_echo_pos);
  uint8_t *ram = g_spc.ram;
  int16_t in_l = (int16_t)(ram[ea] | (ram[(uint16_t)(ea + 1)] << 8));
  int16_t in_r = (int16_t)(ram[(uint16_t)(ea + 2)] | (ram[(uint16_t)(ea + 3)] << 8));
  s_echo_hist_pos = (s_echo_hist_pos + 1) & 7;
  s_echo_hist[s_echo_hist_pos][0] = (int16_t)(in_l >> 1);
  s_echo_hist[s_echo_hist_pos][1] = (int16_t)(in_r >> 1);
  int fir_l = 0, fir_r = 0;
  for (int k = 0; k < 8; k++) {
    int c = (int8_t)s_regs[k * 16 + 0x0F];
    int h = (s_echo_hist_pos + 1 + k) & 7;
    fir_l += (s_echo_hist[h][0] * c) >> 6;
    fir_r += (s_echo_hist[h][1] * c) >> 6;
  }
  fir_l = clamp16(fir_l) & ~1;
  fir_r = clamp16(fir_r) & ~1;
  int out_l = ((main_l * (int8_t)s_regs[0x0C]) >> 7) + ((fir_l * (int8_t)s_regs[0x2C]) >> 7);
  int out_r = ((main_r * (int8_t)s_regs[0x1C]) >> 7) + ((fir_r * (int8_t)s_regs[0x3C]) >> 7);
  if (!(flg & 0x20)) {
    int efb = (int8_t)s_regs[0x0D];
    int wl = clamp16(echo_l + ((fir_l * efb) >> 7)) & ~1;
    int wr = clamp16(echo_r + ((fir_r * efb) >> 7)) & ~1;
    ram[ea] = (uint8_t)wl;
    ram[(uint16_t)(ea + 1)] = (uint8_t)(wl >> 8);
    ram[(uint16_t)(ea + 2)] = (uint8_t)wr;
    ram[(uint16_t)(ea + 3)] = (uint8_t)(wr >> 8);
  }
  s_echo_pos += 4;
  if (s_echo_pos >= len) s_echo_pos = 0;
  s_echo_len = len;
  if (flg & 0x40) out_l = out_r = 0;
  out_l = clamp16((int)(clamp16(out_l) * g_audio_volume));
  out_r = clamp16((int)(clamp16(out_r) * g_audio_volume));
  int next = (s_ring_w + 1) % RING;
  if (next != s_ring_r) {
    s_ring[s_ring_w * 2] = (int16_t)out_l;
    s_ring[s_ring_w * 2 + 1] = (int16_t)out_r;
    s_ring_w = next;
  }
}

int g_audio_peak; /* largest |sample| since the last reset (test logging) */

int apu_take_samples(int16_t *dst, int max_frames) {
  int n = 0;
  while (n < max_frames && s_ring_r != s_ring_w) {
    dst[n * 2] = s_ring[s_ring_r * 2];
    dst[n * 2 + 1] = s_ring[s_ring_r * 2 + 1];
    int a = dst[n * 2] < 0 ? -dst[n * 2] : dst[n * 2];
    if (a > g_audio_peak) g_audio_peak = a;
    s_ring_r = (s_ring_r + 1) % RING;
    n++;
  }
  return n;
}
