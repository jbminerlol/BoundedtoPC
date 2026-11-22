/* Runtime interface for the recompiled EarthBound code (build/gen/eb_gen_*.c).
 *
 * The generated code works on one global 65816 register file (cpu) and the SNES memory map
 * (rd8/wr8 ...). Widths are mostly decided by the recompiler; cpu.M / cpu.XF are kept exact so
 * that dynamic-width code and interrupts see the real processor state.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#define EB_LIKELY(x) __builtin_expect(!!(x), 1)
#define EB_UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct Cpu {
  uint16_t A, X, Y, S, D;
  uint8_t DB, PB;
  uint8_t N, V, Z, C; /* Z = 1 when the zero flag is set */
  uint8_t I, DF;      /* irq disable, decimal */
  uint8_t M, XF;      /* 1 = 8-bit accumulator / index */
  uint8_t E;          /* emulation mode */
  uint32_t cyc;       /* CPU cycles since the start of the current frame */
} Cpu;

extern Cpu cpu;
extern uint16_t g_ws_ext, g_ws_cols; /* widescreen margin (px / tile columns), see enhance.c */
extern uint32_t g_cyc_limit;  /* cycles per frame: POLL() runs the frame boundary past this */
extern uint16_t g_ret_s;      /* S right after the return address of the innermost real call */

/* ---- memory -------------------------------------------------------------------------- */
extern uint8_t *mem_rmap[2048]; /* 8 KB pages over the 24-bit space; NULL = I/O / open bus */
extern uint8_t *mem_wmap[2048];
uint8_t io_rd8(uint32_t a);
void io_wr8(uint32_t a, uint8_t v);

static inline uint8_t rd8(uint32_t a) {
  a &= 0xFFFFFF;
  uint8_t *p = mem_rmap[a >> 13];
  return p ? p[a & 0x1FFF] : io_rd8(a);
}
static inline void wr8(uint32_t a, uint8_t v) {
  a &= 0xFFFFFF;
  uint8_t *p = mem_wmap[a >> 13];
  if (p) p[a & 0x1FFF] = v;
  else io_wr8(a, v);
}
static inline uint16_t rd16(uint32_t a) {
  a &= 0xFFFFFF;
  uint8_t *p = mem_rmap[a >> 13];
  if (EB_LIKELY(p && (a & 0x1FFF) != 0x1FFF)) {
    uint16_t v;
    memcpy(&v, p + (a & 0x1FFF), 2);
    return v;
  }
  return (uint16_t)(rd8(a) | (rd8(a + 1) << 8));
}
static inline void wr16(uint32_t a, uint16_t v) {
  a &= 0xFFFFFF;
  uint8_t *p = mem_wmap[a >> 13];
  if (EB_LIKELY(p && (a & 0x1FFF) != 0x1FFF)) {
    memcpy(p + (a & 0x1FFF), &v, 2);
    return;
  }
  wr8(a, (uint8_t)v);
  wr8(a + 1, (uint8_t)(v >> 8));
}
/* bank-0 accesses (direct page, stack): the second byte wraps inside bank 0 */
static inline uint16_t rd16_b0(uint16_t a) {
  if (EB_LIKELY(a < 0x1FFF)) {
    uint8_t *p = mem_rmap[0];
    uint16_t v;
    memcpy(&v, p + a, 2);
    return v;
  }
  return (uint16_t)(rd8(a) | (rd8((uint16_t)(a + 1)) << 8));
}
static inline void wr16_b0(uint16_t a, uint16_t v) {
  if (EB_LIKELY(a < 0x1FFF)) {
    memcpy(mem_wmap[0] + a, &v, 2);
    return;
  }
  wr8(a, (uint8_t)v);
  wr8((uint16_t)(a + 1), (uint8_t)(v >> 8));
}
static inline uint32_t rd24_b0(uint16_t a) {
  return rd16_b0(a) | ((uint32_t)rd8((uint16_t)(a + 2)) << 16);
}

#define DBASE ((uint32_t)cpu.DB << 16)
#define PBASE ((uint32_t)cpu.PB << 16)

/* canonical ROM address of a code address ($80:9470 -> $C0:9470); 0 if not ROM */
static inline uint32_t eb_canon(uint32_t a) {
  uint32_t bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
  if (bank >= 0xC0) return a & 0xFFFFFF;
  if (bank >= 0x40 && bank <= 0x7D) return ((0xC0 | (bank & 0x3F)) << 16) | off;
  if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x8000)
    return ((0xC0 | (bank & 0x3F)) << 16) | off;
  return 0;
}

/* ---- stack ----------------------------------------------------------------------------- */
static inline void push8(uint8_t v) {
  wr8(cpu.S, v);
  cpu.S--;
  if (EB_UNLIKELY(cpu.E)) cpu.S = 0x100 | (cpu.S & 0xFF);
}
static inline uint8_t pull8(void) {
  cpu.S++;
  if (EB_UNLIKELY(cpu.E)) cpu.S = 0x100 | (cpu.S & 0xFF);
  return rd8(cpu.S);
}
static inline void push16(uint16_t v) {
  push8((uint8_t)(v >> 8));
  push8((uint8_t)v);
}
static inline uint16_t pull16(void) {
  uint8_t lo = pull8();
  return (uint16_t)(lo | (pull8() << 8));
}

/* ---- flags ----------------------------------------------------------------------------- */
#define NZ8(v) (cpu.Z = ((uint8_t)(v) == 0), cpu.N = (uint8_t)(((uint8_t)(v)) >> 7))
#define NZ16(v) (cpu.Z = ((uint16_t)(v) == 0), cpu.N = (uint8_t)(((uint16_t)(v)) >> 15))

static inline uint8_t get_p(void) {
  return (uint8_t)((cpu.N << 7) | (cpu.V << 6) | (cpu.M << 5) | (cpu.XF << 4) | (cpu.DF << 3) |
                   (cpu.I << 2) | (cpu.Z << 1) | cpu.C);
}
static inline void set_p(uint8_t p) {
  cpu.N = (p >> 7) & 1;
  cpu.V = (p >> 6) & 1;
  cpu.M = (p >> 5) & 1;
  cpu.XF = (p >> 4) & 1;
  cpu.DF = (p >> 3) & 1;
  cpu.I = (p >> 2) & 1;
  cpu.Z = (p >> 1) & 1;
  cpu.C = p & 1;
  if (cpu.E) cpu.M = cpu.XF = 1;
  if (cpu.XF) {
    cpu.X &= 0xFF;
    cpu.Y &= 0xFF;
  }
}
static inline void op_rep(uint8_t m) { set_p(get_p() & (uint8_t)~m); }
static inline void op_sep(uint8_t m) { set_p(get_p() | m); }
static inline void op_xce(void) {
  uint8_t c = cpu.C;
  cpu.C = cpu.E;
  cpu.E = c;
  if (cpu.E) {
    cpu.M = cpu.XF = 1;
    cpu.X &= 0xFF;
    cpu.Y &= 0xFF;
    cpu.S = 0x100 | (cpu.S & 0xFF);
  }
}

/* ---- arithmetic -------------------------------------------------------------------------- */
uint16_t eb_adc_dec(uint16_t a, uint16_t v, int wide);
uint16_t eb_sbc_dec(uint16_t a, uint16_t v, int wide);

static inline void adc8(uint16_t v) {
  uint8_t a = (uint8_t)cpu.A;
  if (EB_UNLIKELY(cpu.DF)) {
    cpu.A = (cpu.A & 0xFF00) | (uint8_t)eb_adc_dec(a, (uint8_t)v, 0);
    return;
  }
  uint32_t r = (uint32_t)a + (uint8_t)v + cpu.C;
  cpu.V = (uint8_t)(((~(a ^ v) & (a ^ r)) >> 7) & 1);
  cpu.C = r > 0xFF;
  cpu.A = (cpu.A & 0xFF00) | (uint8_t)r;
  NZ8(r);
}
static inline void adc16(uint16_t v) {
  uint16_t a = cpu.A;
  if (EB_UNLIKELY(cpu.DF)) {
    cpu.A = eb_adc_dec(a, v, 1);
    return;
  }
  uint32_t r = (uint32_t)a + v + cpu.C;
  cpu.V = (uint8_t)(((~(a ^ v) & (a ^ r)) >> 15) & 1);
  cpu.C = r > 0xFFFF;
  cpu.A = (uint16_t)r;
  NZ16(r);
}
static inline void sbc8(uint16_t v) {
  uint8_t a = (uint8_t)cpu.A;
  if (EB_UNLIKELY(cpu.DF)) {
    cpu.A = (cpu.A & 0xFF00) | (uint8_t)eb_sbc_dec(a, (uint8_t)v, 0);
    return;
  }
  uint8_t nv = (uint8_t)~v;
  uint32_t r = (uint32_t)a + nv + cpu.C;
  cpu.V = (uint8_t)(((~(a ^ nv) & (a ^ r)) >> 7) & 1);
  cpu.C = r > 0xFF;
  cpu.A = (cpu.A & 0xFF00) | (uint8_t)r;
  NZ8(r);
}
static inline void sbc16(uint16_t v) {
  uint16_t a = cpu.A;
  if (EB_UNLIKELY(cpu.DF)) {
    cpu.A = eb_sbc_dec(a, v, 1);
    return;
  }
  uint16_t nv = (uint16_t)~v;
  uint32_t r = (uint32_t)a + nv + cpu.C;
  cpu.V = (uint8_t)(((~(a ^ nv) & (a ^ r)) >> 15) & 1);
  cpu.C = r > 0xFFFF;
  cpu.A = (uint16_t)r;
  NZ16(r);
}

/* ---- block moves -------------------------------------------------------------------------- */
void op_mvn(uint8_t src, uint8_t dst);
void op_mvp(uint8_t src, uint8_t dst);

/* ---- control flow ------------------------------------------------------------------------- */
typedef void (*EbFunc)(void);
typedef struct EbFuncEntry {
  uint32_t addr;
  EbFunc fn;
  const char *name;
} EbFuncEntry;
extern const EbFuncEntry eb_func_table[];

void eb_call(uint32_t addr);  /* call the function at addr (PB set from addr) */
void eb_jump(uint32_t addr);  /* tail jump: same as eb_call, the caller returns after it */
uint16_t eb_rts_trick(void);  /* RTS that is a computed jump; returns the next stack word */
uint16_t eb_rtl_trick(void);
void eb_rti_mismatch(void);
void eb_poll(void);           /* frame boundary: render, NMI, audio, input, pacing */
void eb_wai(void);
void eb_fatal(const char *msg);
void eb_setjmp_landing(uint16_t buf);
void **eb_host_jmpbuf(uint16_t buf);

#define CYC(n) (cpu.cyc += (n))
extern uint32_t g_poll_addr; /* code address of the last frame boundary (debugging) */
void eb_poll_at(uint32_t addr);
#define POLL_AT(a)                                             \
  do {                                                         \
    if (EB_UNLIKELY(cpu.cyc >= g_cyc_limit)) eb_poll_at(a);    \
  } while (0)
#define POLL() POLL_AT(0)
extern uint32_t g_last_func; /* last game function entered (crash reports) */
#define ENTER_FUNC(a) \
  do {                \
    g_last_func = (a); \
    POLL_AT(a);       \
  } while (0)
/* native replacements of RTS/RTL-returning game functions end with these */
#define NATIVE_RTS() (cpu.S += 2)
#define NATIVE_RTL() (cpu.S += 3, cpu.PB = rd8(cpu.S))

#define JSR_(ret, fn)          \
  do {                         \
    uint16_t sv_ = g_ret_s;    \
    push16(ret);               \
    g_ret_s = cpu.S;           \
    fn();                      \
    g_ret_s = sv_;             \
  } while (0)
#define JSL_(bank, ret, fn)    \
  do {                         \
    uint16_t sv_ = g_ret_s;    \
    push8(cpu.PB);             \
    push16(ret);               \
    g_ret_s = cpu.S;           \
    cpu.PB = (bank);           \
    fn();                      \
    g_ret_s = sv_;             \
  } while (0)
#define JSR_IND(ret, addr)     \
  do {                         \
    uint16_t sv_ = g_ret_s;    \
    uint32_t t_ = (addr);      \
    push16(ret);               \
    g_ret_s = cpu.S;           \
    eb_call(t_);               \
    g_ret_s = sv_;             \
  } while (0)

/* SETJMP: the host setjmp lives in the caller's frame so a later LONGJMP can unwind to it. */
#define EB_SETJMP_CALL(bank, ret, fn)                       \
  do {                                                      \
    uint16_t buf_ = cpu.A;                                  \
    JSL_(bank, ret, fn);                                    \
    {                                                       \
      volatile uint16_t keep_ = g_ret_s;                    \
      if (__builtin_setjmp(eb_host_jmpbuf(buf_))) {         \
        g_ret_s = keep_;                                    \
        eb_setjmp_landing(buf_);                            \
      }                                                     \
    }                                                       \
  } while (0)

#define RET_RTS()                         \
  do {                                    \
    if (EB_LIKELY(cpu.S == g_ret_s)) {    \
      cpu.S += 2;                         \
      return;                             \
    }                                     \
    eb_rts_trick();                       \
    return;                               \
  } while (0)
#define RET_RTL()                         \
  do {                                    \
    if (EB_LIKELY(cpu.S == g_ret_s)) {    \
      cpu.S += 3;                         \
      cpu.PB = rd8(cpu.S);                \
      return;                             \
    }                                     \
    eb_rtl_trick();                       \
    return;                               \
  } while (0)
#define RET_RTI()                         \
  do {                                    \
    if (EB_UNLIKELY(cpu.S != g_ret_s)) eb_rti_mismatch(); \
    set_p(pull8());                       \
    cpu.S += 2;                           \
    cpu.PB = pull8();                     \
    return;                               \
  } while (0)
