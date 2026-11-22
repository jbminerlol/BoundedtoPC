#pragma once
#include <stdint.h>

/* MP4 recording (H.264 + AAC via Windows Media Foundation), see recorder.c */
int rec_start(const char *path_utf8, int w, int h); /* 1 = recording; w/h rounded down to even */
void rec_stop(void);                                  /* finishes the file (waits for the encoder) */
int rec_active(void);
int rec_width(void);
int rec_height(void);
uint8_t *rec_begin_frame(void);   /* buffer for this frame's w*h BGRA pixels, bottom-up; NULL = drop */
void rec_end_frame(uint8_t *px);  /* hands the filled buffer to the encoder */
void rec_audio(const int16_t *stereo32k, int frames);
