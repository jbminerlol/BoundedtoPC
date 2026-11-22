#pragma once
#include <stdint.h>

typedef struct EbConfig {
  int widescreen;     /* 1: draw the extended field of view */
  int ws_ext;         /* pixels added on each side (computed from the aspect) */
  int square_pixels;  /* 0: SNES 8:7 pixel aspect */
  int filter;         /* 0 nearest, 1 sharp bilinear, 2 bilinear */
  int integer_scale;  /* pixel perfect: whole-number scale only */
  int voxel3d;        /* 3D voxel overworld */
  int battle3d;       /* 3D battles */
  int inventory_plus; /* bigger inventory (QOL) */
  int sprite_limit;   /* SNES sprite-per-line limit */
  float volume;
  int scale;          /* initial window scale */
  int cpu_overclock;  /* extra CPU time per frame in percent (0 = like the real SNES) */
  int fill_letterbox; /* battle backgrounds fill the screen (no letterbox band) */
  int fullscreen;     /* start fullscreen (even 60 Hz pacing on 144 Hz monitors) */
  int no_lag;         /* busy frames get extra CPU time instead of lagging (no sprite glitches) */
} EbConfig;

extern EbConfig g_cfg;
extern char g_base_dir[1024];

void host_fatal(const char *msg);
struct SDL_Gamepad;
struct SDL_Gamepad *host_gamepad(void);
void host_frame(void);
void host_note(const char *msg); /* short note in the window title */
uint64_t host_hash_frame(void);
