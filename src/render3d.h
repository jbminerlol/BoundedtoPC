#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>

void render3d_init(void);
void render3d_reset_camera(void); /* settings menu */
void render3d_event(const SDL_Event *e); /* camera: right-drag orbit, wheel zoom, F6/middle reset */
extern int g_render3d_voxels;            /* voxels drawn last frame (test logging) */
/* Draws the current frame in 3D (voxel overworld / 3D battle) when enabled and the game is in a
 * suitable scene; returns 0 to let the caller present the normal 2D frame. */
int render3d_frame(SDL_Window *win, unsigned target_fbo, int ext); /* draws into target_fbo */
