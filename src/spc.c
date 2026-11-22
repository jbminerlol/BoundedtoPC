/* SPC700 sound CPU, timers, CPU<->APU ports, and a high-level version of the boot-ROM upload
 * protocol (no IPL ROM image is needed: while the SPC700 sits in the boot ROM, the transfer
 * handshake is carried out here directly against ARAM). */
#include "spc.h"

#include "snes.h"

Spc g_spc;
uint64_t g_cpu_total_cycles;
static uint64_t s_cpu_frame_base;
static double s_ratio;

static const uint8_t k_cycles[256] = {
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8, 2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4, 2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6, 2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5, 2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5, 2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4, 2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9, 2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3, 2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3};

/* ---- memory -------------------------------------------------------------------------- */
static void timer_tick_to(void);

static uint8_t spc_read(uint16_t a) {
  if (a >= 0xF0 && a <= 0xFF) {
    switch (a) {
      case 0xF2: return g_spc.dsp_addr;
      case 0xF3: return dsp_read(g_spc.dsp_addr & 0x7F);
      case 0xF4: case 0xF5: case 0xF6: case 0xF7: return g_spc.in[a - 0xF4];
      case 0xF8: case 0xF9: return g_spc.ram[a];
      case 0xFD: case 0xFE: case 0xFF: {
        timer_tick_to();
        uint8_t v = g_spc.timer[a - 0xFD].out & 15;
        g_spc.timer[a - 0xFD].out = 0;
        return v;
      }
      default: return 0;
    }
  }
  return g_spc.ram[a];
}

static void spc_write(uint16_t a, uint8_t v) {
  if (a >= 0xF0 && a <= 0xFF) {
    switch (a) {
      case 0xF1:
        for (int t = 0; t < 3; t++) {
          if ((v & (1 << t)) && !(g_spc.control & (1 << t))) {
            g_spc.timer[t].stage = 0;
            g_spc.timer[t].out = 0;
          }
        }
        if (v & 0x10) g_spc.in[0] = g_spc.in[1] = 0;
        if (v & 0x20) g_spc.in[2] = g_spc.in[3] = 0;
        g_spc.control = v;
        break;
      case 0xF2: g_spc.dsp_addr = v; break;
      case 0xF3:
        if (!(g_spc.dsp_addr & 0x80)) dsp_write(g_spc.dsp_addr, v);
        break;
      case 0xF4: case 0xF5: case 0xF6: case 0xF7: g_spc.out[a - 0xF4] = v; break;
      case 0xFA: case 0xFB: case 0xFC: g_spc.timer[a - 0xFA].target = v; break;
      default: break;
    }
    g_spc.ram[a] = v; /* writes also land in RAM */
    return;
  }
  g_spc.ram[a] = v;
}

/* ---- timers ---------------------------------------------------------------------------- */
static void timer_tick_to(void) {
  while (g_spc.timer_cycles + 16 <= g_spc.cycles) {
    g_spc.timer_cycles += 16;
    g_spc.timer_div++;
    for (int t = 0; t < 3; t++) {
      if (t < 2 && (g_spc.timer_div & 7)) continue; /* 0/1 tick every 128 cycles, 2 every 16 */
      if (!(g_spc.control & (1 << t))) continue;
      SpcTimer *tm = &g_spc.timer[t];
      tm->stage++;
      if ((uint8_t)tm->stage == tm->target) {
        tm->stage = 0;
        tm->out = (tm->out + 1) & 15;
      }
    }
  }
}

/* ---- boot ROM upload protocol (high level) ------------------------------------------------- */
enum { IPL_WAIT_CC, IPL_WAIT_FIRST, IPL_XFER, IPL_OFF };

static void ipl_enter(void) {
  g_spc.ipl_state = IPL_WAIT_CC;
  for (int i = 1; i <= 0xEF; i++) g_spc.ram[i] = 0;
  g_spc.sp = 0xEF;
  g_spc.out[0] = 0xAA;
  g_spc.out[1] = 0xBB;
}

static void ipl_block_start(void) {
  g_spc.ipl_dest = (uint16_t)(g_spc.in[2] | (g_spc.in[3] << 8));
  g_spc.out[0] = g_spc.in[0];
  if (g_spc.in[1]) {
    g_spc.ipl_state = IPL_WAIT_FIRST;
  } else {
    g_spc.ipl_state = IPL_OFF;
    g_spc.pc = g_spc.ipl_dest;
    g_spc.a = g_spc.x = g_spc.y = 0;
    g_spc.psw = 0x02;
    if (g_log_level >= 2) eb_log("spc: boot upload done, executing $%04X", g_spc.pc);
  }
}

static void ipl_step(void) {
  for (int guard = 0; guard < 4; guard++) {
    switch (g_spc.ipl_state) {
      case IPL_WAIT_CC:
        if (g_spc.in[0] != 0xCC) return;
        ipl_block_start();
        break;
      case IPL_WAIT_FIRST:
        if (g_spc.in[0] != 0) return;
        g_spc.ipl_y = 0;
        g_spc.ipl_state = IPL_XFER;
        break;
      case IPL_XFER: {
        uint8_t in0 = g_spc.in[0];
        if (in0 == g_spc.ipl_y) {
          g_spc.ram[g_spc.ipl_dest] = g_spc.in[1];
          g_spc.out[0] = g_spc.ipl_y;
          g_spc.ipl_y++;
          g_spc.ipl_dest++;
          return; /* one byte per handshake */
        }
        if ((int8_t)(g_spc.ipl_y - in0) < 0) {
          ipl_block_start();
          break;
        }
        return;
      }
      default: return;
    }
  }
}

/* ---- SPC700 core ----------------------------------------------------------------------------- */
#define DP(d) ((uint16_t)(((g_spc.psw & 0x20) ? 0x100 : 0) | (uint8_t)(d)))
#define FN 0x80
#define FV 0x40
#define FP 0x20
#define FB 0x10
#define FH 0x08
#define FI 0x04
#define FZ 0x02
#define FC 0x01

static inline uint8_t fetch(void) { return spc_read(g_spc.pc++); }
static inline uint16_t fetch16(void) {
  uint16_t lo = fetch();
  return (uint16_t)(lo | (fetch() << 8));
}
static inline void setnz(uint8_t v) {
  g_spc.psw = (uint8_t)((g_spc.psw & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ));
}
static inline void setf(uint8_t f, int on) {
  if (on) g_spc.psw |= f;
  else g_spc.psw &= (uint8_t)~f;
}
static inline void push(uint8_t v) {
  g_spc.ram[0x100 | g_spc.sp] = v;
  g_spc.sp--;
}
static inline uint8_t pop(void) {
  g_spc.sp++;
  return g_spc.ram[0x100 | g_spc.sp];
}
static inline uint16_t rdw_dp(uint8_t d) {
  return (uint16_t)(spc_read(DP(d)) | (spc_read(DP((uint8_t)(d + 1))) << 8));
}
static inline void wrw_dp(uint8_t d, uint16_t v) {
  spc_write(DP(d), (uint8_t)v);
  spc_write(DP((uint8_t)(d + 1)), (uint8_t)(v >> 8));
}

static uint8_t op_adc(uint8_t a, uint8_t b) {
  int r = a + b + (g_spc.psw & FC);
  setf(FV, (~(a ^ b) & (a ^ r) & 0x80) != 0);
  setf(FH, ((a ^ b ^ r) & 0x10) != 0);
  setf(FC, r > 0xFF);
  setnz((uint8_t)r);
  return (uint8_t)r;
}
static uint8_t op_sbc(uint8_t a, uint8_t b) { return op_adc(a, (uint8_t)~b); }
static void op_cmp(uint8_t a, uint8_t b) {
  int r = a - b;
  setf(FC, r >= 0);
  setnz((uint8_t)r);
}
static uint8_t op_asl(uint8_t v) { setf(FC, v & 0x80); v <<= 1; setnz(v); return v; }
static uint8_t op_lsr(uint8_t v) { setf(FC, v & 1); v >>= 1; setnz(v); return v; }
static uint8_t op_rol(uint8_t v) {
  int c = g_spc.psw & FC;
  setf(FC, v & 0x80);
  v = (uint8_t)((v << 1) | c);
  setnz(v);
  return v;
}
static uint8_t op_ror(uint8_t v) {
  int c = g_spc.psw & FC;
  setf(FC, v & 1);
  v = (uint8_t)((v >> 1) | (c << 7));
  setnz(v);
  return v;
}

static int branch(int cond) {
  int8_t r = (int8_t)fetch();
  if (cond) {
    g_spc.pc = (uint16_t)(g_spc.pc + r);
    return 2;
  }
  return 0;
}

/* address helpers */
static inline uint16_t a_dpx(void) { return DP(fetch() + g_spc.x); }
static inline uint16_t a_abs(void) { return fetch16(); }
static inline uint16_t a_absx(void) { return (uint16_t)(fetch16() + g_spc.x); }
static inline uint16_t a_absy(void) { return (uint16_t)(fetch16() + g_spc.y); }
static inline uint16_t a_idpx(void) {
  uint8_t d = (uint8_t)(fetch() + g_spc.x);
  return rdw_dp(d);
}
static inline uint16_t a_idpy(void) {
  uint8_t d = fetch();
  return (uint16_t)(rdw_dp(d) + g_spc.y);
}

static int spc_step(void) {
  uint8_t op = fetch();
  int extra = 0;
  uint8_t v, d, s;
  uint16_t a, w;
  switch (op) {
    /* ---- ALU: OR AND EOR CMP ADC SBC rows (x4..x9, x14..x19) -------------------------- */
#define ALU_ROW(base, OPEXPR)                                                     \
  case base + 0x04: d = fetch(); v = spc_read(DP(d)); OPEXPR; break;              \
  case base + 0x05: v = spc_read(a_abs()); OPEXPR; break;                         \
  case base + 0x06: v = spc_read(DP(g_spc.x)); OPEXPR; break;                     \
  case base + 0x07: v = spc_read(a_idpx()); OPEXPR; break;                        \
  case base + 0x08: v = fetch(); OPEXPR; break;                                   \
  case base + 0x14: v = spc_read(a_dpx()); OPEXPR; break;                         \
  case base + 0x15: v = spc_read(a_absx()); OPEXPR; break;                        \
  case base + 0x16: v = spc_read(a_absy()); OPEXPR; break;                        \
  case base + 0x17: v = spc_read(a_idpy()); OPEXPR; break;
#define DO_OR  g_spc.a |= v; setnz(g_spc.a)
#define DO_AND g_spc.a &= v; setnz(g_spc.a)
#define DO_EOR g_spc.a ^= v; setnz(g_spc.a)
#define DO_CMP op_cmp(g_spc.a, v)
#define DO_ADC g_spc.a = op_adc(g_spc.a, v)
#define DO_SBC g_spc.a = op_sbc(g_spc.a, v)
    ALU_ROW(0x00, DO_OR)
    ALU_ROW(0x20, DO_AND)
    ALU_ROW(0x40, DO_EOR)
    ALU_ROW(0x60, DO_CMP)
    ALU_ROW(0x80, DO_ADC)
    ALU_ROW(0xA0, DO_SBC)
    /* memory-to-memory forms: x9 (dd, ds), x18 (dd, #i), x19 ((X), (Y)) */
#define MEM_ROW(base, OPF)                                                                 \
  case base + 0x09: s = fetch(); d = fetch(); v = spc_read(DP(s)); { uint8_t m = spc_read(DP(d)); OPF(DP(d)); } break; \
  case base + 0x18: s = fetch(); d = fetch(); v = s; { uint8_t m = spc_read(DP(d)); OPF(DP(d)); } break;              \
  case base + 0x19: v = spc_read(DP(g_spc.y)); { uint8_t m = spc_read(DP(g_spc.x)); OPF(DP(g_spc.x)); } break;
#define F_OR(addr)  { m |= v; setnz(m); spc_write(addr, m); }
#define F_AND(addr) { m &= v; setnz(m); spc_write(addr, m); }
#define F_EOR(addr) { m ^= v; setnz(m); spc_write(addr, m); }
#define F_CMP(addr) { op_cmp(m, v); }
#define F_ADC(addr) { m = op_adc(m, v); spc_write(addr, m); }
#define F_SBC(addr) { m = op_sbc(m, v); spc_write(addr, m); }
    MEM_ROW(0x00, F_OR)
    MEM_ROW(0x20, F_AND)
    MEM_ROW(0x40, F_EOR)
    MEM_ROW(0x60, F_CMP)
    MEM_ROW(0x80, F_ADC)
    MEM_ROW(0xA0, F_SBC)

    /* ---- TCALL ------------------------------------------------------------------------ */
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1: {
      int n = op >> 4;
      push((uint8_t)(g_spc.pc >> 8));
      push((uint8_t)g_spc.pc);
      a = (uint16_t)(0xFFDE - n * 2);
      g_spc.pc = (uint16_t)(spc_read(a) | (spc_read((uint16_t)(a + 1)) << 8));
      break;
    }
    /* ---- SET1 / CLR1 / BBS / BBC --------------------------------------------------------- */
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2:
      d = fetch(); spc_write(DP(d), (uint8_t)(spc_read(DP(d)) | (1 << (op >> 5)))); break;
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
      d = fetch(); spc_write(DP(d), (uint8_t)(spc_read(DP(d)) & ~(1 << (op >> 5)))); break;
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3:
      d = fetch(); v = spc_read(DP(d)); extra = branch((v >> (op >> 5)) & 1); break;
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3:
      d = fetch(); v = spc_read(DP(d)); extra = branch(!((v >> (op >> 5)) & 1)); break;

    /* ---- branches ---------------------------------------------------------------------------- */
    case 0x10: extra = branch(!(g_spc.psw & FN)); break;
    case 0x30: extra = branch(g_spc.psw & FN); break;
    case 0x50: extra = branch(!(g_spc.psw & FV)); break;
    case 0x70: extra = branch(g_spc.psw & FV); break;
    case 0x90: extra = branch(!(g_spc.psw & FC)); break;
    case 0xB0: extra = branch(g_spc.psw & FC); break;
    case 0xD0: extra = branch(!(g_spc.psw & FZ)); break;
    case 0xF0: extra = branch(g_spc.psw & FZ); break;
    case 0x2F: extra = branch(1); break;

    /* ---- bit/carry ops (m.b) ---------------------------------------------------------------- */
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
      w = fetch16();
      a = w & 0x1FFF;
      int bit = w >> 13;
      int mb = (spc_read(a) >> bit) & 1;
      int c = g_spc.psw & FC;
      switch (op) {
        case 0x0A: setf(FC, c | mb); break;
        case 0x2A: setf(FC, c | !mb); break;
        case 0x4A: setf(FC, c & mb); break;
        case 0x6A: setf(FC, c & !mb); break;
        case 0x8A: setf(FC, c ^ mb); break;
        case 0xAA: setf(FC, mb); break;
        case 0xCA: spc_write(a, (uint8_t)((spc_read(a) & ~(1 << bit)) | (c << bit))); break;
        case 0xEA: spc_write(a, (uint8_t)(spc_read(a) ^ (1 << bit))); break;
      }
      break;
    }
    /* ---- shifts / inc / dec ------------------------------------------------------------------ */
    case 0x0B: d = fetch(); spc_write(DP(d), op_asl(spc_read(DP(d)))); break;
    case 0x0C: a = a_abs(); spc_write(a, op_asl(spc_read(a))); break;
    case 0x1B: a = a_dpx(); spc_write(a, op_asl(spc_read(a))); break;
    case 0x1C: g_spc.a = op_asl(g_spc.a); break;
    case 0x2B: d = fetch(); spc_write(DP(d), op_rol(spc_read(DP(d)))); break;
    case 0x2C: a = a_abs(); spc_write(a, op_rol(spc_read(a))); break;
    case 0x3B: a = a_dpx(); spc_write(a, op_rol(spc_read(a))); break;
    case 0x3C: g_spc.a = op_rol(g_spc.a); break;
    case 0x4B: d = fetch(); spc_write(DP(d), op_lsr(spc_read(DP(d)))); break;
    case 0x4C: a = a_abs(); spc_write(a, op_lsr(spc_read(a))); break;
    case 0x5B: a = a_dpx(); spc_write(a, op_lsr(spc_read(a))); break;
    case 0x5C: g_spc.a = op_lsr(g_spc.a); break;
    case 0x6B: d = fetch(); spc_write(DP(d), op_ror(spc_read(DP(d)))); break;
    case 0x6C: a = a_abs(); spc_write(a, op_ror(spc_read(a))); break;
    case 0x7B: a = a_dpx(); spc_write(a, op_ror(spc_read(a))); break;
    case 0x7C: g_spc.a = op_ror(g_spc.a); break;
    case 0x8B: d = fetch(); v = (uint8_t)(spc_read(DP(d)) - 1); setnz(v); spc_write(DP(d), v); break;
    case 0x8C: a = a_abs(); v = (uint8_t)(spc_read(a) - 1); setnz(v); spc_write(a, v); break;
    case 0x9B: a = a_dpx(); v = (uint8_t)(spc_read(a) - 1); setnz(v); spc_write(a, v); break;
    case 0x9C: g_spc.a--; setnz(g_spc.a); break;
    case 0xAB: d = fetch(); v = (uint8_t)(spc_read(DP(d)) + 1); setnz(v); spc_write(DP(d), v); break;
    case 0xAC: a = a_abs(); v = (uint8_t)(spc_read(a) + 1); setnz(v); spc_write(a, v); break;
    case 0xBB: a = a_dpx(); v = (uint8_t)(spc_read(a) + 1); setnz(v); spc_write(a, v); break;
    case 0xBC: g_spc.a++; setnz(g_spc.a); break;
    case 0x1D: g_spc.x--; setnz(g_spc.x); break;
    case 0x3D: g_spc.x++; setnz(g_spc.x); break;
    case 0xDC: g_spc.y--; setnz(g_spc.y); break;
    case 0xFC: g_spc.y++; setnz(g_spc.y); break;

    /* ---- 16-bit ops ------------------------------------------------------------------------- */
    case 0x1A: d = fetch(); w = (uint16_t)(rdw_dp(d) - 1); wrw_dp(d, w);
      setf(FN, w & 0x8000); setf(FZ, w == 0); break;
    case 0x3A: d = fetch(); w = (uint16_t)(rdw_dp(d) + 1); wrw_dp(d, w);
      setf(FN, w & 0x8000); setf(FZ, w == 0); break;
    case 0x5A: {
      d = fetch(); w = rdw_dp(d);
      uint16_t ya = (uint16_t)((g_spc.y << 8) | g_spc.a);
      int r = ya - w;
      setf(FC, r >= 0); setf(FN, r & 0x8000); setf(FZ, (uint16_t)r == 0);
      break;
    }
    case 0x7A: case 0x9A: {
      d = fetch(); w = rdw_dp(d);
      uint16_t ya = (uint16_t)((g_spc.y << 8) | g_spc.a);
      uint32_t r;
      if (op == 0x7A) {
        r = (uint32_t)ya + w;
        setf(FC, r > 0xFFFF);
        setf(FV, (~(ya ^ w) & (ya ^ r) & 0x8000) != 0);
        setf(FH, ((ya ^ w ^ r) & 0x1000) != 0);
      } else {
        r = (uint32_t)ya - w;
        setf(FC, ya >= w);
        setf(FV, ((ya ^ w) & (ya ^ r) & 0x8000) != 0);
        setf(FH, ((ya ^ w ^ r) & 0x1000) == 0);
      }
      g_spc.a = (uint8_t)r;
      g_spc.y = (uint8_t)(r >> 8);
      setf(FN, r & 0x8000); setf(FZ, (uint16_t)r == 0);
      break;
    }
    case 0xBA: d = fetch(); w = rdw_dp(d); g_spc.a = (uint8_t)w; g_spc.y = (uint8_t)(w >> 8);
      setf(FN, w & 0x8000); setf(FZ, w == 0); break;
    case 0xDA: d = fetch(); wrw_dp(d, (uint16_t)((g_spc.y << 8) | g_spc.a)); break;

    /* ---- compares with X / Y ---------------------------------------------------------------- */
    case 0x1E: op_cmp(g_spc.x, spc_read(a_abs())); break;
    case 0x3E: op_cmp(g_spc.x, spc_read(DP(fetch()))); break;
    case 0x5E: op_cmp(g_spc.y, spc_read(a_abs())); break;
    case 0x7E: op_cmp(g_spc.y, spc_read(DP(fetch()))); break;
    case 0xAD: op_cmp(g_spc.y, fetch()); break;
    case 0xC8: op_cmp(g_spc.x, fetch()); break;

    /* ---- jumps / calls ------------------------------------------------------------------------ */
    case 0x1F: a = a_absx(); g_spc.pc = (uint16_t)(spc_read(a) | (spc_read((uint16_t)(a + 1)) << 8)); break;
    case 0x5F: g_spc.pc = fetch16(); break;
    case 0x3F: a = fetch16(); push((uint8_t)(g_spc.pc >> 8)); push((uint8_t)g_spc.pc); g_spc.pc = a; break;
    case 0x4F: v = fetch(); push((uint8_t)(g_spc.pc >> 8)); push((uint8_t)g_spc.pc); g_spc.pc = (uint16_t)(0xFF00 | v); break;
    case 0x6F: { uint8_t lo = pop(); g_spc.pc = (uint16_t)(lo | (pop() << 8)); break; }
    case 0x7F: { g_spc.psw = pop(); uint8_t lo = pop(); g_spc.pc = (uint16_t)(lo | (pop() << 8)); break; }
    case 0x0F:
      push((uint8_t)(g_spc.pc >> 8)); push((uint8_t)g_spc.pc); push(g_spc.psw);
      g_spc.psw = (uint8_t)((g_spc.psw | FB) & ~FI);
      g_spc.pc = (uint16_t)(spc_read(0xFFDE) | (spc_read(0xFFDF) << 8));
      break;
    case 0x2E: d = fetch(); v = spc_read(DP(d)); extra = branch(g_spc.a != v); break;
    case 0xDE: a = a_dpx(); v = spc_read(a); extra = branch(g_spc.a != v); break;
    case 0x6E: d = fetch(); v = (uint8_t)(spc_read(DP(d)) - 1); spc_write(DP(d), v); extra = branch(v != 0); break;
    case 0xFE: g_spc.y--; extra = branch(g_spc.y != 0); break;

    /* ---- stack ---------------------------------------------------------------------------- */
    case 0x0D: push(g_spc.psw); break;
    case 0x2D: push(g_spc.a); break;
    case 0x4D: push(g_spc.x); break;
    case 0x6D: push(g_spc.y); break;
    case 0x8E: g_spc.psw = pop(); break;
    case 0xAE: g_spc.a = pop(); break;
    case 0xCE: g_spc.x = pop(); break;
    case 0xEE: g_spc.y = pop(); break;

    /* ---- test-and-set ------------------------------------------------------------------------ */
    case 0x0E: a = a_abs(); v = spc_read(a); setnz((uint8_t)(g_spc.a - v)); spc_write(a, v | g_spc.a); break;
    case 0x4E: a = a_abs(); v = spc_read(a); setnz((uint8_t)(g_spc.a - v)); spc_write(a, (uint8_t)(v & ~g_spc.a)); break;

    /* ---- flags -------------------------------------------------------------------------------- */
    case 0x20: g_spc.psw &= (uint8_t)~FP; break;
    case 0x40: g_spc.psw |= FP; break;
    case 0x60: g_spc.psw &= (uint8_t)~FC; break;
    case 0x80: g_spc.psw |= FC; break;
    case 0xA0: g_spc.psw |= FI; break;
    case 0xC0: g_spc.psw &= (uint8_t)~FI; break;
    case 0xE0: g_spc.psw &= (uint8_t)~(FV | FH); break;
    case 0xED: g_spc.psw ^= FC; break;

    /* ---- MOV ---------------------------------------------------------------------------------- */
    case 0x5D: g_spc.x = g_spc.a; setnz(g_spc.x); break;
    case 0x7D: g_spc.a = g_spc.x; setnz(g_spc.a); break;
    case 0xDD: g_spc.a = g_spc.y; setnz(g_spc.a); break;
    case 0xFD: g_spc.y = g_spc.a; setnz(g_spc.y); break;
    case 0x9D: g_spc.x = g_spc.sp; setnz(g_spc.x); break;
    case 0xBD: g_spc.sp = g_spc.x; break;
    case 0x8D: g_spc.y = fetch(); setnz(g_spc.y); break;
    case 0xCD: g_spc.x = fetch(); setnz(g_spc.x); break;
    case 0xE8: g_spc.a = fetch(); setnz(g_spc.a); break;
    case 0x8F: s = fetch(); d = fetch(); spc_write(DP(d), s); break;
    case 0xFA: s = fetch(); d = fetch(); spc_write(DP(d), spc_read(DP(s))); break;
    case 0xAF: spc_write(DP(g_spc.x), g_spc.a); g_spc.x++; break;
    case 0xBF: g_spc.a = spc_read(DP(g_spc.x)); g_spc.x++; setnz(g_spc.a); break;
    case 0xC4: spc_write(DP(fetch()), g_spc.a); break;
    case 0xC5: spc_write(a_abs(), g_spc.a); break;
    case 0xC6: spc_write(DP(g_spc.x), g_spc.a); break;
    case 0xC7: spc_write(a_idpx(), g_spc.a); break;
    case 0xC9: spc_write(a_abs(), g_spc.x); break;
    case 0xCB: spc_write(DP(fetch()), g_spc.y); break;
    case 0xCC: spc_write(a_abs(), g_spc.y); break;
    case 0xD4: spc_write(a_dpx(), g_spc.a); break;
    case 0xD5: spc_write(a_absx(), g_spc.a); break;
    case 0xD6: spc_write(a_absy(), g_spc.a); break;
    case 0xD7: spc_write(a_idpy(), g_spc.a); break;
    case 0xD8: spc_write(DP(fetch()), g_spc.x); break;
    case 0xD9: spc_write(DP(fetch() + g_spc.y), g_spc.x); break;
    case 0xDB: spc_write(a_dpx(), g_spc.y); break;
    case 0xE4: g_spc.a = spc_read(DP(fetch())); setnz(g_spc.a); break;
    case 0xE5: g_spc.a = spc_read(a_abs()); setnz(g_spc.a); break;
    case 0xE6: g_spc.a = spc_read(DP(g_spc.x)); setnz(g_spc.a); break;
    case 0xE7: g_spc.a = spc_read(a_idpx()); setnz(g_spc.a); break;
    case 0xE9: g_spc.x = spc_read(a_abs()); setnz(g_spc.x); break;
    case 0xEB: g_spc.y = spc_read(DP(fetch())); setnz(g_spc.y); break;
    case 0xEC: g_spc.y = spc_read(a_abs()); setnz(g_spc.y); break;
    case 0xF4: g_spc.a = spc_read(a_dpx()); setnz(g_spc.a); break;
    case 0xF5: g_spc.a = spc_read(a_absx()); setnz(g_spc.a); break;
    case 0xF6: g_spc.a = spc_read(a_absy()); setnz(g_spc.a); break;
    case 0xF7: g_spc.a = spc_read(a_idpy()); setnz(g_spc.a); break;
    case 0xF8: g_spc.x = spc_read(DP(fetch())); setnz(g_spc.x); break;
    case 0xF9: g_spc.x = spc_read(DP(fetch() + g_spc.y)); setnz(g_spc.x); break;
    case 0xFB: g_spc.y = spc_read(a_dpx()); setnz(g_spc.y); break;

    /* ---- misc ------------------------------------------------------------------------------- */
    case 0x9E: {
      uint16_t ya = (uint16_t)((g_spc.y << 8) | g_spc.a);
      uint8_t x = g_spc.x;
      setf(FV, g_spc.y >= x);
      setf(FH, (g_spc.y & 15) >= (x & 15));
      if (g_spc.y < (x << 1)) {
        g_spc.a = (uint8_t)(ya / x);
        g_spc.y = (uint8_t)(ya % x);
      } else {
        g_spc.a = (uint8_t)(255 - (ya - (x << 9)) / (256 - x));
        g_spc.y = (uint8_t)(x + (ya - (x << 9)) % (256 - x));
      }
      setnz(g_spc.a);
      break;
    }
    case 0xCF: {
      uint16_t r = (uint16_t)(g_spc.y * g_spc.a);
      g_spc.a = (uint8_t)r;
      g_spc.y = (uint8_t)(r >> 8);
      setnz(g_spc.y);
      break;
    }
    case 0x9F: g_spc.a = (uint8_t)((g_spc.a >> 4) | (g_spc.a << 4)); setnz(g_spc.a); break;
    case 0xDF:
      if ((g_spc.psw & FC) || g_spc.a > 0x99) { g_spc.a += 0x60; g_spc.psw |= FC; }
      if ((g_spc.psw & FH) || (g_spc.a & 15) > 9) g_spc.a += 6;
      setnz(g_spc.a);
      break;
    case 0xBE:
      if (!(g_spc.psw & FC) || g_spc.a > 0x99) { g_spc.a -= 0x60; g_spc.psw &= (uint8_t)~FC; }
      if (!(g_spc.psw & FH) || (g_spc.a & 15) > 9) g_spc.a -= 6;
      setnz(g_spc.a);
      break;
    case 0x00: break;
    case 0xEF: case 0xFF: g_spc.pc--; break; /* SLEEP / STOP: stay here */
    default: break;
  }
  return k_cycles[op] + extra;
}

/* ---- run / sync ------------------------------------------------------------------------------- */
static void spc_run_until(uint64_t target) {
  while (g_spc.cycles < target) {
    int c;
    if (g_spc.ipl_state != IPL_OFF) {
      c = 32; /* idle in the (high level) boot ROM */
    } else {
      if (g_spc.pc >= 0xFFC0 && (g_spc.control & 0x80)) {
        ipl_enter();
        continue;
      }
      c = spc_step();
    }
    g_spc.cycles += (uint64_t)c;
    g_spc.dsp_cycles += c;
    while (g_spc.dsp_cycles >= 32) {
      g_spc.dsp_cycles -= 32;
      dsp_sample();
    }
  }
  timer_tick_to();
}

void apu_catch_up(void) {
  g_cpu_total_cycles = s_cpu_frame_base + cpu.cyc;
  uint64_t target = (uint64_t)((double)g_cpu_total_cycles * s_ratio);
  if (target > g_spc.cycles + 1024000) g_spc.cycles = target - 1024000; /* never run >1 s at once */
  spc_run_until(target);
}

void apu_end_frame(void) {
  /* cpu.cyc is about to drop by one frame: fold it into the base */
  apu_catch_up();
  s_cpu_frame_base += SNES_FRAME_CYCLES;
}

uint8_t apu_read_port(int n) {
  apu_catch_up();
  if (g_spc.ipl_state != IPL_OFF) ipl_step();
  return g_spc.out[n & 3];
}

void apu_write_port(int n, uint8_t v) {
  apu_catch_up();
  g_spc.in[n & 3] = v;
}

void apu_reset(void) {
  memset(&g_spc, 0, sizeof g_spc);
  s_ratio = 1024000.0 / (60.0988 * (double)SNES_FRAME_CYCLES);
  s_cpu_frame_base = 0;
  g_spc.control = 0x80;
  g_spc.timer[0].target = g_spc.timer[1].target = g_spc.timer[2].target = 0;
  dsp_reset();
  ipl_enter();
}
