/* F9 video recording: MP4 (H.264 video + AAC audio) through Windows Media Foundation, which is
 * part of Windows, so nothing extra has to be installed. The game thread hands over each frame
 * (as read back from the window) and its audio; a worker thread does all the encoding.
 *
 * Timestamps come from the game clock (frame n at n / 60.0988 s, audio by sample count), so a
 * recording plays back at exactly the game's speed even if the PC stuttered while recording. */
#define COBJMACROS
#include <windows.h>
/* after windows.h: */
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <SDL3/SDL.h>

#include "recorder.h"
#include "snes.h"

#define NSLOT 8
#define AUD_RATE 48000
#define AUD_CAP (AUD_RATE * 2 * 4) /* 4 s of stereo samples */

typedef struct Slot {
  uint8_t *px; /* bottom-up BGRA, w*h*4 */
  int full;
  int64_t index;
} Slot;

static struct {
  SDL_Thread *thread;
  SDL_Mutex *mu;
  SDL_Condition *cv;
  int active, stop, failed, started;
  int w, h;
  wchar_t path[1100];
  Slot slot[NSLOT];
  int64_t next_index; /* game frames since the recording started */
  int64_t dropped;
  /* audio, resampled 32 kHz -> 48 kHz on the game thread */
  int16_t aud[AUD_CAP];
  int aud_n; /* samples (not frames) waiting */
  int64_t aud_written; /* stereo frames handed to the encoder */
  double rpos;
  int16_t prev[2];
} R;

int rec_active(void) { return R.active; }
int rec_width(void) { return R.w; }
int rec_height(void) { return R.h; }

static IMFMediaType *video_type(GUID subtype, int w, int h, UINT32 bitrate) {
  IMFMediaType *t = NULL;
  if (FAILED(MFCreateMediaType(&t))) return NULL;
  IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
  IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &subtype);
  if (bitrate) IMFMediaType_SetUINT32(t, &MF_MT_AVG_BITRATE, bitrate);
  IMFMediaType_SetUINT32(t, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  IMFMediaType_SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)(UINT32)w << 32) | (UINT32)h);
  IMFMediaType_SetUINT64(t, &MF_MT_FRAME_RATE, ((UINT64)60 << 32) | 1u);
  IMFMediaType_SetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1u);
  return t;
}

static IMFMediaType *audio_type(GUID subtype, UINT32 avg_bytes) {
  IMFMediaType *t = NULL;
  if (FAILED(MFCreateMediaType(&t))) return NULL;
  IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
  IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &subtype);
  IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, AUD_RATE);
  IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_NUM_CHANNELS, 2);
  IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avg_bytes);
  if (IsEqualGUID(&subtype, &MFAudioFormat_PCM)) IMFMediaType_SetUINT32(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
  return t;
}

static HRESULT write_buffer(IMFSinkWriter *wr, DWORD stream, const void *data, DWORD bytes, LONGLONG t, LONGLONG d,
                            int flip_rows, int row_bytes, int rows) {
  IMFMediaBuffer *buf = NULL;
  IMFSample *s = NULL;
  BYTE *p = NULL;
  HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
  if (SUCCEEDED(hr)) hr = IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
  if (SUCCEEDED(hr)) {
    if (flip_rows) {
      for (int y = 0; y < rows; y++)
        memcpy(p + (size_t)y * row_bytes, (const BYTE *)data + (size_t)(rows - 1 - y) * row_bytes, (size_t)row_bytes);
    } else {
      memcpy(p, data, bytes);
    }
    IMFMediaBuffer_Unlock(buf);
    hr = IMFMediaBuffer_SetCurrentLength(buf, bytes);
  }
  if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
  if (SUCCEEDED(hr)) hr = IMFSample_AddBuffer(s, buf);
  if (SUCCEEDED(hr)) hr = IMFSample_SetSampleTime(s, t);
  if (SUCCEEDED(hr)) hr = IMFSample_SetSampleDuration(s, d);
  if (SUCCEEDED(hr)) hr = IMFSinkWriter_WriteSample(wr, stream, s);
  if (s) IMFSample_Release(s);
  if (buf) IMFMediaBuffer_Release(buf);
  return hr;
}

static LONGLONG frame_time(int64_t i) { return (LONGLONG)((double)i * 1e7 / 60.0988); }

static int worker(void *arg) {
  (void)arg;
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  int com = SUCCEEDED(hr);
  hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  int mf = SUCCEEDED(hr);
  IMFSinkWriter *wr = NULL;
  IMFAttributes *attr = NULL;
  DWORD vs = 0, as = 0;
  int w = R.w, h = R.h;
  UINT32 bitrate = (UINT32)SDL_min((double)w * h * 60.0 * 0.3, 40e6);
  if (bitrate < 4000000) bitrate = 4000000;
  if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 2);
  if (SUCCEEDED(hr)) IMFAttributes_SetUINT32(attr, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
  if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(R.path, NULL, attr, &wr);
  IMFMediaType *vout = NULL, *vin = NULL, *aout = NULL, *ain = NULL;
  if (SUCCEEDED(hr)) {
    vout = video_type(MFVideoFormat_H264, w, h, bitrate);
    vin = video_type(MFVideoFormat_RGB32, w, h, 0);
    if (vin) IMFMediaType_SetUINT32(vin, &MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4)); /* top-down rows */
    aout = audio_type(MFAudioFormat_AAC, 24000);
    ain = audio_type(MFAudioFormat_PCM, AUD_RATE * 4);
    hr = (vout && vin && aout && ain) ? S_OK : E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(wr, vout, &vs);
  if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(wr, vs, vin, NULL);
  if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(wr, aout, &as);
  if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(wr, as, ain, NULL);
  if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(wr);
  if (vout) IMFMediaType_Release(vout);
  if (vin) IMFMediaType_Release(vin);
  if (aout) IMFMediaType_Release(aout);
  if (ain) IMFMediaType_Release(ain);
  if (attr) IMFAttributes_Release(attr);

  SDL_LockMutex(R.mu);
  R.started = 1;
  if (FAILED(hr)) {
    R.failed = 1;
    eb_log("recording: could not start the encoder (error %08lX)", (unsigned long)hr);
  }
  SDL_SignalCondition(R.cv);
  SDL_UnlockMutex(R.mu);

  int16_t *abuf = (int16_t *)SDL_malloc(AUD_CAP * sizeof(int16_t));
  int64_t frames_written = 0;
  while (SUCCEEDED(hr)) {
    SDL_LockMutex(R.mu);
    int pick = -1;
    int64_t best = INT64_MAX;
    for (int i = 0; i < NSLOT; i++)
      if (R.slot[i].full == 2 && R.slot[i].index < best) best = R.slot[i].index, pick = i;
    int an = R.aud_n;
    if (an && abuf) {
      memcpy(abuf, R.aud, (size_t)an * sizeof(int16_t));
      R.aud_n = 0;
    }
    int stop = R.stop;
    if (pick < 0 && !an && !stop) {
      SDL_WaitCondition(R.cv, R.mu);
      SDL_UnlockMutex(R.mu);
      continue;
    }
    SDL_UnlockMutex(R.mu);
    if (an && abuf) {
      int64_t t0 = R.aud_written;
      int frames = an / 2;
      hr = write_buffer(wr, as, abuf, (DWORD)(an * 2), (LONGLONG)(t0 * 10000000 / AUD_RATE),
                        (LONGLONG)((int64_t)frames * 10000000 / AUD_RATE), 0, 0, 0);
      R.aud_written += frames;
    }
    if (pick >= 0 && SUCCEEDED(hr)) {
      Slot *s = &R.slot[pick];
      hr = write_buffer(wr, vs, s->px, (DWORD)(w * h * 4), frame_time(s->index),
                        frame_time(s->index + 1) - frame_time(s->index), 1, w * 4, h);
      frames_written++;
      SDL_LockMutex(R.mu);
      s->full = 0;
      SDL_UnlockMutex(R.mu);
    }
    if (stop && pick < 0 && !an) break;
  }
  if (FAILED(hr)) eb_log("recording: encoder error %08lX", (unsigned long)hr);
  if (wr) {
    HRESULT fh = IMFSinkWriter_Finalize(wr);
    if (FAILED(fh) && SUCCEEDED(hr)) eb_log("recording: could not finish the file (error %08lX)", (unsigned long)fh);
    IMFSinkWriter_Release(wr);
  }
  SDL_free(abuf);
  eb_log("recording: %lld frames written, %lld dropped", (long long)frames_written, (long long)R.dropped);
  if (mf) MFShutdown();
  if (com) CoUninitialize();
  SDL_LockMutex(R.mu);
  R.failed |= FAILED(hr);
  SDL_UnlockMutex(R.mu);
  return 0;
}

int rec_start(const char *path_utf8, int w, int h) {
  if (R.active) return 1;
  if (!R.mu) {
    R.mu = SDL_CreateMutex();
    R.cv = SDL_CreateCondition();
  }
  w &= ~1;
  h &= ~1;
  if (w < 16 || h < 16) return 0;
  MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, R.path, (int)(sizeof R.path / sizeof R.path[0]));
  for (int i = 0; i < NSLOT; i++) {
    R.slot[i].px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
    R.slot[i].full = 0;
    if (!R.slot[i].px) {
      for (int k = 0; k <= i; k++) SDL_free(R.slot[k].px), R.slot[k].px = NULL;
      return 0;
    }
  }
  R.w = w;
  R.h = h;
  R.stop = R.failed = R.started = 0;
  R.next_index = 0;
  R.dropped = 0;
  R.aud_n = 0;
  R.aud_written = 0;
  R.rpos = 0;
  R.prev[0] = R.prev[1] = 0;
  R.thread = SDL_CreateThread(worker, "recorder", NULL);
  if (!R.thread) return 0;
  SDL_LockMutex(R.mu);
  while (!R.started) SDL_WaitCondition(R.cv, R.mu);
  int failed = R.failed;
  SDL_UnlockMutex(R.mu);
  R.active = !failed;
  if (failed) {
    rec_stop();
    DeleteFileW(R.path); /* the encoder never started: no half-made file */
  }
  return R.active;
}

/* game thread: a free slot to read the next frame into (NULL = encoder behind, frame dropped) */
uint8_t *rec_begin_frame(void) {
  if (!R.active) return NULL;
  uint8_t *p = NULL;
  SDL_LockMutex(R.mu);
  for (int i = 0; i < NSLOT; i++) {
    if (!R.slot[i].full) {
      R.slot[i].full = 1;
      R.slot[i].index = R.next_index;
      p = R.slot[i].px;
      break;
    }
  }
  if (!p) R.dropped++;
  R.next_index++;
  SDL_UnlockMutex(R.mu);
  return p;
}

void rec_end_frame(uint8_t *px) {
  if (!px) return;
  SDL_LockMutex(R.mu);
  for (int i = 0; i < NSLOT; i++)
    if (R.slot[i].px == px) R.slot[i].full = 2;
  SDL_SignalCondition(R.cv);
  SDL_UnlockMutex(R.mu);
}

/* game thread: this frame's audio (stereo, 32 kHz) -> 48 kHz for the AAC encoder */
void rec_audio(const int16_t *in, int n) {
  if (!R.active || n <= 0) return;
  int16_t out[4096 * 2];
  int no = 0;
  while (R.rpos < n - 1 && no < 4096) {
    double e = R.rpos + 1.0;
    int i = (int)e;
    double f = e - i;
    const int16_t *a = i == 0 ? R.prev : &in[(i - 1) * 2];
    const int16_t *b = &in[i * 2];
    out[no * 2] = (int16_t)(a[0] + (b[0] - a[0]) * f);
    out[no * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
    no++;
    R.rpos += 32000.0 / AUD_RATE;
  }
  R.rpos -= n;
  R.prev[0] = in[(n - 1) * 2];
  R.prev[1] = in[(n - 1) * 2 + 1];
  SDL_LockMutex(R.mu);
  int room = AUD_CAP - R.aud_n;
  int take = SDL_min(room, no * 2);
  memcpy(R.aud + R.aud_n, out, (size_t)take * sizeof(int16_t));
  R.aud_n += take;
  SDL_SignalCondition(R.cv);
  SDL_UnlockMutex(R.mu);
}

void rec_stop(void) {
  if (!R.thread) return;
  SDL_LockMutex(R.mu);
  R.stop = 1;
  SDL_SignalCondition(R.cv);
  SDL_UnlockMutex(R.mu);
  SDL_WaitThread(R.thread, NULL);
  R.thread = NULL;
  for (int i = 0; i < NSLOT; i++) {
    SDL_free(R.slot[i].px);
    R.slot[i].px = NULL;
    R.slot[i].full = 0;
  }
  R.active = 0;
}
