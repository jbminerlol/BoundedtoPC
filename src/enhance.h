#pragma once
#include <stdint.h>

/* per-OAM-entry info for the OAM currently on screen (filled at the NMI's OAM DMA) */
typedef struct ObjFrameInfo {
  uint16_t group[128];   /* entries written by the same spritemap call share a group */
  int16_t group_x[128];  /* that call's base position (entity anchor, screen coords) */
  int16_t group_y[128];
  int8_t slot[128];      /* entity slot that drew it (-1: not an entity) */
  uint8_t valid[128];
  int valid_count;
} ObjFrameInfo;

extern ObjFrameInfo g_obj_frame;
extern uint16_t g_ws_ext, g_ws_cols;
void eb_oam_dma_source(uint32_t abus);
