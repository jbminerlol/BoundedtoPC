/* Runtime support for the recompiled 65816 code: dispatch, computed jumps, interrupts,
 * frame boundaries, decimal arithmetic, block moves, SETJMP/LONGJMP. */
#include <stdlib.h>

#include "eb_gen.h"
#include "eb_syms.h"
#include "ppu.h"
#include "snes.h"

Cpu cpu;
uint32_t g_frame_cycles = SNES_NOMINAL_FRAME_CYCLES;
uint32_t g_cyc_limit = SNES_NOMINAL_FRAME_CYCLES;
uint64_t g_lag_frames;
uint16_t g_ret_s;
int g_in_interrupt;
uint64_t g_frame_count;
HostHooks g_host;

/* ---- dispatch ------------------------------------------------------------------------------- */
static const EbFuncEntry *find_entry(uint32_t addr) {
  uint32_t c = eb_canon(addr);
  int lo = 0, hi = EB_NUM_FUNCS - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t a = eb_func_table[mid].addr;
    if (a == c) return &eb_func_table[mid];
    if (a < c) lo = mid + 1;
    else hi = mid - 1;
  }
  return NULL;
}

uint32_t eb_func_addr(const char *name) {
  for (int i = 0; i < EB_NUM_FUNCS; i++)
    if (!strcmp(eb_func_table[i].name, name)) return eb_func_table[i].addr;
  return 0;
}

const char *eb_func_name(uint32_t addr) {
  /* the function whose entry is the closest one at or below addr */
  uint32_t c = eb_canon(addr);
  int lo = 0, hi = EB_NUM_FUNCS - 1, best = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (eb_func_table[mid].addr <= c) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return best >= 0 ? eb_func_table[best].name : "?";
}

void eb_call(uint32_t addr) {
  const EbFuncEntry *e = find_entry(addr);
  if (!e) {
    char msg[128];
    snprintf(msg, sizeof msg, "jump to unknown code at $%06X (A=%04X X=%04X Y=%04X S=%04X)", addr,
             cpu.A, cpu.X, cpu.Y, cpu.S);
    eb_fatal(msg);
  }
  cpu.PB = (uint8_t)(addr >> 16);
  e->fn();
}

void eb_jump(uint32_t addr) { eb_call(addr); }

uint16_t eb_rts_trick(void) {
  uint16_t t = pull16();
  uint16_t cont = rd16_b0((uint16_t)(cpu.S + 1));
  uint16_t sv = g_ret_s;
  g_ret_s = cpu.S;
  eb_call(PBASE | (uint16_t)(t + 1));
  g_ret_s = sv;
  return cont;
}

uint16_t eb_rtl_trick(void) {
  uint8_t lo = pull8(), hi = pull8(), bank = pull8();
  uint16_t cont = rd16_b0((uint16_t)(cpu.S + 1));
  uint16_t sv = g_ret_s;
  g_ret_s = cpu.S;
  eb_call(((uint32_t)bank << 16) | (uint16_t)((lo | (hi << 8)) + 1));
  g_ret_s = sv;
  return cont;
}

void eb_rti_mismatch(void) {
  eb_log("warning: RTI with S=%04X but interrupt frame at %04X", cpu.S, g_ret_s);
}

void eb_fatal(const char *msg) {
  eb_log("FATAL: %s (frame %llu)", msg, (unsigned long long)g_frame_count);
  extern void host_fatal(const char *msg);
  host_fatal(msg);
  exit(1);
}

/* ---- interrupts / frames --------------------------------------------------------------------- */
void snes_interrupt(uint32_t vector_addr) {
  uint16_t target = (uint16_t)(g_rom[(vector_addr & 0xFFFF)] | (g_rom[(vector_addr & 0xFFFF) + 1] << 8));
  uint16_t sv = g_ret_s;
  uint32_t svlimit = g_cyc_limit;
  push8(cpu.PB);
  push16(0xFFFF);
  push8(get_p());
  g_ret_s = cpu.S;
  cpu.I = 1;
  cpu.DF = 0;
  cpu.PB = 0;
  g_in_interrupt = 1;
  g_cyc_limit = 0xFFFFFFFFu; /* no nested frame boundaries inside the handler */
  eb_call(0xC00000u | target);
  g_in_interrupt = 0;
  g_cyc_limit = svlimit;
  g_ret_s = sv;
}

static int in_frame_wait(uint32_t addr) {
  static uint32_t lo, hi;
  if (!lo) {
    lo = eb_func_addr("WAIT_UNTIL_NEXT_FRAME");
    hi = lo + 0x40; /* the routine is 0x30-odd bytes */
  }
  uint32_t c = eb_canon(addr);
  return c >= lo && c < hi;
}

/* No lag frames (g_no_lag, default on): when the frame's CPU time runs out before the game has
 * finished the frame (it is not yet waiting for the next frame), it gets more time instead of the
 * NMI cutting in. On a lag frame the NMI still runs the game's VRAM upload queue but keeps the old
 * sprite list, so freshly uploaded tiles showed through the old sprite layout for a frame (sprites
 * glitching whenever something new happened on screen), and the game slowed down. The SPC is
 * clocked per frame, so music timing does not change. Not during forced blank (loading). */
int g_no_lag = 1;
static uint32_t s_extra_cycles;

void eb_poll(void) {
  if (g_in_interrupt) return;
  {
    extern int game_on_map(void);
    int waiting = in_frame_wait(g_poll_addr);
    if (!waiting && g_no_lag && !(g_ppu.inidisp & 0x80) && s_extra_cycles < SNES_FRAME_CYCLES * 4) {
      uint32_t chunk = SNES_FRAME_CYCLES / 8;
      s_extra_cycles += chunk;
      cpu.cyc = cpu.cyc > chunk ? cpu.cyc - chunk : 0;
      return;
    }
    s_extra_cycles = 0;
    if (!waiting && game_on_map()) g_lag_frames++;
  }
  /* end of the visible frame: let the host render/present it, then start vblank */
  if (g_host.frame) g_host.frame();
  apu_end_frame();
  if (cpu.cyc >= SNES_FRAME_CYCLES) cpu.cyc -= SNES_FRAME_CYCLES;
  else cpu.cyc = 0;
  if (cpu.cyc > SNES_FRAME_CYCLES / 2) cpu.cyc = SNES_FRAME_CYCLES / 2;
  g_frame_count++;
  if (g_regs.nmitimen & 1) {
    g_regs.joy[0] = g_pad_state[0];
    g_regs.joy[1] = g_pad_state[1];
  }
  g_regs.rdnmi_flag = 1;
  if (g_regs.nmitimen & 0x80) {
    /* diagnostics: an NMI with no new sprite list (NEXT_FRAME_DISPLAY_ID 0) while VRAM uploads
     * are queued shows new tiles through the old sprite layout = a one-frame sprite glitch */
    if (g_wram[SYM_NEXT_FRAME_DISPLAY_ID] == 0 && g_wram[SYM_DMA_QUEUE_INDEX] != g_wram[SYM_LAST_COMPLETED_DMA_INDEX]) {
      g_glitch_nmis++;
      static int verbose = -1;
      if (verbose < 0) verbose = getenv("EB_LOG_LAG") != NULL;
      if (verbose) eb_log("lag NMI with queued VRAM uploads at frame %llu (loop $%06X %s)", (unsigned long long)g_frame_count,
                          g_poll_addr, eb_func_name(g_poll_addr));
    }
    snes_interrupt(0xFFEA);
  }
}

uint64_t g_glitch_nmis; /* NMIs that showed freshly queued tiles with an old sprite list */
uint32_t g_poll_addr;
uint32_t g_last_func;

/* ---- test harness: call a game routine out of band, detecting hangs ------------------------- */
static void *s_test_jb[5];
static int s_test_active;
static uint64_t s_test_deadline;
static uint64_t s_test_cycles;

void eb_poll_at(uint32_t addr) {
  g_poll_addr = addr;
  if (s_test_active) {
    s_test_cycles += cpu.cyc;
    cpu.cyc = 0;
    if (s_test_cycles > s_test_deadline) __builtin_longjmp(s_test_jb, 1);
    return;
  }
  eb_poll();
}

/* Runs the function at addr like JSR/JSL from a safe point and restores the CPU afterwards.
 * Returns 1 if it returned, 0 if it was still running after max_cycles (a hang). *a_out = A. */
int eb_test_call(uint32_t addr, int far, uint64_t max_cycles, uint16_t *a_out) {
  Cpu saved = cpu;
  uint16_t sv = g_ret_s;
  uint32_t svlim = g_cyc_limit;
  int ok = 0;
  s_test_active = 1;
  s_test_cycles = 0;
  s_test_deadline = max_cycles;
  g_cyc_limit = 1000; /* make every back edge come through eb_poll_at for the watchdog */
  if (__builtin_setjmp(s_test_jb) == 0) {
    if (far) push8(cpu.PB);
    push16(0xFFFF);
    g_ret_s = cpu.S;
    eb_call(addr);
    ok = 1;
  }
  s_test_active = 0;
  if (a_out) *a_out = cpu.A;
  cpu = saved;
  g_ret_s = sv;
  g_cyc_limit = svlim;
  return ok;
}

void eb_wai(void) {
  cpu.cyc = g_cyc_limit;
  eb_poll();
}

void snes_run(void) {
  memset(&cpu, 0, sizeof cpu);
  cpu.E = 1;
  cpu.S = 0x01FF;
  cpu.M = cpu.XF = 1;
  cpu.I = 1;
  g_ret_s = 0xFFFF;
  uint16_t reset = (uint16_t)(g_rom[0xFFFC] | (g_rom[0xFFFD] << 8));
  eb_log("reset vector $%04X (%s)", reset, eb_func_name(0xC00000u | reset));
  eb_call(0xC00000u | reset);
  eb_fatal("the game's reset routine returned");
}

/* ---- decimal mode ------------------------------------------------------------------------------ */
uint16_t eb_adc_dec(uint16_t a, uint16_t v, int wide) {
  int n = wide ? 4 : 2, c = cpu.C;
  uint32_t res = 0;
  for (int i = 0; i < n; i++) {
    int d = ((a >> (4 * i)) & 15) + ((v >> (4 * i)) & 15) + c;
    if (d > 9) d += 6;
    c = d > 15;
    res |= (uint32_t)(d & 15) << (4 * i);
  }
  uint16_t sign = wide ? 0x8000 : 0x80, mask = wide ? 0xFFFF : 0xFF;
  cpu.C = (uint8_t)c;
  cpu.V = ((~(a ^ v) & (a ^ res)) & sign) != 0;
  cpu.Z = (res & mask) == 0;
  cpu.N = (res & sign) != 0;
  return (uint16_t)res;
}

uint16_t eb_sbc_dec(uint16_t a, uint16_t v, int wide) {
  int n = wide ? 4 : 2, c = cpu.C;
  uint32_t res = 0;
  for (int i = 0; i < n; i++) {
    int d = ((a >> (4 * i)) & 15) - ((v >> (4 * i)) & 15) - (1 - c);
    if (d < 0) {
      d += 10;
      c = 0;
    } else {
      c = 1;
    }
    res |= (uint32_t)(d & 15) << (4 * i);
  }
  uint16_t sign = wide ? 0x8000 : 0x80, mask = wide ? 0xFFFF : 0xFF;
  cpu.C = (uint8_t)c;
  cpu.V = (((a ^ v) & (a ^ res)) & sign) != 0;
  cpu.Z = (res & mask) == 0;
  cpu.N = (res & sign) != 0;
  return (uint16_t)res;
}

/* ---- block moves -------------------------------------------------------------------------------- */
void op_mvn(uint8_t src, uint8_t dst) {
  cpu.DB = dst;
  do {
    wr8(((uint32_t)dst << 16) | cpu.Y, rd8(((uint32_t)src << 16) | cpu.X));
    if (cpu.XF) {
      cpu.X = (cpu.X + 1) & 0xFF;
      cpu.Y = (cpu.Y + 1) & 0xFF;
    } else {
      cpu.X++;
      cpu.Y++;
    }
    cpu.A--;
    cpu.cyc += 7;
  } while (cpu.A != 0xFFFF);
}

void op_mvp(uint8_t src, uint8_t dst) {
  cpu.DB = dst;
  do {
    wr8(((uint32_t)dst << 16) | cpu.Y, rd8(((uint32_t)src << 16) | cpu.X));
    if (cpu.XF) {
      cpu.X = (cpu.X - 1) & 0xFF;
      cpu.Y = (cpu.Y - 1) & 0xFF;
    } else {
      cpu.X--;
      cpu.Y--;
    }
    cpu.A--;
    cpu.cyc += 7;
  } while (cpu.A != 0xFFFF);
}

/* ---- SETJMP / LONGJMP ------------------------------------------------------------------------- */
static struct {
  uint16_t buf;
  int used;
  void *jb[5];
} s_jmp[8];

void **eb_host_jmpbuf(uint16_t buf) {
  for (int i = 0; i < 8; i++)
    if (s_jmp[i].used && s_jmp[i].buf == buf) return s_jmp[i].jb;
  for (int i = 0; i < 8; i++) {
    if (!s_jmp[i].used) {
      s_jmp[i].used = 1;
      s_jmp[i].buf = buf;
      return s_jmp[i].jb;
    }
  }
  eb_fatal("too many SETJMP buffers");
  return NULL;
}

void eb_setjmp_landing(uint16_t buf) {
  if (g_log_level >= 2) eb_log("longjmp landed (buffer $%04X)", buf);
}

/* Native LONGJMP (system/longjmp.asm): restores S, D, P and DB saved by SETJMP, rewrites the
 * return address and "returns" from SETJMP's call with A = X - and unwinds the C stack. */
void F_LONGJMP(void) {
  op_rep(0x30);
  uint16_t y = cpu.A, val = cpu.X;
  int slot = -1;
  for (int i = 0; i < 8; i++)
    if (s_jmp[i].used && s_jmp[i].buf == y) slot = i;
  if (slot < 0) eb_fatal("LONGJMP to a buffer SETJMP never filled");
  cpu.DB = 0;
  cpu.S = rd16(y + 7u);
  cpu.D = rd16(y + 5u);
  uint16_t pw = rd16(y + 3u);
  push16(pw);
  (void)pull8();
  set_p(pull8());
  wr16_b0((uint16_t)(cpu.S + 1), rd16((uint32_t)y));
  wr16_b0((uint16_t)(cpu.S + 3), rd16(y + 2u));
  cpu.DB = pull8();
  cpu.A = val;
  NZ16(cpu.A);
  cpu.S += 3;
  cpu.PB = rd8(cpu.S);
  __builtin_longjmp(s_jmp[slot].jb, 1);
}
