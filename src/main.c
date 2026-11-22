/* EarthBound for Windows: SDL3 host (window/OpenGL presentation, audio, input, saves,
 * configuration, test hooks). The game itself is the recompiled 65816 code in build/gen. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eb_syms.h"
#include "enhance.h"
#include "game.h"
#include "menu.h"
#include "gl.h"
#include "host.h"
#include "png.h"
#include "ppu.h"
#include "recorder.h"
#include "render3d.h"
#include "snes.h"

/* defaults: pixel perfect (square pixels, whole-number scale, nearest), 16:9 widescreen */
EbConfig g_cfg = {.widescreen = 1, .square_pixels = 1, .filter = 0, .integer_scale = 1, .voxel3d = 0,
                  .battle3d = 0, .inventory_plus = 1, .sprite_limit = 0, .volume = 1.0f, .scale = 3,
                  .fill_letterbox = 1, .fullscreen = 0, .no_lag = 1};
#define EB_CFG_VERSION 3 /* bump to reset display settings saved by older builds */
char g_base_dir[1024];

static SDL_Window *s_win;
static SDL_GLContext s_glctx;
static SDL_AudioStream *s_audio;
static SDL_Gamepad *s_pad;
static uint32_t s_fb[PPU_MAX_W * PPU_H];
static GLuint s_tex, s_prog, s_vao;
static int s_tex_w;
static char s_save_path[1200], s_ini_path[1200];
static char s_rom_path[1024];   /* the ROM chosen last time (earthbound.ini rom=) */
static char s_loaded_rom[1024]; /* the ROM loaded now */
static uint64_t s_next_ns;
static int s_fast, s_test_mode, s_mute, s_fullscreen;
static long s_test_frames = -1, s_hash_every;
static uint64_t s_sram_hash;
static int s_fast_forward;
static int s_menu; /* the settings menu is open (Esc / Guide / Select+Start) */
static int s_swap_interval = -1, s_vsync_k; /* k > 0: vsync interval k paces the game */
static int s_bug_report;                     /* Ctrl+F12: write a bug report this frame */
static int s_shot_request;                   /* F12: save a screenshot of the window */
static int s_state_report;                   /* F12 key: also save the bug report (no BMP) */
static int s_rec_toggle;                     /* F9: start/stop recording */
static GLuint s_pfbo, s_ptex, s_rfbo, s_rtex; /* offscreen picture (captures read it) + rec scaler */
static int s_pw, s_ph, s_rw, s_rh;
static SDL_AtomicInt s_shots_writing;
static char s_title_note[96];
static long s_title_note_until;

/* Decide how frames are paced from the refresh rate the window is shown at. */
static void classify_display(void) {
  SDL_DisplayID disp = SDL_GetDisplayForWindow(s_win);
  const SDL_DisplayMode *dm = NULL;
  if (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN) dm = SDL_GetWindowFullscreenMode(s_win);
  if (!dm) dm = SDL_GetCurrentDisplayMode(disp);
  float hz = dm ? dm->refresh_rate : 0.0f;
  float k = hz / 60.0f;
  int mult = (int)(k + 0.5f);
  s_vsync_k = (hz > 0 && mult >= 1 && mult <= 4 && SDL_fabsf(k - (float)mult) < 0.02f * (float)mult) ? mult : 0;
  s_swap_interval = -1; /* re-apply on the next frame */
  if (s_vsync_k) {
    eb_log("display %.2f Hz: vsync, every frame shown for %d refresh(es)", hz, s_vsync_k);
  } else {
    const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(disp);
    int n = 0, has120 = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(disp, &n);
    char rates[256] = "";
    for (int i = 0; modes && desk && i < n; i++) {
      if (modes[i]->w != desk->w || modes[i]->h != desk->h) continue;
      float r = modes[i]->refresh_rate;
      if (SDL_fabsf(r - 120.0f) < 1.5f || SDL_fabsf(r - 60.0f) < 0.8f) has120 = 1;
      char one[16];
      SDL_snprintf(one, sizeof one, " %.0f", r);
      if (!SDL_strstr(rates, one)) SDL_strlcat(rates, one, sizeof rates);
    }
    SDL_free(modes);
    eb_log("display %.2f Hz (modes at %dx%d:%s): 60 fps can't be shown evenly; timer pacing, vsync off "
           "(smooth with G-Sync/FreeSync%s)", hz, desk ? desk->w : 0, desk ? desk->h : 0, rates,
           has120 ? ", or press F11: fullscreen switches to an even refresh rate" : "");
  }
}

/* Fullscreen: on a 144/165 Hz desktop, switch the monitor to 120 (or 60) Hz at the same
 * resolution while fullscreen, so every frame lasts the same number of refreshes. */
static void set_fullscreen(int on) {
  if (on) {
    SDL_DisplayID disp = SDL_GetDisplayForWindow(s_win);
    const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(disp);
    const SDL_DisplayMode *pick = NULL;
    float dhz = desk ? desk->refresh_rate : 0.0f;
    float dk = dhz / 60.0f;
    int even = dhz > 0 && SDL_fabsf(dk - (float)(int)(dk + 0.5f)) < 0.02f * (float)(int)(dk + 0.5f);
    if (desk && !even) {
      int n = 0;
      SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(disp, &n);
      float best = 0;
      for (int i = 0; modes && i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        if (m->w != desk->w || m->h != desk->h) continue;
        float r = m->refresh_rate;
        int ok120 = SDL_fabsf(r - 120.0f) < 1.5f, ok60 = SDL_fabsf(r - 60.0f) < 0.8f;
        float score = ok120 ? 2.0f : ok60 ? 1.0f : 0.0f;
        if (score > best) {
          best = score;
          pick = m;
        }
      }
      if (pick) eb_log("fullscreen at %dx%d %.2f Hz for even frame pacing", pick->w, pick->h, pick->refresh_rate);
      SDL_SetWindowFullscreenMode(s_win, pick);
      SDL_free(modes);
    } else {
      SDL_SetWindowFullscreenMode(s_win, NULL);
    }
    SDL_SetWindowFullscreen(s_win, true);
  } else {
    SDL_SetWindowFullscreen(s_win, false);
    SDL_SetWindowFullscreenMode(s_win, NULL);
  }
  SDL_SyncWindow(s_win);
  classify_display();
}
static long s_paced_frames, s_ff_frames, s_pace_resets;
static uint64_t s_sleep_asked;

/* ---- scripted input (EB_INPUT="frame:buttons;frame:-;...") ---------------------------------- */
typedef struct InputEvt {
  long frame;
  uint16_t buttons;
} InputEvt;
static InputEvt s_script[512];
static int s_script_n, s_script_i;
static uint16_t s_script_buttons;

static uint16_t button_bit(const char *name) {
  static const struct {
    const char *n;
    uint16_t b;
  } k[] = {{"b", 0x8000}, {"y", 0x4000}, {"select", 0x2000}, {"start", 0x1000}, {"up", 0x0800},
           {"down", 0x0400}, {"left", 0x0200}, {"right", 0x0100}, {"a", 0x0080}, {"x", 0x0040},
           {"l", 0x0020}, {"r", 0x0010}};
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!SDL_strcasecmp(name, k[i].n)) return k[i].b;
  return 0;
}

static void parse_script(const char *s) {
  char buf[8192];
  SDL_strlcpy(buf, s, sizeof buf);
  char *save = NULL;
  for (char *tok = SDL_strtok_r(buf, ";", &save); tok && s_script_n < 512; tok = SDL_strtok_r(NULL, ";", &save)) {
    char *colon = SDL_strchr(tok, ':');
    if (!colon) continue;
    *colon = 0;
    InputEvt e = {SDL_atoi(tok), 0};
    char *save2 = NULL;
    for (char *b = SDL_strtok_r(colon + 1, "+,", &save2); b; b = SDL_strtok_r(NULL, "+,", &save2))
      e.buttons |= button_bit(b);
    s_script[s_script_n++] = e;
  }
}

/* ---- config file ------------------------------------------------------------------------------ */
static void load_ini(void) {
  FILE *f = fopen(s_ini_path, "r");
  if (!f) return;
  char line[1200];
  EbConfig defaults = g_cfg;
  int version = 0;
  while (fgets(line, sizeof line, f)) {
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '#' || line[0] == ';') continue;
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (*k == ' ') k++;
    for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
    int iv = atoi(v);
    if (!strcmp(k, "rom")) {
      while (*v == ' ') v++;
      SDL_strlcpy(s_rom_path, v, sizeof s_rom_path);
      for (char *e = s_rom_path + strlen(s_rom_path); e > s_rom_path && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--)
        e[-1] = 0;
      continue;
    }
    if (!strcmp(k, "cfg_version")) version = iv;
    else if (!strcmp(k, "integer_scale")) g_cfg.integer_scale = iv;
    else if (!strcmp(k, "widescreen")) g_cfg.widescreen = iv;
    else if (!strcmp(k, "square_pixels")) g_cfg.square_pixels = iv;
    else if (!strcmp(k, "filter")) g_cfg.filter = iv;
    else if (!strcmp(k, "voxel3d")) g_cfg.voxel3d = iv;
    else if (!strcmp(k, "battle3d")) g_cfg.battle3d = iv;
    else if (!strcmp(k, "inventory_plus")) g_cfg.inventory_plus = iv;
    else if (!strcmp(k, "sprite_limit")) g_cfg.sprite_limit = iv;
    else if (!strcmp(k, "volume")) g_cfg.volume = (float)atof(v);
    else if (!strcmp(k, "scale")) g_cfg.scale = iv;
    else if (!strcmp(k, "cpu_overclock")) g_cfg.cpu_overclock = iv;
    else if (!strcmp(k, "fill_letterbox")) g_cfg.fill_letterbox = iv;
    else if (!strcmp(k, "fullscreen")) g_cfg.fullscreen = iv;
    else if (!strcmp(k, "no_lag")) g_cfg.no_lag = iv;
  }
  fclose(f);
  if (version < EB_CFG_VERSION) {
    /* settings from an older build: take the new display defaults */
    g_cfg.square_pixels = defaults.square_pixels;
    g_cfg.filter = defaults.filter;
    g_cfg.integer_scale = defaults.integer_scale;
    g_cfg.fullscreen = defaults.fullscreen; /* windowed by default again (v3) */
  }
}

static void save_ini(void) {
  FILE *f = fopen(s_ini_path, "w");
  if (!f) return;
  fprintf(f,
          "# EarthBound for Windows settings (also changeable in game: F1-F7)\n"
          "cfg_version=%d\nwidescreen=%d\nsquare_pixels=%d\ninteger_scale=%d\nfilter=%d\n"
          "voxel3d=%d\nbattle3d=%d\ninventory_plus=%d\nsprite_limit=%d\nvolume=%.2f\nscale=%d\n"
          "# extra CPU time per frame in percent (0 = real SNES; more = no slowdown)\n"
          "cpu_overclock=%d\n"
          "# battle backgrounds fill the screen instead of the original letterbox band\n"
          "fill_letterbox=%d\n"
          "# start fullscreen (F11 toggles; fullscreen runs 144 Hz monitors at 60 Hz = smooth)\n"
          "fullscreen=%d\n"
          "# busy frames get extra CPU time instead of slowing down (1 = no lag frames and no sprite\n"
          "# glitches when something new appears; 0 = slowdown like the real SNES)\n"
          "no_lag=%d\n"
          "# the ROM picked at start (Esc in game opens the settings menu)\n"
          "rom=%s\n",
          EB_CFG_VERSION, g_cfg.widescreen, g_cfg.square_pixels, g_cfg.integer_scale, g_cfg.filter,
          g_cfg.voxel3d, g_cfg.battle3d, g_cfg.inventory_plus, g_cfg.sprite_limit, g_cfg.volume,
          g_cfg.scale, g_cfg.cpu_overclock, g_cfg.fill_letterbox, g_cfg.fullscreen, g_cfg.no_lag, s_rom_path);
  fclose(f);
}

static int ws_ext(void) {
  /* 16:9 at the chosen pixel aspect, rounded to 8 px (one tile) */
  double par = g_cfg.square_pixels ? 1.0 : 8.0 / 7.0;
  double w = 224.0 * 16.0 / 9.0 / par;
  int ext = (int)((w - 256.0) / 2.0 + 0.5);
  ext = (ext + 7) & ~7;
  if (ext > PPU_MAX_EXT) ext = PPU_MAX_EXT;
  return ext;
}

/* ---- saves ------------------------------------------------------------------------------------------ */
static uint64_t fnv(const void *p, size_t n) {
  const uint8_t *b = p;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

static void load_sram(void) {
  FILE *f = fopen(s_save_path, "rb");
  if (f) {
    size_t n = fread(g_sram, 1, sizeof g_sram, f);
    fclose(f);
    eb_log("loaded save %s (%u bytes)", s_save_path, (unsigned)n);
  }
  s_sram_hash = fnv(g_sram, sizeof g_sram);
}

static void flush_sram(int force) {
  uint64_t h = fnv(g_sram, sizeof g_sram);
  if (h == s_sram_hash && !force) return;
  char tmp[1300];
  SDL_snprintf(tmp, sizeof tmp, "%s.tmp", s_save_path);
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    eb_log("cannot write %s", tmp);
    return;
  }
  fwrite(g_sram, 1, sizeof g_sram, f);
  fclose(f);
  remove(s_save_path);
  rename(tmp, s_save_path);
  s_sram_hash = h;
  if (g_log_level >= 1) eb_log("saved %s", s_save_path);
}

/* ---- presentation ---------------------------------------------------------------------------------- */
static const char *k_vs =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float(gl_VertexID & 1) * 2.0 - 1.0, float(gl_VertexID >> 1) * 2.0 - 1.0);\n"
    "  uv = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";
static const char *k_fs =
    "#version 330 core\n"
    "uniform sampler2D tex; uniform vec2 texsize; uniform vec2 fullsize; uniform vec2 scale;\n"
    "uniform int mode;\n"
    "in vec2 uv; out vec4 color;\n"
    "void main() {\n"
    "  vec2 t = uv * texsize;\n"
    "  if (mode == 1) {\n"
    "    vec2 f = fract(t); vec2 rr = 0.5 - 0.5 / max(scale, vec2(1.0));\n"
    "    vec2 cd = f - 0.5; f = (cd - clamp(cd, -rr, rr)) * scale + 0.5;\n"
    "    t = floor(t) + f;\n"
    "  } else if (mode == 0) { t = floor(t) + 0.5; }\n"
    "  color = vec4(texture(tex, t / fullsize).rgb, 1.0);\n"
    "}\n";

/* the picture is drawn into an offscreen buffer of the window's size, then copied to the window:
 * screenshots, recordings and test captures read it back (works even for hidden test windows) */
static GLuint make_color_target(GLuint *fbo, GLuint *tex, int w, int h) {
  if (!*fbo) gl.GenFramebuffers(1, fbo);
  if (*tex) gl.DeleteTextures(1, tex);
  gl.GenTextures(1, tex);
  gl.BindTexture(GL_TEXTURE_2D, *tex);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.BindFramebuffer(GL_FRAMEBUFFER, *fbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
  if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) eb_log("offscreen picture: incomplete");
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  return *fbo;
}

static void ensure_present_fbo(int w, int h) {
  if (s_pfbo && w == s_pw && h == s_ph) return;
  make_color_target(&s_pfbo, &s_ptex, w, h);
  s_pw = w;
  s_ph = h;
}

static void present_2d(int tex_w) {
  int ww, wh;
  SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
  gl.BindFramebuffer(GL_FRAMEBUFFER, s_pfbo);
  gl.Viewport(0, 0, ww, wh);
  gl.ClearColor(0, 0, 0, 1);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  double par = g_cfg.square_pixels ? 1.0 : 8.0 / 7.0;
  double aw = tex_w * par, ah = PPU_H;
  double sc = SDL_min(ww / aw, wh / ah);
  if (g_cfg.integer_scale && g_cfg.square_pixels && sc >= 1.0) sc = SDL_floor(sc); /* pixel perfect */
  int vw = (int)(aw * sc + 0.5), vh = (int)(ah * sc + 0.5);
  int vx = (ww - vw) / 2, vy = (wh - vh) / 2;
  gl.Viewport(vx, vy, vw, vh);
  gl.UseProgram(s_prog);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, s_tex);
  gl.Uniform1i(gl.GetUniformLocation(s_prog, "tex"), 0);
  gl.Uniform2f(gl.GetUniformLocation(s_prog, "texsize"), (float)tex_w, (float)PPU_H);
  gl.Uniform2f(gl.GetUniformLocation(s_prog, "fullsize"), (float)PPU_MAX_W, (float)PPU_H);
  gl.Uniform2f(gl.GetUniformLocation(s_prog, "scale"), (float)(vw / (double)tex_w), (float)(vh / (double)PPU_H));
  gl.Uniform1i(gl.GetUniformLocation(s_prog, "mode"), g_cfg.filter);
  gl.BindVertexArray(s_vao);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void upload_frame(int tex_w) {
  gl.BindTexture(GL_TEXTURE_2D, s_tex);
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, PPU_MAX_W);
  /* place the used columns at the left; the shader maps uv over tex_w columns */
  gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, PPU_H, GL_BGRA, GL_UNSIGNED_BYTE, s_fb);
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void setup_gl(void) {
  if (!gl_load()) host_fatal("OpenGL 3.3 is required (a needed function is missing)");
  s_prog = gl_program(k_vs, k_fs, "present");
  gl.GenVertexArrays(1, &s_vao);
  gl.GenTextures(1, &s_tex);
  gl.BindTexture(GL_TEXTURE_2D, s_tex);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PPU_MAX_W, PPU_H, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* ---- screenshots (tests only) ------------------------------------------------------------------------ */
static void write_bmp(const char *path, int w) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  int rowbytes = w * 3, pad = (4 - rowbytes % 4) % 4;
  uint32_t size = 54 + (uint32_t)(rowbytes + pad) * PPU_H;
  uint8_t h[54] = {'B', 'M'};
  memcpy(h + 2, &size, 4);
  h[10] = 54;
  h[14] = 40;
  memcpy(h + 18, &w, 4);
  int hh = PPU_H;
  memcpy(h + 22, &hh, 4);
  h[26] = 1;
  h[28] = 24;
  fwrite(h, 1, 54, f);
  for (int y = PPU_H - 1; y >= 0; y--) {
    for (int x = 0; x < w; x++) {
      uint32_t c = s_fb[y * PPU_MAX_W + x];
      uint8_t px[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)};
      fwrite(px, 1, 3, f);
    }
    for (int p = 0; p < pad; p++) fputc(0, f);
  }
  fclose(f);
}

uint64_t host_hash_frame(void) { return fnv(s_fb, sizeof s_fb); }

/* ---- input ---------------------------------------------------------------------------------------- */
static uint16_t read_input(void) {
  const bool *k = SDL_GetKeyboardState(NULL);
  uint16_t b = 0;
  if (k[SDL_SCANCODE_UP]) b |= 0x0800;
  if (k[SDL_SCANCODE_DOWN]) b |= 0x0400;
  if (k[SDL_SCANCODE_LEFT]) b |= 0x0200;
  if (k[SDL_SCANCODE_RIGHT]) b |= 0x0100;
  if (k[SDL_SCANCODE_X]) b |= 0x0080;          /* A */
  if (k[SDL_SCANCODE_Z]) b |= 0x8000;          /* B */
  if (k[SDL_SCANCODE_S]) b |= 0x0040;          /* X */
  if (k[SDL_SCANCODE_A]) b |= 0x4000;          /* Y */
  if (k[SDL_SCANCODE_Q]) b |= 0x0020;          /* L */
  if (k[SDL_SCANCODE_W]) b |= 0x0010;          /* R */
  if (k[SDL_SCANCODE_RETURN]) b |= 0x1000;     /* Start */
  if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= 0x2000; /* Select */
  if (s_pad) {
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= 0x0800;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= 0x0400;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= 0x0200;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= 0x0100;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_EAST)) b |= 0x0080;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_SOUTH)) b |= 0x8000;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_NORTH)) b |= 0x0040;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_WEST)) b |= 0x4000;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= 0x0020;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= 0x0010;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_START)) b |= 0x1000;
    if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_BACK)) b |= 0x2000;
    int lx = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX);
    int ly = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (lx < -16000) b |= 0x0200;
    if (lx > 16000) b |= 0x0100;
    if (ly < -16000) b |= 0x0800;
    if (ly > 16000) b |= 0x0400;
  }
  if ((b & 0x0C00) == 0x0C00) b &= (uint16_t)~0x0C00; /* no up+down */
  if ((b & 0x0300) == 0x0300) b &= (uint16_t)~0x0300;
  return b;
}

static void quit_now(void) {
  flush_sram(0);
  save_ini();
  if (rec_active()) rec_stop(); /* finish the MP4 so it plays */
  for (int i = 0; i < 200 && SDL_GetAtomicInt(&s_shots_writing) > 0; i++) SDL_Delay(10);
  SDL_Quit();
  exit(0);
}

static void toggle(int *v, const char *what) {
  *v = !*v;
  eb_log("%s %s", what, *v ? "on" : "off");
  save_ini();
}

static void pump_events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    render3d_event(&e);
    switch (e.type) {
      case SDL_EVENT_QUIT: quit_now(); break;
      case SDL_EVENT_GAMEPAD_ADDED:
        if (!s_pad) s_pad = SDL_OpenGamepad(e.gdevice.which);
        break;
      case SDL_EVENT_GAMEPAD_REMOVED:
        if (s_pad && SDL_GetGamepadID(s_pad) == e.gdevice.which) {
          SDL_CloseGamepad(s_pad);
          s_pad = NULL;
        }
        break;
      case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        if (e.key.scancode == SDL_SCANCODE_F1) toggle(&g_cfg.voxel3d, "3D voxel overworld");
        else if (e.key.scancode == SDL_SCANCODE_F2) toggle(&g_cfg.battle3d, "3D battles");
        else if (e.key.scancode == SDL_SCANCODE_F3) toggle(&g_cfg.widescreen, "widescreen");
        else if (e.key.scancode == SDL_SCANCODE_F4) {
          g_cfg.filter = (g_cfg.filter + 1) % 3;
          save_ini();
        } else if (e.key.scancode == SDL_SCANCODE_F5) toggle(&g_cfg.square_pixels, "square pixels");
        else if (e.key.scancode == SDL_SCANCODE_F7) toggle(&g_cfg.integer_scale, "whole-number scaling");
        else if (e.key.scancode == SDL_SCANCODE_F11 ||
                 (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
          s_fullscreen = !s_fullscreen;
          g_cfg.fullscreen = s_fullscreen;
          save_ini();
          set_fullscreen(s_fullscreen);
        } else if (e.key.scancode == SDL_SCANCODE_TAB) s_fast_forward = 1;
        else if (e.key.scancode == SDL_SCANCODE_F12 && (e.key.mod & SDL_KMOD_CTRL)) s_bug_report = 1;
        else if (e.key.scancode == SDL_SCANCODE_F12) s_shot_request = s_state_report = 1;
        else if (e.key.scancode == SDL_SCANCODE_F9) s_rec_toggle = 1;
        else if (e.key.scancode == SDL_SCANCODE_ESCAPE && !s_test_mode) s_menu = 1;
        break;
      case SDL_EVENT_KEY_UP:
        if (e.key.scancode == SDL_SCANCODE_TAB) s_fast_forward = 0;
        break;
    }
  }
}

/* ---- anti-piracy self test (EB_TEST_ANTIPIRACY=frame) ----------------------------------------- */
extern int eb_test_call(uint32_t addr, int far, uint64_t max_cycles, uint16_t *a_out);
extern uint32_t eb_func_addr(const char *name);

static void test_antipiracy(void) {
  static const char *names[3] = {"CHECK_HARDWARE", "SRAM_CHECK_ROUTINE_CHECKSUM",
                                 "FINAL_BATTLE_ANTIPIRACY_CHECK"};
  uint8_t before[sizeof g_sram];
  memcpy(before, g_sram, sizeof g_sram);
  int all_ok = 1;
  for (int i = 0; i < 3; i++) {
    uint32_t a = eb_func_addr(names[i]);
    uint16_t ra = 0xFFFF;
    int returned = a ? eb_test_call(a, 1, 5000000, &ra) : 0;
    uint16_t flag = (uint16_t)(g_wram[SYM_PIRACY_FLAG] | (g_wram[SYM_PIRACY_FLAG + 1] << 8));
    /* CHECK_HARDWARE legitimately writes its scratch byte ($30:7FF0 = SRAM $1FF0) */
    before[0x1FF0] = g_sram[0x1FF0];
    int sram_same = !memcmp(before, g_sram, sizeof g_sram);
    int ok = returned && sram_same && (i != 1 || (ra == 0 && flag == 0)) && (i != 2 || ra == 0);
    all_ok &= ok;
    eb_log("anti-piracy test: %-30s %s (returned=%d A=%04X PIRACY_FLAG=%04X save untouched=%d)",
           names[i], ok ? "PASS" : "FAIL", returned, ra, flag, sram_same);
  }
  eb_log("anti-piracy test: %s", all_ok ? "all checks pass like a genuine cart" : "SOME CHECKS FAILED");
}

/* ---- test auto-play (EB_AUTOPLAY=1): title -> new game -> names -> into the game ------------ */
static int s_autoplay;

/* tests: EB_WALK="start_frame@x,y;x,y;..." walks the leader through world waypoints from
 * start_frame on (d-pad toward the next point, next one once within 3 px) */
static uint16_t walk_buttons(long frame) {
  static int parsed, n, cur;
  static long start;
  static int wx[64], wy[64];
  if (!parsed) {
    parsed = 1;
    const char *w = SDL_getenv("EB_WALK");
    if (!w) return 0;
    start = SDL_atoi(w);
    const char *q = SDL_strchr(w, '@');
    while (q && n < 64) {
      q++;
      if (SDL_sscanf(q, "%d,%d", &wx[n], &wy[n]) == 2) n++;
      q = SDL_strchr(q, ';');
    }
  }
  if (!n || frame < start || cur >= n) return 0;
  int x = g_wram[SYM_GAME_STATE + 130] | g_wram[SYM_GAME_STATE + 131] << 8;
  int y = g_wram[SYM_GAME_STATE + 134] | g_wram[SYM_GAME_STATE + 135] << 8;
  int dx = wx[cur] - x, dy = wy[cur] - y;
  if (dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3) {
    eb_log("walk: reached waypoint %d (%d,%d) at frame %ld", cur, wx[cur], wy[cur], frame);
    cur++;
    return 0;
  }
  uint16_t b = 0;
  if (dx > 2) b |= 0x0100;
  if (dx < -2) b |= 0x0200;
  if (dy > 2) b |= 0x0400;
  if (dy < -2) b |= 0x0800;
  return b;
}

static uint16_t autoplay_buttons(long frame) {
  extern long g_name_prompt_frame;
  static long naming_start = -1, last_prompt = -1;
  if (!s_autoplay) return 0;
  if (g_name_prompt_frame != last_prompt && frame - g_name_prompt_frame > 90) {
    last_prompt = g_name_prompt_frame;
    naming_start = frame; /* keyboard is up: Up to the bottom row, A on Don't Care, Start = OK */
  }
  if (naming_start >= 0) {
    long d = frame - naming_start;
    if (d < 6) return 0x0800;
    if (d >= 30 && d < 36) return 0x0080;
    if (d >= 60 && d < 66) return 0x1000;
    if (d < 120) return 0;
    naming_start = -1;
  }
  if (frame < 1700) return (frame % 120) < 6 ? 0x1000 : 0;
  return (frame % 45) < 6 ? 0x0080 : 0;
}

/* share of non-black pixels in the widescreen margins (test logging) */
static int margin_fill_pct(int ext) {
  if (ext <= 0) return -1;
  long lit = 0, total = 0;
  for (int y = 0; y < PPU_H; y += 2) {
    for (int x = 0; x < ext; x++) {
      uint32_t l = s_fb[y * PPU_MAX_W + x], r = s_fb[y * PPU_MAX_W + 256 + ext + x];
      lit += ((l & 0xFFFFFF) != 0) + ((r & 0xFFFFFF) != 0);
      total += 2;
    }
  }
  return (int)(lit * 100 / total);
}

/* Ctrl+F12 (and F12, without the BMP since F12 saves a PNG): everything needed to diagnose a
 * sprite/background problem, in bugreports/ next to the exe */
static void title_note(const char *msg, long frames);
static void write_bug_report(int tex_w, int with_bmp) {
  char dir[1200], path[1300];
  SDL_snprintf(dir, sizeof dir, "%sbugreports", g_base_dir);
  SDL_CreateDirectory(dir);
  long frame = (long)g_frame_count;
  if (with_bmp) {
    SDL_snprintf(path, sizeof path, "%s/frame_%06ld.bmp", dir, frame);
    write_bmp(path, tex_w);
  }
  SDL_snprintf(path, sizeof path, "%s/frame_%06ld.txt", dir, frame);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "frame %ld scene %d ext %d widescreen %d voxel %d\n", frame, (int)game_scene(), g_cfg.ws_ext,
          g_cfg.widescreen, g_cfg.voxel3d);
  fprintf(f, "ppu: inidisp %02x bgmode %02x obsel %02x tm %02x ts %02x bgsc %02x %02x %02x %02x\n",
          g_ppu.inidisp, g_ppu.bgmode, g_ppu.obsel, g_ppu.tm, g_ppu.ts, g_ppu.bgsc[0], g_ppu.bgsc[1],
          g_ppu.bgsc[2], g_ppu.bgsc[3]);
  fprintf(f, "scroll regs: bg1 %d,%d bg2 %d,%d bg3 %d,%d\n", g_ppu.hofs[0], g_ppu.vofs[0], g_ppu.hofs[1],
          g_ppu.vofs[1], g_ppu.hofs[2], g_ppu.vofs[2]);
  fprintf(f, "ram: BG1_X_POS %d BG1_Y_POS %d NEXT_FRAME_DISPLAY_ID %d NEXT_FRAME_BUF_ID %d ws_ext %d ws_cols %d\n",
          g_wram[SYM_BG1_X_POS] | (g_wram[SYM_BG1_X_POS + 1] << 8), g_wram[SYM_BG1_Y_POS] | (g_wram[SYM_BG1_Y_POS + 1] << 8),
          g_wram[SYM_NEXT_FRAME_DISPLAY_ID], g_wram[SYM_NEXT_FRAME_BUF_ID], g_ws_ext, g_ws_cols);
  fprintf(f, "OAM (visible entries): idx x9 wide(valid,x) y tile attr large | group gx gy\n");
  for (int i = 0; i < 128; i++) {
    int y = g_oam[i * 4 + 1];
    if (y >= 224 && y < 240) continue;
    int hi = (g_oam[0x200 + (i >> 2)] >> ((i & 3) * 2)) & 3;
    int x9 = g_oam[i * 4] | ((hi & 1) << 8);
    fprintf(f, "%3d %3d (%d,%4d) %3d %02x %02x %d | %5u %4d %4d slot %d\n", i, x9, g_obj_wide_valid[i], g_obj_wide_x[i], y,
            g_oam[i * 4 + 2], g_oam[i * 4 + 3], hi >> 1, g_obj_frame.group[i], g_obj_frame.group_x[i],
            g_obj_frame.group_y[i], g_obj_frame.slot[i]);
  }
#define R16(a) (g_wram[(a)] | g_wram[(a) + 1] << 8)
  fprintf(f, "entities (slot: script sprite vram smap smapsize bytewidth rows dir shown surface screen abs):\n");
  for (int s = 0; s < 30; s++) {
    if (R16(SYM_ENTITY_SCRIPT_TABLE + 2 * s) == 0xFFFF) continue;
    fprintf(f, "%2d: %04x %3d %04x %04x %3d %3d %d %d %04x %04x %d,%d %d,%d\n", s, R16(SYM_ENTITY_SCRIPT_TABLE + 2 * s),
            R16(SYM_ENTITY_SPRITE_IDS + 2 * s), R16(SYM_ENTITY_VRAM_ADDRESS + 2 * s), R16(SYM_ENTITY_SPRITEMAP_POINTER_LOW + 2 * s),
            R16(SYM_ENTITY_SPRITEMAP_SIZES + 2 * s), R16(SYM_ENTITY_BYTE_WIDTHS + 2 * s), R16(SYM_ENTITY_TILE_HEIGHTS + 2 * s),
            R16(SYM_ENTITY_DIRECTIONS + 2 * s), R16(SYM_ENTITY_CURRENT_DISPLAYED_SPRITES + 2 * s),
            R16(SYM_ENTITY_SURFACE_FLAGS + 2 * s), (int16_t)R16(SYM_ENTITY_SCREEN_X_TABLE + 2 * s),
            (int16_t)R16(SYM_ENTITY_SCREEN_Y_TABLE + 2 * s), R16(SYM_ENTITY_ABS_X_TABLE + 2 * s), R16(SYM_ENTITY_ABS_Y_TABLE + 2 * s));
  }
  fprintf(f, "sprite VRAM cells (88):");
  for (int i = 0; i < 88; i++) fprintf(f, "%s%02x", (i % 8) ? " " : "\n  ", g_wram[SYM_SPRITE_VRAM_TABLE + i]);
  fprintf(f, "\nspritemap heap (OVERWORLD_SPRITEMAPS, 5-byte entries):");
  for (int i = 0; i < 0x380; i++) fprintf(f, "%s%02x", (i % 20) ? ((i % 5) ? "" : " ") : "\n  ", g_wram[SYM_OVERWORLD_SPRITEMAPS + i]);
  fprintf(f, "\nobsel %02x\n", g_ppu.obsel);
#undef R16
  fprintf(f, "recent DMA (frame src->bbad size vmadd nmi):\n");
  for (int k = 0; k < 256; k++) {
    DmaLogEntry *e = &g_dma_log[(g_dma_log_pos + k) & 255];
    if (!e->size) continue;
    fprintf(f, "%6u %06X->21%02X %5u %04X %d\n", e->frame, e->src, e->bbad, e->size, e->vmadd, e->in_nmi);
  }
  fclose(f);
  {
    /* sprite VRAM ($4000-$5FFF words) as a 16-tile-wide sheet with OBJ palette 0..7 by band */
    static uint32_t img[128 * 256];
    for (int t = 0; t < 512; t++) {
      int tx = (t & 15) * 8, ty = (t >> 4) * 8;
      int pal = 0; /* grey ramp: shows what is where, palette-independent */
      (void)pal;
      for (int py = 0; py < 8; py++) {
        int a = 0x4000 + t * 16 + py;
        uint16_t w0 = (uint16_t)(g_vram[(a * 2) & 0xFFFF] | g_vram[(a * 2 + 1) & 0xFFFF] << 8);
        uint16_t w1 = (uint16_t)(g_vram[((a + 8) * 2) & 0xFFFF] | g_vram[((a + 8) * 2 + 1) & 0xFFFF] << 8);
        for (int px = 0; px < 8; px++) {
          int sh = 7 - px;
          int c = ((w0 >> sh) & 1) | (((w0 >> (sh + 8)) & 1) << 1) | (((w1 >> sh) & 1) << 2) | (((w1 >> (sh + 8)) & 1) << 3);
          uint32_t v = c ? (uint32_t)(0x30 + c * 13) : 0x10;
          img[(ty + py) * 128 + tx + px] = 0xFF000000u | v << 16 | v << 8 | (c ? v : 0x40);
        }
      }
    }
    SDL_snprintf(path, sizeof path, "%s/frame_%06ld_objvram.png", dir, frame);
    png_write_bgra(path, (const uint8_t *)img, 128, 256, 128 * 4, 0);
  }
  eb_log("bug report written: %s", path);
  if (with_bmp) title_note("bug report saved", 120);
}

/* ---- screenshots (F12) and recording (F9) ------------------------------------------------------ */
static void title_note(const char *msg, long frames) {
  SDL_strlcpy(s_title_note, msg, sizeof s_title_note);
  s_title_note_until = (long)g_frame_count + frames;
}

/* for the other modules (3D camera reset etc.): a short note in the window title */
void host_note(const char *msg) { title_note(msg, 90); }

static void update_title(void) {
  static char last[160];
  char t[160];
  if (rec_active()) SDL_snprintf(t, sizeof t, "EarthBound   [REC - F9 to stop]");
  else if ((long)g_frame_count < s_title_note_until) SDL_snprintf(t, sizeof t, "EarthBound   [%s]", s_title_note);
  else SDL_strlcpy(t, "EarthBound", sizeof t);
  if (strcmp(t, last)) {
    SDL_SetWindowTitle(s_win, t);
    SDL_strlcpy(last, t, sizeof last);
  }
}

/* "<exe dir>/<sub>/EarthBound_<date>_<time><ext>", not overwriting an existing file */
static void capture_path(char *out, size_t n, const char *sub, const char *ext) {
  char dir[1200], stamp[64];
  SDL_snprintf(dir, sizeof dir, "%s%s", g_base_dir, sub);
  SDL_CreateDirectory(dir);
  SDL_Time now;
  SDL_DateTime dt;
  if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
    SDL_snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                 dt.second);
  else
    SDL_snprintf(stamp, sizeof stamp, "frame%06llu", (unsigned long long)g_frame_count);
  SDL_snprintf(out, n, "%s/EarthBound_%s%s", dir, stamp, ext);
  for (int k = 2; k < 100; k++) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(out, &info)) break;
    SDL_snprintf(out, n, "%s/EarthBound_%s_%d%s", dir, stamp, k, ext);
  }
}

typedef struct ShotJob {
  char path[1300];
  uint8_t *px;
  int w, h;
} ShotJob;

static int shot_thread(void *arg) {
  ShotJob *j = (ShotJob *)arg;
  int ok = png_write_bgra(j->path, j->px, j->w, j->h, j->w * 4, 1);
  eb_log(ok ? "screenshot saved: %s" : "screenshot could not be written: %s", j->path);
  SDL_free(j->px);
  SDL_free(j);
  SDL_AddAtomicInt(&s_shots_writing, -1);
  return 0;
}

/* reads the offscreen picture (bottom-up BGRA) */
static void read_picture(GLuint fbo, int w, int h, void *dst) {
  gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
  gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, dst);
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* a bottom-up RGBA picture (as read_picture returns it) as a 24-bit BMP (tests) */
static void write_bmp_rgba(const char *path, const uint8_t *px, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  int rowb = w * 3, pad = (4 - rowb % 4) % 4;
  uint32_t size = 54 + (uint32_t)(rowb + pad) * (uint32_t)h;
  uint8_t hd[54] = {'B', 'M'};
  memcpy(hd + 2, &size, 4);
  hd[10] = 54;
  hd[14] = 40;
  memcpy(hd + 18, &w, 4);
  memcpy(hd + 22, &h, 4);
  hd[26] = 1;
  hd[28] = 24;
  fwrite(hd, 1, 54, f);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
    for (int q = 0; q < pad; q++) fputc(0, f);
  }
  fclose(f);
}

static void take_screenshot(int w, int h) {
  ShotJob *j = (ShotJob *)SDL_malloc(sizeof *j);
  uint8_t *px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
  if (!j || !px) {
    SDL_free(j);
    SDL_free(px);
    return;
  }
  read_picture(s_pfbo, w, h, px);
  capture_path(j->path, sizeof j->path, "screenshots", ".png");
  j->px = px;
  j->w = w;
  j->h = h;
  SDL_AddAtomicInt(&s_shots_writing, 1);
  SDL_Thread *t = SDL_CreateThread(shot_thread, "screenshot", j);
  if (t) SDL_DetachThread(t);
  else shot_thread(j);
  title_note("screenshot saved", 120);
}

static void toggle_recording(int ww, int wh) {
  if (rec_active()) {
    rec_stop();
    title_note("recording saved in the recordings folder", 180);
    return;
  }
  /* the window's size, at most 1920x1080 */
  int rw = ww, rh = wh;
  if (rw > 1920 || rh > 1080) {
    double s = SDL_min(1920.0 / rw, 1080.0 / rh);
    rw = (int)(rw * s);
    rh = (int)(rh * s);
  }
  char path[1300];
  capture_path(path, sizeof path, "recordings", ".mp4");
  if (rec_start(path, rw, rh)) {
    eb_log("recording to %s (%dx%d)", path, rec_width(), rec_height());
  } else {
    eb_log("recording could not start");
    title_note("recording could not start", 180);
  }
}

static void record_frame(int ww, int wh) {
  uint8_t *dst = rec_begin_frame();
  if (!dst) return;
  int rw = rec_width(), rh = rec_height();
  GLuint src = s_pfbo;
  if (rw != ww || rh != wh) { /* window size changed since the start: scale into the recording size */
    if (rw != s_rw || rh != s_rh) {
      make_color_target(&s_rfbo, &s_rtex, rw, rh);
      s_rw = rw;
      s_rh = rh;
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s_pfbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rfbo);
    gl.BlitFramebuffer(0, 0, ww, wh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    src = s_rfbo;
  }
  read_picture(src, rw, rh, dst);
  rec_end_frame(dst);
}

/* frame timing statistics for test runs */
static uint64_t s_t_emu, s_t_render, s_t_present, s_t_sleep, s_t_last_end, s_t_max_frame;

/* ---- menus: settings (Esc / Guide / Select+Start) and the ROM picker -------------------------------- */
static uint32_t s_menu_cv[PPU_MAX_W * PPU_H];
static GLuint s_menu_tex, s_menu_prog, s_menu_vao;
static int s_menu_cur;
static uint16_t s_menu_mask; /* buttons that closed the menu: kept from the game until released */
static int s_guide_was, s_chord_was;
extern int g_no_lag;
void render3d_reset_camera(void);

/* where the game picture sits in the window (as present_2d draws it) */
static void picture_rect(int ww, int wh, int tex_w, int *vx, int *vy, int *vw, int *vh) {
  double par = g_cfg.square_pixels ? 1.0 : 8.0 / 7.0;
  double aw = tex_w * par, ah = PPU_H;
  double sc = SDL_min(ww / aw, wh / ah);
  if (g_cfg.integer_scale && g_cfg.square_pixels && sc >= 1.0) sc = SDL_floor(sc);
  *vw = (int)(aw * sc + 0.5);
  *vh = (int)(ah * sc + 0.5);
  *vx = (ww - *vw) / 2;
  *vy = (wh - *vh) / 2;
}

/* the menu canvas on top of the picture in s_pfbo */
static void draw_menu_canvas(int tex_w) {
  if (!s_menu_prog) {
    static const char *vs =
        "#version 330 core\n"
        "out vec2 v_uv; uniform vec2 u_uvmax;\n"
        "void main() { vec2 p = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);\n"
        "  v_uv = vec2((p.x * 0.5 + 0.5) * u_uvmax.x, (0.5 - p.y * 0.5) * u_uvmax.y);\n"
        "  gl_Position = vec4(p, 0.0, 1.0); }\n";
    static const char *fs =
        "#version 330 core\n"
        "in vec2 v_uv; out vec4 o; uniform sampler2D tex;\n"
        "void main() { vec4 c = texture(tex, v_uv); if (c.a < 0.5) discard; o = vec4(c.rgb, 1.0); }\n";
    s_menu_prog = gl_program(vs, fs, "menu");
    gl.GenTextures(1, &s_menu_tex);
    gl.BindTexture(GL_TEXTURE_2D, s_menu_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PPU_MAX_W, PPU_H, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.GenVertexArrays(1, &s_menu_vao);
  }
  int ww, wh, vx, vy, vw, vh;
  SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
  picture_rect(ww, wh, tex_w, &vx, &vy, &vw, &vh);
  gl.BindFramebuffer(GL_FRAMEBUFFER, s_pfbo);
  gl.Viewport(vx, vy, vw, vh);
  gl.Disable(GL_DEPTH_TEST);
  gl.Disable(GL_BLEND);
  gl.UseProgram(s_menu_prog);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, s_menu_tex);
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, PPU_H, GL_BGRA, GL_UNSIGNED_BYTE, s_menu_cv);
  gl.Uniform1i(gl.GetUniformLocation(s_menu_prog, "tex"), 0);
  gl.Uniform2f(gl.GetUniformLocation(s_menu_prog, "u_uvmax"), (float)tex_w / PPU_MAX_W, 1.0f);
  gl.BindVertexArray(s_menu_vao);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void show_pfbo(void) {
  int ww, wh;
  SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s_pfbo);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  gl.BlitFramebuffer(0, 0, ww, wh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  SDL_GL_SwapWindow(s_win);
}

static void handle_pad_device(const SDL_Event *e) {
  if (e->type == SDL_EVENT_GAMEPAD_ADDED && !s_pad) s_pad = SDL_OpenGamepad(e->gdevice.which);
  if (e->type == SDL_EVENT_GAMEPAD_REMOVED && s_pad && SDL_GetGamepadID(s_pad) == e->gdevice.which) {
    SDL_CloseGamepad(s_pad);
    s_pad = NULL;
  }
}

/* menu buttons with auto-repeat for the d-pad: returns the newly pressed SNES-style bits */
static uint16_t menu_press(uint16_t cur, uint16_t *prev, int *held) {
  uint16_t press = (uint16_t)(cur & ~*prev);
  uint16_t dirs = (uint16_t)(cur & 0x0F00);
  if (dirs && dirs == (*prev & 0x0F00)) {
    if (++*held > 18 && (*held - 18) % 5 == 0) press |= dirs;
  } else {
    *held = 0;
  }
  *prev = cur;
  return press;
}

enum { MI_3D, MI_BATTLE3D, MI_WIDE, MI_SQUARE, MI_INTEGER, MI_FILTER, MI_FULL, MI_NOLAG, MI_SPRLIM, MI_VOLUME,
       MI_CAMERA, MI_CLOSE, MI_N };

static void settings_item(int i, const char **label, const char **value) {
  static const char *onoff[2] = {"Off", "On"}, *filt[3] = {"Off", "Sharp", "Soft"};
  *value = "";
  switch (i) {
    case MI_3D: *label = "3D overworld"; *value = onoff[g_cfg.voxel3d != 0]; break;
    case MI_BATTLE3D: *label = "3D battles"; *value = onoff[g_cfg.battle3d != 0]; break;
    case MI_WIDE: *label = "Widescreen"; *value = onoff[g_cfg.widescreen != 0]; break;
    case MI_SQUARE: *label = "Square pixels"; *value = onoff[g_cfg.square_pixels != 0]; break;
    case MI_INTEGER: *label = "Pixel-perfect size"; *value = onoff[g_cfg.integer_scale != 0]; break;
    case MI_FILTER: *label = "Smoothing"; *value = filt[g_cfg.filter % 3]; break;
    case MI_FULL: *label = "Fullscreen"; *value = onoff[s_fullscreen != 0]; break;
    case MI_NOLAG: *label = "No slowdown"; *value = onoff[g_cfg.no_lag != 0]; break;
    case MI_SPRLIM: *label = "Sprite limit"; *value = onoff[g_cfg.sprite_limit != 0]; break;
    case MI_VOLUME: *label = "Volume"; break;
    case MI_CAMERA: *label = "Reset 3D camera"; break;
    default: *label = "Back to the game"; break;
  }
}

/* dir: -1 left, +1 right, 0 A/Start */
static void settings_change(int i, int dir) {
  switch (i) {
    case MI_3D: g_cfg.voxel3d = !g_cfg.voxel3d; break;
    case MI_BATTLE3D: g_cfg.battle3d = !g_cfg.battle3d; break;
    case MI_WIDE: g_cfg.widescreen = !g_cfg.widescreen; break;
    case MI_SQUARE: g_cfg.square_pixels = !g_cfg.square_pixels; break;
    case MI_INTEGER: g_cfg.integer_scale = !g_cfg.integer_scale; break;
    case MI_FILTER: g_cfg.filter = (g_cfg.filter + (dir < 0 ? 2 : 1)) % 3; break;
    case MI_FULL:
      s_fullscreen = !s_fullscreen;
      g_cfg.fullscreen = s_fullscreen;
      set_fullscreen(s_fullscreen);
      break;
    case MI_NOLAG: g_cfg.no_lag = !g_cfg.no_lag; g_no_lag = g_cfg.no_lag; break;
    case MI_SPRLIM: g_cfg.sprite_limit = !g_cfg.sprite_limit; break;
    case MI_VOLUME: {
      int v = (int)(g_cfg.volume * 10.0f + 0.5f) + (dir < 0 ? -1 : 1);
      if (dir == 0) v = v > 10 ? 0 : v; /* A steps up and wraps */
      v = v < 0 ? 0 : v > 10 ? 10 : v;
      g_cfg.volume = v / 10.0f;
      if (s_audio) SDL_SetAudioStreamGain(s_audio, s_mute ? 0.0f : g_cfg.volume);
      break;
    }
    case MI_CAMERA: render3d_reset_camera(); return;
    default: s_menu = 0; return;
  }
  save_ini();
}

static void settings_draw(int tex_w) {
  menu_begin(s_menu_cv, tex_w, PPU_H);
  const int W = 184, H = 216;
  int x = (tex_w - W) / 2, y = (PPU_H - H) / 2;
  menu_window(x, y, W, H, 1);
  uint32_t ink = menu_ink(1), dim = menu_dim(1);
  menu_text(x + 12, y + 6, "Settings", ink);
  for (int i = 0; i < MI_N; i++) {
    const char *label, *value;
    settings_item(i, &label, &value);
    int ry = y + 24 + i * 14;
    menu_text(x + 22, ry, label, i == MI_CAMERA && !g_cfg.voxel3d ? dim : ink);
    if (i == MI_VOLUME) {
      /* ten little boxes, filled up to the volume */
      int lvl = (int)(g_cfg.volume * 10.0f + 0.5f);
      for (int b = 0; b < 10; b++) {
        int bx = x + W - 14 - (10 - b) * 5, by = ry + 5;
        for (int yy = 0; yy < 6; yy++)
          for (int xx = 0; xx < 4; xx++) {
            int edge = yy == 0 || yy == 5 || xx == 0 || xx == 3;
            if (b < lvl || edge) s_menu_cv[(by + yy) * tex_w + bx + xx] = b < lvl ? ink : dim;
          }
      }
    } else if (value[0]) {
      menu_text(x + W - 12 - menu_text_width(value), ry, value, value[0] == 'O' && value[1] == 'f' ? dim : ink);
    }
    if (i == s_menu_cur) menu_cursor(x + 16, ry, ink);
  }
}

/* the game is paused while the settings menu is up; the picture keeps redrawing so changes show */
static void settings_menu_loop(void) {
  uint16_t prev = read_input();
  int held = 0;
  while (s_menu) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      render3d_event(&e);
      handle_pad_device(&e);
      if (e.type == SDL_EVENT_QUIT) quit_now();
      if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE) s_menu = 0;
    }
    uint16_t cur = read_input();
    if (s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_GUIDE)) cur |= 0x8000;
    uint16_t press = menu_press(cur, &prev, &held);
    if (press & 0x0800) s_menu_cur = (s_menu_cur + MI_N - 1) % MI_N;
    if (press & 0x0400) s_menu_cur = (s_menu_cur + 1) % MI_N;
    if (press & 0x0200 && s_menu_cur < MI_CAMERA) settings_change(s_menu_cur, -1);
    if (press & 0x0100 && s_menu_cur < MI_CAMERA) settings_change(s_menu_cur, 1);
    if (press & (0x0080 | 0x1000)) settings_change(s_menu_cur, 0);
    if (press & 0x8000) s_menu = 0;
    if (!s_menu) s_menu_mask = cur;
    int ext = g_cfg.widescreen ? ws_ext() : 0;
    g_cfg.ws_ext = ext;
    ppu_sprite_limit = g_cfg.sprite_limit;
    game_before_render(ext);
    ppu_render_frame(s_fb, PPU_MAX_W, ext);
    int tex_w = 256 + 2 * ext;
    int ww, wh;
    SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
    if (ww < 1 || wh < 1) {
      SDL_Delay(16);
      continue;
    }
    ensure_present_fbo(ww, wh);
    if (!render3d_frame(s_win, s_pfbo, ext)) {
      upload_frame(tex_w);
      s_tex_w = tex_w;
      present_2d(tex_w);
    }
    if (s_menu) {
      settings_draw(tex_w);
      draw_menu_canvas(tex_w);
    }
    show_pfbo();
    SDL_Delay(12);
  }
  host_note("");
}

/* ---- the ROM picker, before the game starts ----------------------------------------------------- */
static int try_rom(const char *path, char *err, size_t errlen);
static SDL_AtomicInt s_dlg_done;
static char s_dlg_path[1024];

static void SDLCALL rom_dialog_cb(void *ud, const char *const *files, int filter) {
  (void)ud;
  (void)filter;
  if (files && files[0]) SDL_strlcpy(s_dlg_path, files[0], sizeof s_dlg_path);
  else s_dlg_path[0] = 0;
  SDL_SetAtomicInt(&s_dlg_done, 1);
}

static void quit_before_start(void) {
  save_ini();
  SDL_Quit();
  exit(0);
}

/* the Windows file dialog; 1 = a file was picked (s_dlg_path) */
static int ask_rom_file(void) {
  static const SDL_DialogFileFilter filters[] = {{"SNES ROM (*.sfc, *.smc)", "sfc;smc"}, {"All files", "*"}};
  SDL_SetAtomicInt(&s_dlg_done, 0);
  s_dlg_path[0] = 0;
  SDL_ShowOpenFileDialog(rom_dialog_cb, NULL, s_win, filters, 2, s_loaded_rom[0] ? s_loaded_rom : NULL, false);
  while (!SDL_GetAtomicInt(&s_dlg_done)) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      handle_pad_device(&e);
      if (e.type == SDL_EVENT_QUIT) quit_before_start();
    }
    SDL_Delay(15);
  }
  return s_dlg_path[0] != 0;
}

/* word-wraps `s` into lines of at most `w` pixels of the main font; returns the y after it */
static int wrap_text(int x, int y, int w, const char *s, uint32_t c) {
  char line[160];
  int n = 0, last_space = -1;
  for (const char *p = s;; p++) {
    if (*p && n < (int)sizeof line - 1) {
      line[n] = *p;
      if (*p == ' ') last_space = n;
      line[++n] = 0;
      if (menu_text_width(line) <= w) continue;
    }
    if (!*p && n == 0) break;
    int cut = (*p && last_space > 0) ? last_space : n;
    char out[160];
    SDL_memcpy(out, line, (size_t)cut);
    out[cut] = 0;
    menu_text(x, y, out, c);
    y += 14;
    if (!*p && cut == n) break;
    int rest = n - cut - (cut < n && line[cut] == ' ' ? 1 : 0);
    SDL_memmove(line, line + n - rest, (size_t)rest);
    n = rest;
    line[n] = 0;
    last_space = -1;
    for (int i = 0; i < n; i++) if (line[i] == ' ') last_space = i;
    if (!*p && n == 0) break;
  }
  return y;
}

/* the ROMs in the folder of the ROM that loaded (og roms/ by default): .sfc and .smc files */
static char s_pick_dir[1024];
static char s_pick_name[64][128];
static int s_pick_n;

static void scan_rom_dir(void) {
  SDL_strlcpy(s_pick_dir, s_loaded_rom, sizeof s_pick_dir);
  char *cut = s_pick_dir;
  for (char *q = s_pick_dir; *q; q++)
    if (*q == '/' || *q == '\\') cut = q + 1;
  *cut = 0;
  s_pick_n = 0;
  int count = 0;
  char **list = SDL_GlobDirectory(s_pick_dir[0] ? s_pick_dir : ".", "*", SDL_GLOB_CASEINSENSITIVE, &count);
  for (int i = 0; list && i < count && s_pick_n < 64; i++) {
    const char *nm = list[i];
    size_t L = strlen(nm);
    if (L < 5 || L >= sizeof s_pick_name[0]) continue;
    if (SDL_strcasecmp(nm + L - 4, ".sfc") && SDL_strcasecmp(nm + L - 4, ".smc")) continue;
    if (SDL_strchr(nm, '/') || SDL_strchr(nm, '\\')) continue; /* files in this folder only */
    SDL_strlcpy(s_pick_name[s_pick_n++], nm, sizeof s_pick_name[0]);
  }
  SDL_free(list);
  for (int i = 1; i < s_pick_n; i++) /* alphabetical */
    for (int j = i; j > 0 && SDL_strcasecmp(s_pick_name[j - 1], s_pick_name[j]) > 0; j--) {
      char t[128];
      SDL_strlcpy(t, s_pick_name[j], sizeof t);
      SDL_strlcpy(s_pick_name[j], s_pick_name[j - 1], sizeof t);
      SDL_strlcpy(s_pick_name[j - 1], t, sizeof t);
    }
}

static int pick_loaded_index(void) {
  const char *name = s_loaded_rom;
  for (const char *q = s_loaded_rom; *q; q++)
    if (*q == '/' || *q == '\\') name = q + 1;
  for (int i = 0; i < s_pick_n; i++)
    if (!SDL_strcasecmp(s_pick_name[i], name)) return i;
  return 0;
}

/* the picker: the folder's ROMs, then "Browse..." and "Quit", in the plain window frame */
static void rom_picker_draw(int cur, const char *msg) {
  int ext = g_cfg.widescreen ? ws_ext() : 0, tex_w = 256 + 2 * ext;
  menu_begin(s_menu_cv, tex_w, PPU_H);
  const int W = 240, VIS = 7;
  int rows = s_pick_n + 2 < VIS ? s_pick_n + 2 : VIS;
  int H = 46 + rows * 15 + (msg[0] ? 58 : 6) + 8;
  int x = (tex_w - W) / 2, y = (PPU_H - H) / 2;
  menu_window(x, y, W, H, 0);
  uint32_t ink = menu_ink(0), dim = menu_dim(0);
  menu_text(x + 12, y + 8, "BoundedToPC", ink);
  /* which folder: its last part */
  char folder[128] = {0};
  {
    size_t L = strlen(s_pick_dir);
    while (L && (s_pick_dir[L - 1] == '/' || s_pick_dir[L - 1] == '\\')) L--;
    size_t b = L;
    while (b && s_pick_dir[b - 1] != '/' && s_pick_dir[b - 1] != '\\') b--;
    size_t n = L - b < sizeof folder - 1 ? L - b : sizeof folder - 1;
    SDL_memcpy(folder, s_pick_dir + b, n);
  }
  int fx = menu_text(x + 12, y + 26, "Choose your ROM in ", dim);
  menu_text(fx, y + 26, folder[0] ? folder : "this folder", ink);
  int n_items = s_pick_n + 2, first = cur - VIS / 2;
  if (first > n_items - VIS) first = n_items - VIS;
  if (first < 0) first = 0;
  int loaded = pick_loaded_index();
  for (int r = 0; r < VIS && first + r < n_items; r++) {
    int i = first + r, ry = y + 46 + r * 15;
    char shown[128];
    if (i < s_pick_n) SDL_strlcpy(shown, s_pick_name[i], sizeof shown);
    else SDL_strlcpy(shown, i == s_pick_n ? "Browse..." : "Quit", sizeof shown);
    while (shown[0] && menu_text_width(shown) > W - 44) shown[strlen(shown) - 1] = 0;
    menu_text(x + 26, ry, shown, (i < s_pick_n && i != loaded) ? dim : ink);
    if (i == cur) menu_cursor(x + 20, ry, ink);
  }
  if (first > 0) menu_text(x + W - 20, y + 46, "^", dim);
  if (first + VIS < n_items) menu_text(x + W - 20, y + 46 + (VIS - 1) * 15, "v", dim);
  if (msg[0]) wrap_text(x + 12, y + 46 + rows * 15 + 4, W - 24, msg, dim);
  int ww, wh;
  SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
  if (ww > 0 && wh > 0) {
    ensure_present_fbo(ww, wh);
    gl.BindFramebuffer(GL_FRAMEBUFFER, s_pfbo);
    gl.Viewport(0, 0, ww, wh);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    draw_menu_canvas(tex_w);
    show_pfbo();
  }
}

/* before the game starts: pick one of the folder's ROMs (each is checked when picked: only the
 * retail EarthBound (USA) dump works), browse for one elsewhere, or quit; drawn with the loaded
 * ROM's own font */
static void rom_picker(void) {
  char msg[600] = {0};
  scan_rom_dir();
  int cur = pick_loaded_index(), held = 0;
  uint16_t prev = 0xFFFF;
  for (;;) {
    SDL_Event e;
    int esc = 0;
    while (SDL_PollEvent(&e)) {
      handle_pad_device(&e);
      if (e.type == SDL_EVENT_QUIT) quit_before_start();
      if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE) esc = 1;
    }
    int n_items = s_pick_n + 2;
    uint16_t press = menu_press(read_input(), &prev, &held);
    if (press & 0x0800) cur = (cur + n_items - 1) % n_items;
    if (press & 0x0400) cur = (cur + 1) % n_items;
    if (esc || (press & 0x8000)) cur = n_items - 1;
    if (press & (0x0080 | 0x1000)) {
      if (cur == n_items - 1) quit_before_start();
      char keep[1024], err[512] = {0}, path[1300];
      SDL_strlcpy(keep, s_loaded_rom, sizeof keep);
      int picked = 0;
      if (cur < s_pick_n) {
        SDL_snprintf(path, sizeof path, "%s%s", s_pick_dir, s_pick_name[cur]);
        picked = 1;
      } else if (ask_rom_file()) {
        SDL_strlcpy(path, s_dlg_path, sizeof path);
        picked = 1;
      }
      if (picked) {
        if (try_rom(path, err, sizeof err)) break;
        SDL_snprintf(msg, sizeof msg, "That ROM can't be used: %s", err[0] ? err : "it could not be read.");
        try_rom(keep, err, sizeof err); /* back to the ROM that works (its font draws this menu) */
      }
      prev = 0xFFFF;
    }
    rom_picker_draw(cur, msg);
    SDL_Delay(12);
  }
  SDL_strlcpy(s_rom_path, s_loaded_rom, sizeof s_rom_path);
  save_ini();
}

/* ---- the frame ------------------------------------------------------------------------------------- */
void host_frame(void) {
  uint64_t t_start = SDL_GetTicksNS();
  if (s_t_last_end) s_t_emu += t_start - s_t_last_end;
  int ext = g_cfg.widescreen ? ws_ext() : 0;
  g_cfg.ws_ext = ext;
  ppu_sprite_limit = g_cfg.sprite_limit;
  game_before_render(ext);
  ppu_render_frame(s_fb, PPU_MAX_W, ext);
  uint64_t t_rendered = SDL_GetTicksNS();
  s_t_render += t_rendered - t_start;
  int tex_w = 256 + 2 * ext;
  long frame = (long)g_frame_count;

  pump_events();
  /* scripted test runs stay deterministic (a focused test window would otherwise pick up the
   * player's keys); every other run, including copies launched for the player, takes input */
  uint16_t pad = (s_script_n || s_autoplay) ? 0 : read_input();
  while (s_script_i < s_script_n && s_script[s_script_i].frame <= frame) {
    s_script_buttons = s_script[s_script_i].buttons;
    s_script_i++;
  }
  /* the settings menu: Esc (pump_events), the gamepad's Guide button, or Select + Start */
  if (s_pad && !s_test_mode) {
    int guide = SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_GUIDE);
    int chord = (pad & 0x3000) == 0x3000;
    if ((guide && !s_guide_was) || (chord && !s_chord_was)) s_menu = 1;
    s_guide_was = guide;
    s_chord_was = chord;
  }
  s_menu_mask &= pad; /* forget released buttons */
  pad &= (uint16_t)~s_menu_mask;
  g_pad_state[0] = pad | s_script_buttons | autoplay_buttons(frame) | walk_buttons(frame);

  /* audio */
  static int16_t abuf[4096 * 2];
  int n = apu_take_samples(abuf, 4096);
  if (n && rec_active()) rec_audio(abuf, n);
  if (s_audio && n) {
    SDL_PutAudioStreamData(s_audio, abuf, n * 4);
    int queued = SDL_GetAudioStreamQueued(s_audio);
    float target = 4 * 533 * 4.0f;
    float ratio = 1.0f + (queued - target) / (target * 40.0f);
    if (ratio < 0.995f) ratio = 0.995f;
    if (ratio > 1.005f) ratio = 1.005f;
    if (queued > target * 6) SDL_ClearAudioStream(s_audio);
    SDL_SetAudioStreamFrequencyRatio(s_audio, ratio);
  }

  /* present */
  if (s_win) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    ensure_present_fbo(ww, wh);
    int drew3d = render3d_frame(s_win, s_pfbo, ext);
    if (!drew3d) {
      upload_frame(tex_w);
      s_tex_w = tex_w;
      /* shader samples uv*PPU_MAX_W: squeeze uv to the used columns via texsize */
      gl.UseProgram(s_prog);
      present_2d(tex_w);
    }
    /* tests: EB_TEST_SHOT=frame presses F12, EB_TEST_REC=start:stop presses F9 twice */
    const char *ts = SDL_getenv("EB_TEST_SHOT"); /* "frame[,frame...]" */
    for (const char *q = ts; q && *q; q = SDL_strchr(q, ',') ? SDL_strchr(q, ',') + 1 : NULL)
      if (SDL_atoi(q) == frame) s_shot_request = 1;
    /* tests: EB_TEST_REPORT=frame acts as Ctrl+F12, EB_TEST_STATE=frame as the F12 key's report */
    if (SDL_getenv("EB_TEST_REPORT") && SDL_atoi(SDL_getenv("EB_TEST_REPORT")) == frame) s_bug_report = 1;
    if (SDL_getenv("EB_TEST_STATE") && SDL_atoi(SDL_getenv("EB_TEST_STATE")) == frame) s_shot_request = s_state_report = 1;
    const char *tr = SDL_getenv("EB_TEST_REC");
    if (tr && (SDL_atoi(tr) == frame || (SDL_strchr(tr, ':') && SDL_atoi(SDL_strchr(tr, ':') + 1) == frame)))
      s_rec_toggle = 1;
    if (s_shot_request) {
      s_shot_request = 0;
      take_screenshot(ww, wh);
    }
    if (s_rec_toggle) {
      s_rec_toggle = 0;
      toggle_recording(ww, wh);
    }
    if (rec_active()) record_frame(ww, wh);
    update_title();
    /* tests: capture the window, EB_SHOT3D="frame:path.bmp[;frame:path.bmp...]" */
    const char *shot3d = SDL_getenv("EB_SHOT3D");
    char shot_path[600];
    shot_path[0] = 0;
    for (const char *q = shot3d; q && *q; q = SDL_strchr(q, ';') ? SDL_strchr(q, ';') + 1 : NULL) {
      const char *colon = SDL_strchr(q, ':'), *end = SDL_strchr(q, ';');
      if (!colon || (end && colon > end) || SDL_atoi(q) != frame) continue;
      size_t n = end ? (size_t)(end - colon - 1) : SDL_strlen(colon + 1);
      if (n >= sizeof shot_path) n = sizeof shot_path - 1;
      SDL_memcpy(shot_path, colon + 1, n);
      shot_path[n] = 0;
      break;
    }
    if (shot_path[0]) {
      uint8_t *px = (uint8_t *)SDL_malloc((size_t)ww * wh * 4);
      if (px) {
        read_picture(s_pfbo, ww, wh, px);
        FILE *f = fopen(shot_path, "wb");
        if (f) {
          int rowb = ww * 3, pad = (4 - rowb % 4) % 4;
          uint32_t size = 54 + (uint32_t)(rowb + pad) * (uint32_t)wh;
          uint8_t h[54] = {'B', 'M'};
          memcpy(h + 2, &size, 4);
          h[10] = 54;
          h[14] = 40;
          memcpy(h + 18, &ww, 4);
          memcpy(h + 22, &wh, 4);
          h[26] = 1;
          h[28] = 24;
          fwrite(h, 1, 54, f);
          for (int y = 0; y < wh; y++) { /* GL rows are bottom-up like BMP */
            for (int x = 0; x < ww; x++) fwrite(px + ((size_t)y * ww + x) * 4, 1, 3, f);
            for (int p = 0; p < pad; p++) fputc(0, f);
          }
          fclose(f);
          eb_log("wrote 3D capture (drew3d=%d, voxels %d)", drew3d, g_render3d_voxels);
        }
        SDL_free(px);
      }
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s_pfbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.BlitFramebuffer(0, 0, ww, wh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_SwapWindow(s_win);
    /* tests: EB_TEST_MENU=frame:path.bmp opens the settings menu, draws it once and saves it */
    const char *tm = SDL_getenv("EB_TEST_MENU");
    if (tm && SDL_atoi(tm) == frame && SDL_strchr(tm, ':')) {
      settings_draw(tex_w);
      draw_menu_canvas(tex_w);
      uint8_t *px = (uint8_t *)SDL_malloc((size_t)ww * wh * 4);
      if (px) {
        read_picture(s_pfbo, ww, wh, px);
        write_bmp_rgba(SDL_strchr(tm, ':') + 1, px, ww, wh);
        SDL_free(px);
      }
    }
    if (s_menu && !s_test_mode) settings_menu_loop();
  }
  uint64_t t_presented = SDL_GetTicksNS();
  s_t_present += t_presented - t_rendered;

  if ((frame % 60) == 0) flush_sram(0);
  const char *ap = SDL_getenv("EB_TEST_ANTIPIRACY");
  if (ap && SDL_atoi(ap) == frame) test_antipiracy();

  /* test hooks */
  if (s_hash_every && frame % s_hash_every == 0) {
    extern const char *eb_func_name(uint32_t addr);
    extern int g_audio_peak;
    eb_log("lag frames on the map so far: %llu, sprite-glitch NMIs %llu", (unsigned long long)g_lag_frames,
           (unsigned long long)g_glitch_nmis);
    eb_log("frame %ld hash %016llx pad %04x | inidisp %02x mode %d tm %02x | audio %5d | loop $%06X %s",
           frame, (unsigned long long)host_hash_frame(), g_pad_state[0], g_ppu.inidisp,
           g_ppu.bgmode & 7, g_ppu.tm, g_audio_peak, g_poll_addr, eb_func_name(g_poll_addr));
    g_audio_peak = 0;
    if (ext > 0)
      eb_log("  scene %d margin fill %d%%, wide sprites %d, voxels %d", (int)game_scene(),
             margin_fill_pct(ext), g_obj_frame.valid_count, g_cfg.voxel3d ? g_render3d_voxels : 0);
  }
  if (SDL_getenv("EB_SYNC_LOG") && game_scene() == SCENE_OVERWORLD) {
    /* sprite/background sync: world x of each entity anchor = screen anchor + BG1 scroll used
     * for this picture; a standing object must keep the same value while the camera moves */
    char line[512];
    int n = SDL_snprintf(line, sizeof line, "sync f%ld hofs %d vofs %d |", frame, g_ppu.hofs[0], g_ppu.vofs[0]);
    uint16_t seen[16];
    int ns = 0;
    for (int i = 0; i < 128 && ns < 10 && n < (int)sizeof line - 24; i++) {
      if (!g_obj_frame.valid[i]) continue;
      int dup = 0;
      for (int s = 0; s < ns; s++) dup |= seen[s] == g_obj_frame.group[i];
      if (dup) continue;
      seen[ns++] = g_obj_frame.group[i];
      n += SDL_snprintf(line + n, sizeof line - (size_t)n, " %d,%d", g_obj_frame.group_x[i] + g_ppu.hofs[0],
                        g_obj_frame.group_y[i] + g_ppu.vofs[0]);
    }
    eb_log("%s", line);
  }
  const char *shot = SDL_getenv("EB_SHOT");
  if (shot) {
    long f = SDL_atoi(shot);
    const char *p = SDL_strchr(shot, ':');
    if (f == frame && p) {
      write_bmp(p + 1, tex_w);
      eb_log("wrote %s", p + 1);
    }
  }
  extern void eb_text_flush(void);
  eb_text_flush();
  if (s_bug_report) {
    write_bug_report(tex_w, 1);
    s_bug_report = 0;
  } else if (s_state_report) {
    write_bug_report(tex_w, 0);
  }
  s_state_report = 0;
  game_after_frame();
  static uint64_t s_samples_total, s_t0;
  s_samples_total += (uint64_t)n;
  if (s_t0 == 0) s_t0 = SDL_GetTicksNS();
  if (s_test_frames >= 0 && frame >= s_test_frames) {
    double secs = (SDL_GetTicksNS() - s_t0) / 1e9;
    double f = frame > 0 ? (double)frame : 1.0;
    eb_log("test: reached frame %ld after %.1f s (%.2f fps, %.0f audio samples/s)", frame, secs,
           frame / secs, s_samples_total / secs);
    eb_log("test: per frame ms: game %.2f, render %.2f, present %.2f, sleep %.2f, worst frame %.1f",
           s_t_emu / f / 1e6, s_t_render / f / 1e6, s_t_present / f / 1e6, s_t_sleep / f / 1e6,
           s_t_max_frame / 1e6);
    eb_log("test: paced frames %ld, fast-forward frames %ld, pacing resets %ld, sleep asked %.2f ms/frame",
           s_paced_frames, s_ff_frames, s_pace_resets, s_sleep_asked / f / 1e6);
    quit_now();
  }

  /* pacing: on a 60/120/180/240 Hz display, vsync with swap interval k = refresh/60 shows every
   * game frame for exactly k refreshes (perfectly even motion; the audio resampler absorbs 60.0
   * vs 60.1 Hz). Other rates (144, 165 Hz...) can't show 60 fps evenly: present on a 60.1 Hz
   * timer with vsync off (smooth with G-Sync/FreeSync). Fast-forward/test runs: no vsync. */
  uint64_t t_before_sleep = SDL_GetTicksNS();
  int want_interval = (s_fast || s_fast_forward) ? 0 : s_vsync_k;
  if (s_win && want_interval != s_swap_interval) {
    if (SDL_GL_SetSwapInterval(want_interval)) s_swap_interval = want_interval;
    else s_swap_interval = 0, s_vsync_k = 0;
  }
  if (s_fast_forward) s_ff_frames++;
  if (!s_fast && !s_fast_forward && !(s_vsync_k > 0 && s_swap_interval == s_vsync_k)) {
    const uint64_t period = (uint64_t)(1e9 / 60.0988);
    uint64_t now = t_before_sleep;
    if (s_next_ns == 0 || now > s_next_ns + period * 4) {
      s_next_ns = now;
      s_pace_resets++;
    }
    s_next_ns += period;
    if (s_next_ns > now) {
      s_sleep_asked += s_next_ns - now;
      SDL_DelayPrecise(s_next_ns - now);
    }
    s_paced_frames++;
  }
  uint64_t t_end = SDL_GetTicksNS();
  s_t_sleep += t_end - t_before_sleep;
  if (s_t_last_end && t_end - s_t_last_end > s_t_max_frame) s_t_max_frame = t_end - s_t_last_end;
  s_t_last_end = t_end;
}

SDL_Gamepad *host_gamepad(void) { return s_pad; }

void host_fatal(const char *msg) {
  eb_log("fatal: %s", msg);
  flush_sram(0);
  if (rec_active()) rec_stop();
  if (!s_test_mode) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "EarthBound", msg, s_win);
  SDL_Quit();
  exit(1);
}

/* ---- ROM search ---------------------------------------------------------------------------------- */
static int try_rom(const char *path, char *err, size_t errlen) {
  SDL_PathInfo info;
  if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) return 0;
  if (snes_load_rom(path, err, errlen)) {
    eb_log("ROM: %s", path);
    SDL_strlcpy(s_loaded_rom, path, sizeof s_loaded_rom);
    return 1;
  }
  eb_log("%s", err);
  return 0;
}

static int find_rom(int argc, char **argv) {
  char err[512] = {0};
  if (argc > 1 && argv[1][0] != '-' && try_rom(argv[1], err, sizeof err)) return 1;
  if (s_rom_path[0] && try_rom(s_rom_path, err, sizeof err)) return 1;
  const char *names[] = {"EarthBound (USA).sfc", "og roms/EarthBound (USA).sfc", "roms/EarthBound (USA).sfc",
                         "EarthBound.sfc", "earthbound.sfc"};
  char dir[1100];
  SDL_strlcpy(dir, g_base_dir, sizeof dir);
  for (int up = 0; up < 5; up++) {
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
      char p[1300];
      SDL_snprintf(p, sizeof p, "%s%s", dir, names[i]);
      if (try_rom(p, err, sizeof err)) return 1;
    }
    SDL_strlcat(dir, "../", sizeof dir);
  }
  return 0;
}

/* ---- crash reports ------------------------------------------------------------------------------ */
#include <windows.h>
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
  extern const char *eb_func_name(uint32_t addr);
  extern uint32_t g_last_func;
  HMODULE base = GetModuleHandleA(NULL);
  uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
  eb_log("CRASH: exception %08lX at exe+0x%llx, frame %llu, last game function $%06X %s",
         (unsigned long)ep->ExceptionRecord->ExceptionCode, (unsigned long long)(at - (uintptr_t)base),
         (unsigned long long)g_frame_count, g_last_func, eb_func_name(g_last_func));
  eb_log("CRASH: A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X PB=%02X M=%d X=%d", cpu.A, cpu.X, cpu.Y,
         cpu.S, cpu.D, cpu.DB, cpu.PB, cpu.M, cpu.XF);
  flush_sram(0);
  return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
  SetUnhandledExceptionFilter(crash_handler);
  const char *base = SDL_GetBasePath();
  SDL_strlcpy(g_base_dir, base ? base : "./", sizeof g_base_dir);
  const char *lv = SDL_getenv("EB_LOG");
  if (lv) g_log_level = SDL_atoi(lv);
  SDL_snprintf(s_ini_path, sizeof s_ini_path, "%searthbound.ini", g_base_dir);
  load_ini();
  const char *tf = SDL_getenv("EB_TEST_FRAMES");
  if (tf) {
    s_test_frames = SDL_atoi(tf);
    s_test_mode = 1;
  }
  if (SDL_getenv("EB_FAST")) s_fast = 1;
  if (SDL_getenv("EB_AUTOPLAY")) s_autoplay = 1;
  extern int g_log_text;
  if (SDL_getenv("EB_LOG_TEXT")) g_log_text = 1;
  if (SDL_getenv("EB_MUTE")) s_mute = 1;
  const char *he = SDL_getenv("EB_HASH");
  if (he) s_hash_every = SDL_atoi(he);
  const char *in = SDL_getenv("EB_INPUT");
  if (in) parse_script(in);
  const char *ws = SDL_getenv("EB_WIDESCREEN");
  if (ws) g_cfg.widescreen = SDL_atoi(ws);
  const char *vx = SDL_getenv("EB_VOXEL");
  if (vx) g_cfg.voxel3d = SDL_atoi(vx);
  const char *b3 = SDL_getenv("EB_BATTLE3D");
  if (b3) g_cfg.battle3d = SDL_atoi(b3);

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  int have_rom = find_rom(argc, argv);
  int rom_on_cmdline = argc > 1 && argv[1][0] != '-';
  if (!have_rom && (s_test_mode || rom_on_cmdline)) {
    host_fatal("Could not find your EarthBound (USA) ROM.\n\nPut \"EarthBound (USA).sfc\" (3 MB, unheadered "
               "or headered) next to this program, or drag the ROM onto the exe.");
  }
  char savedir[1200];
  /* test runs never touch the real saves */
  SDL_snprintf(savedir, sizeof savedir, "%s%s", g_base_dir, s_test_mode ? "saves_test" : "saves");
  SDL_CreateDirectory(savedir);
  SDL_snprintf(s_save_path, sizeof s_save_path, "%s/earthbound.srm", savedir);

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  int scale = g_cfg.scale < 1 ? 3 : g_cfg.scale;
  int w0 = 400 * scale, h0 = 224 * scale; /* 16:9 at square pixels: 400x224 per scale step */
  SDL_WindowFlags wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (s_test_mode) wflags |= SDL_WINDOW_HIDDEN; /* test runs never pop up over the player's game */
  s_win = SDL_CreateWindow("EarthBound", w0, h0, wflags);
  if (!s_win) host_fatal(SDL_GetError());
  s_glctx = SDL_GL_CreateContext(s_win);
  if (!s_glctx) host_fatal("Could not create an OpenGL 3.3 context. Update your graphics driver.");
  SDL_GL_SetSwapInterval(0);
  classify_display();
  if (g_cfg.fullscreen && !s_test_mode) {
    s_fullscreen = 1;
    set_fullscreen(1);
  }
  setup_gl();
  render3d_init();

  /* tests: EB_TEST_PICKER=path.bmp draws the ROM picker once and saves it */
  if (SDL_getenv("EB_TEST_PICKER") && have_rom) {
    scan_rom_dir();
    rom_picker_draw(pick_loaded_index(), SDL_getenv("EB_TEST_PICKER_MSG") ? SDL_getenv("EB_TEST_PICKER_MSG") : "");
    int ww, wh;
    SDL_GetWindowSizeInPixels(s_win, &ww, &wh);
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)ww * wh * 4);
    if (px) {
      read_picture(s_pfbo, ww, wh, px);
      write_bmp_rgba(SDL_getenv("EB_TEST_PICKER"), px, ww, wh);
      SDL_free(px);
    }
  }
  /* the ROM picker: unless a test or the command line chose the ROM */
  if (!s_test_mode && !rom_on_cmdline) {
    while (!have_rom) {
      char err[512] = {0};
      if (!ask_rom_file()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "BoundedToPC",
                                 "BoundedToPC needs your EarthBound (USA) ROM (3 MB .sfc) to run.", s_win);
        quit_before_start();
      }
      have_rom = try_rom(s_dlg_path, err, sizeof err);
      if (!have_rom)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "BoundedToPC", err[0] ? err : "That file could not be read.", s_win);
    }
    rom_picker();
  }

  SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 32000};
  s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (s_audio) {
    SDL_SetAudioStreamGain(s_audio, s_mute ? 0.0f : g_cfg.volume);
    SDL_ResumeAudioStreamDevice(s_audio);
  } else {
    eb_log("no audio: %s", SDL_GetError());
  }

  const char *oc = SDL_getenv("EB_CPU_OVERCLOCK");
  if (oc) g_cfg.cpu_overclock = SDL_atoi(oc);
  if (g_cfg.cpu_overclock < 0) g_cfg.cpu_overclock = 0;
  if (g_cfg.cpu_overclock > 400) g_cfg.cpu_overclock = 400;
  g_frame_cycles = SNES_NOMINAL_FRAME_CYCLES * (uint32_t)(100 + g_cfg.cpu_overclock) / 100u;
  g_cyc_limit = g_frame_cycles;
  extern int g_no_lag;
  const char *nl = SDL_getenv("EB_NO_LAG");
  if (nl) g_cfg.no_lag = SDL_atoi(nl);
  g_no_lag = g_cfg.no_lag;
  eb_log("CPU time per frame: %u cycles (%d%% extra)", g_frame_cycles, g_cfg.cpu_overclock);
  snes_reset();
  load_sram();
  game_init();
  g_host.frame = host_frame;
  snes_run();
  return 0;
}
