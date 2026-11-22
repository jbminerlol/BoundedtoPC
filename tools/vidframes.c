/* vidframes: pull frames out of a video (MP4 etc.) with Windows Media Foundation, the decoder
 * that ships with Windows, so bug videos can be looked at frame by frame.
 *
 *   vidframes VIDEO OUTDIR [start_s] [end_s] [every] [crop x,y,w,h] [scale]
 *
 * Writes OUTDIR/f<index>_<ms>.png for every `every`-th decoded frame between start_s and end_s
 * (optionally cropped, and scaled down by an integer factor) and prints one line per decoded
 * frame: index, timestamp, and how much it differs from the previous frame (mean abs diff),
 * which shows repeated frames and one-frame glitches. Build (llvm-mingw):
 *   clang -O2 -o vidframes.exe tools/vidframes.c src/png.c -Isrc -lmfplat -lmfreadwrite -lmfuuid -lole32
 */
#define COBJMACROS
#include <windows.h>
/* after windows.h */
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: vidframes VIDEO OUTDIR [start_s] [end_s] [every] [crop x,y,w,h] [scale]\n");
    return 2;
  }
  double t0 = argc > 3 ? atof(argv[3]) : 0.0, t1 = argc > 4 ? atof(argv[4]) : 1e9;
  int every = argc > 5 ? atoi(argv[5]) : 1;
  int cx = 0, cy = 0, cw = 0, ch = 0;
  if (argc > 6 && strcmp(argv[6], "-")) sscanf(argv[6], "%d,%d,%d,%d", &cx, &cy, &cw, &ch);
  int scale = argc > 7 ? atoi(argv[7]) : 1;
  if (every < 1) every = 1;
  if (scale < 1) scale = 1;
  CreateDirectoryA(argv[2], NULL);

  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(hr)) {
    fprintf(stderr, "Media Foundation start failed %08lx\n", (unsigned long)hr);
    return 1;
  }
  wchar_t wpath[2048];
  MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wpath, 2048);
  IMFAttributes *attr = NULL;
  MFCreateAttributes(&attr, 1);
  IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  IMFSourceReader *rd = NULL;
  hr = MFCreateSourceReaderFromURL(wpath, attr, &rd);
  if (FAILED(hr)) {
    fprintf(stderr, "cannot open %s (%08lx)\n", argv[1], (unsigned long)hr);
    return 1;
  }
  IMFMediaType *mt = NULL;
  MFCreateMediaType(&mt);
  IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
  IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
  hr = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
  IMFMediaType_Release(mt);
  if (FAILED(hr)) {
    fprintf(stderr, "cannot decode to RGB (%08lx)\n", (unsigned long)hr);
    return 1;
  }
  IMFMediaType *cur = NULL;
  IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
  UINT64 fs = 0;
  IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs);
  int w = (int)(fs >> 32), h = (int)(fs & 0xFFFFFFFF);
  UINT32 stride_u = 0;
  int stride = w * 4;
  if (SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &stride_u))) stride = (int)(INT32)stride_u;
  IMFMediaType_Release(cur);
  if (cw <= 0 || ch <= 0) cx = 0, cy = 0, cw = w, ch = h;
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx + cw > w) cw = w - cx;
  if (cy + ch > h) ch = h - cy;
  printf("video %dx%d stride %d, crop %d,%d %dx%d, scale 1/%d\n", w, h, stride, cx, cy, cw, ch, scale);

  uint8_t *prev = (uint8_t *)calloc((size_t)w * h, 4), *frame = (uint8_t *)malloc((size_t)w * h * 4);
  int ow = cw / scale, oh = ch / scale;
  uint8_t *out = (uint8_t *)malloc((size_t)(ow > 0 ? ow : 1) * (oh > 0 ? oh : 1) * 4);
  int index = 0, have_prev = 0;
  for (;;) {
    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0;
    IMFSample *smp = NULL;
    hr = IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &smp);
    if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
    if (!smp) continue;
    IMFMediaBuffer *buf = NULL;
    IMFSample_ConvertToContiguousBuffer(smp, &buf);
    BYTE *p = NULL;
    DWORD len = 0;
    IMFMediaBuffer_Lock(buf, &p, NULL, &len);
    /* to top-down BGRA */
    int astride = stride < 0 ? -stride : stride;
    for (int y = 0; y < h; y++) {
      const BYTE *src = stride < 0 ? p + (size_t)(h - 1 - y) * astride : p + (size_t)y * astride;
      if ((size_t)((y + 1) * astride) > len) break;
      memcpy(frame + (size_t)y * w * 4, src, (size_t)w * 4);
    }
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(smp);
    double sec = ts / 1e7;
    /* mean abs difference to the previous frame inside the crop */
    double diff = 0;
    if (have_prev) {
      long long sum = 0;
      for (int y = cy; y < cy + ch; y += 2)
        for (int x = cx; x < cx + cw; x += 2) {
          const uint8_t *a = frame + ((size_t)y * w + x) * 4, *b = prev + ((size_t)y * w + x) * 4;
          sum += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
        }
      diff = (double)sum / ((double)((cw + 1) / 2) * ((ch + 1) / 2) * 3);
    }
    printf("%5d %9.3f s  diff %6.2f\n", index, sec, diff);
    if (sec >= t0 && sec <= t1 && (index % every) == 0 && ow > 0 && oh > 0) {
      for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) memcpy(out + ((size_t)y * ow + x) * 4, frame + ((size_t)(cy + y * scale) * w + cx + x * scale) * 4, 4);
      char path[1200];
      snprintf(path, sizeof path, "%s/f%05d_%07d.png", argv[2], index, (int)(sec * 1000 + 0.5));
      png_write_bgra(path, out, ow, oh, ow * 4, 0);
    }
    memcpy(prev, frame, (size_t)w * h * 4);
    have_prev = 1;
    index++;
    if (sec > t1 + 1.0) break;
  }
  printf("%d frames\n", index);
  IMFSourceReader_Release(rd);
  MFShutdown();
  CoUninitialize();
  return 0;
}
