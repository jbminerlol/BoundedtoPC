/* EarthBound-specific hooks: game state queries and enhancement patches. */
#pragma once
#include <stdint.h>

void game_init(void);
void game_before_render(int ext);   /* per frame, before the PPU draws (widescreen layer setup) */
void game_after_frame(void);

/* game state as the enhancements see it */
typedef enum GameScene {
  SCENE_OTHER = 0,  /* title, menus, cutscenes: 2D */
  SCENE_OVERWORLD,
  SCENE_BATTLE,
} GameScene;
GameScene game_scene(void);
