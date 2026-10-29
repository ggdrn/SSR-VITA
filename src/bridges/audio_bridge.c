// Audio output for both engines in the APK:
//  * CRI ADX2 (libcri_ware_unity.so) talks OpenSL ES -> minimal OpenSL ES
//    engine/output-mix/buffer-queue player implemented over SceAudioOut.
//  * Unity's FMOD expects the Java org.fmod.FMODAudioDevice pump thread ->
//    emulated by a thread that calls fmodProcess() and feeds SceAudioOut.

#include "bridges.h"
#include "../loader/so_util.h"
#include "../jni/jni.h"
#include "../util/log.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

// ============================================================================
// OpenSL ES
// ============================================================================

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PARAMETER_INVALID 2
#define SL_RESULT_FEATURE_UNSUPPORTED 12
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3
#define SL_OBJECT_STATE_REALIZED 2
#define SL_DATAFORMAT_PCM 2

typedef struct { uint32_t d[4]; } SLInterfaceID_;
const SLInterfaceID_ SL_IID_ENGINE_val = {{0x8d97c260, 0xddd4, 0x11db, 0x958f}};
const SLInterfaceID_ SL_IID_PLAY_val = {{0xef0bd9c0, 0xddd7, 0x11db, 0xbf49}};
const SLInterfaceID_ SL_IID_BUFFERQUEUE_val = {{0x2bc99cc0, 0xddd4, 0x11db, 0x8d99}};
const SLInterfaceID_ SL_IID_VOLUME_val = {{0x09e8ede0, 0xddde, 0x11db, 0xb4f6}};
const SLInterfaceID_ SL_IID_ANDROIDSIMPLEBUFFERQUEUE_val = {{0x198e4940, 0xc5d7, 0x11df, 0xa2a6}};
const SLInterfaceID_ *SL_IID_ENGINE_bridge = &SL_IID_ENGINE_val;
const SLInterfaceID_ *SL_IID_PLAY_bridge = &SL_IID_PLAY_val;
const SLInterfaceID_ *SL_IID_BUFFERQUEUE_bridge = &SL_IID_BUFFERQUEUE_val;
const SLInterfaceID_ *SL_IID_VOLUME_bridge = &SL_IID_VOLUME_val;

static int iid_eq(const SLInterfaceID_ *a, const SLInterfaceID_ *b) { return a && memcmp(a, b, sizeof(*a)) == 0; }

typedef struct {
  SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample, containerSize, channelMask, endianness;
} SLDataFormat_PCM;
typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

// Every SL interface is a pointer to a vtable pointer; we embed the vtable
// pointer as the first field of each interface struct so `self` can be cast
// back to the owning object.

typedef struct sl_object sl_object;

typedef struct {
  SLresult (*Realize)(void *self, uint32_t async);
  SLresult (*Resume)(void *self, uint32_t async);
  SLresult (*GetState)(void *self, SLuint32 *state);
  SLresult (*GetInterface)(void *self, const SLInterfaceID_ *iid, void *out);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  void (*AbortAsyncOperation)(void *self);
  void (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, int32_t prio, uint32_t preemptable);
  SLresult (*GetPriority)(void *self, int32_t *prio, uint32_t *preemptable);
  SLresult (*SetLossOfControlInterfaces)(void *self, int16_t n, void *ids, uint32_t enabled);
} SLObjectItf_;

typedef struct {
  void *CreateLEDDevice, *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, void *out, SLDataSource *src, void *snk, SLuint32 n, const void *ids,
                                const void *req);
  void *CreateAudioRecorder, *CreateMidiPlayer, *CreateListener, *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, void *out, SLuint32 n, const void *ids, const void *req);
  void *CreateMetadataExtractor, *CreateExtensionObject, *QueryNumSupportedInterfaces, *QuerySupportedInterfaces,
      *QueryNumSupportedExtensions, *QuerySupportedExtension, *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *state);
  SLresult (*GetDuration)(void *self, SLuint32 *ms);
  SLresult (*GetPosition)(void *self, SLuint32 *ms);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *mask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 ms);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *ms);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 ms);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *ms);
} SLPlayItf_;

typedef void (*sl_bq_callback)(void *caller, void *ctx);

typedef struct {
  SLresult (*Enqueue)(void *self, const void *buf, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, SLuint32 *state);  // {count, playIndex}
  SLresult (*RegisterCallback)(void *self, sl_bq_callback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, int16_t mb);
  SLresult (*GetVolumeLevel)(void *self, int16_t *mb);
  SLresult (*GetMaxVolumeLevel)(void *self, int16_t *mb);
  SLresult (*SetMute)(void *self, uint32_t mute);
  SLresult (*GetMute)(void *self, uint32_t *mute);
  SLresult (*EnableStereoPosition)(void *self, uint32_t enable);
  SLresult (*IsEnabledStereoPosition)(void *self, uint32_t *enable);
  SLresult (*SetStereoPosition)(void *self, int16_t pos);
  SLresult (*GetStereoPosition)(void *self, int16_t *pos);
} SLVolumeItf_;

#define BQ_MAX 16
#define SL_GRAIN 512
#define SL_PORT_RATE 48000

typedef enum { OBJ_ENGINE, OBJ_MIX, OBJ_PLAYER } obj_type;

struct sl_object {
  const SLObjectItf_ *obj_itf;
  const SLEngineItf_ *engine_itf;
  const SLPlayItf_ *play_itf;
  const SLBufferQueueItf_ *bq_itf;
  const SLVolumeItf_ *vol_itf;
  obj_type type;

  // player state
  int channels, rate, bits;
  int output_rate;
  volatile SLuint32 play_state;
  volatile int16_t volume_mb;
  struct sl_buffer {
    const void *buf;
    SLuint32 size;
  } *bq;
  int bq_capacity;
  SceKernelLwMutexWork bq_lock;
  unsigned bq_generation;
  unsigned enqueued;
  volatile int bq_head, bq_count;
  SLuint32 bq_play_index;
  sl_bq_callback cb;
  void *cb_ctx;
  SceUID sema;
  SceUID thread;
  volatile int quit;
  int port;
  unsigned buffers_seen;
  int heard_pcm;
  unsigned pcm_nonzero;
  uint64_t pcm_samples, pcm_clipped;
  int pcm_peak;
  uint64_t frames_played;
  sl_object *next_player;
};

static sl_object *players;
static SceKernelLwMutexWork players_lock;

#define OBJ_FROM(field, self) ((sl_object *)((char *)(self) - offsetof(sl_object, field)))

static SLresult obj_Realize(void *self, uint32_t async) { return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, uint32_t async) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *s) {
  *s = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void obj_Abort(void *self) {}
static SLresult obj_SetPriority(void *self, int32_t p, uint32_t pre) { return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, int32_t *p, uint32_t *pre) { return SL_RESULT_SUCCESS; }
static SLresult obj_SetLoss(void *self, int16_t n, void *ids, uint32_t en) { return SL_RESULT_SUCCESS; }

static SLresult obj_GetInterface(void *self, const SLInterfaceID_ *iid, void *out) {
  sl_object *o = OBJ_FROM(obj_itf, self);
  void **res = out;
  if (iid_eq(iid, &SL_IID_ENGINE_val) && o->type == OBJ_ENGINE)
    *res = &o->engine_itf;
  else if (iid_eq(iid, &SL_IID_PLAY_val) && o->type == OBJ_PLAYER)
    *res = &o->play_itf;
  else if ((iid_eq(iid, &SL_IID_BUFFERQUEUE_val) || iid_eq(iid, &SL_IID_ANDROIDSIMPLEBUFFERQUEUE_val)) &&
           o->type == OBJ_PLAYER)
    *res = &o->bq_itf;
  else if (iid_eq(iid, &SL_IID_VOLUME_val) && o->type == OBJ_PLAYER)
    *res = &o->vol_itf;
  else {
    L_WARN("opensl: unsupported interface requested on object type %d", o->type);
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void obj_Destroy(void *self) {
  sl_object *o = OBJ_FROM(obj_itf, self);
  if (o->type == OBJ_PLAYER) {
    sceKernelLockLwMutex(&players_lock, 1, NULL);
    for (sl_object **p = &players; *p; p = &(*p)->next_player) {
      if (*p == o) { *p = o->next_player; break; }
    }
    sceKernelUnlockLwMutex(&players_lock, 1);
    o->quit = 1;
    sceKernelSignalSema(o->sema, 1);
    sceKernelWaitThreadEnd(o->thread, NULL, NULL);
    sceKernelDeleteThread(o->thread);
    sceKernelDeleteSema(o->sema);
    sceKernelDeleteLwMutex(&o->bq_lock);
    free(o->bq);
    if (o->port >= 0)
      sceAudioOutReleasePort(o->port);
  }
  free(o);
}

static const SLObjectItf_ object_vtbl = {obj_Realize, obj_Resume, obj_GetState, obj_GetInterface,
                                          obj_RegisterCallback, obj_Abort, obj_Destroy, obj_SetPriority,
                                          obj_GetPriority, obj_SetLoss};

// --- player -----------------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  sl_object *o = OBJ_FROM(play_itf, self);
  L_INFO("opensl: port %d play state %u", o->port, state);
  o->play_state = state;
  sceKernelSignalSema(o->sema, 1);
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *state) {
  *state = OBJ_FROM(play_itf, self)->play_state;
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetZero(void *self, SLuint32 *v) {
  *v = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPosition(void *self, SLuint32 *ms) {
  sl_object *o = OBJ_FROM(play_itf, self);
  sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
  *ms = o->frames_played * 1000 / o->rate;
  sceKernelUnlockLwMutex(&o->bq_lock, 1);
  return SL_RESULT_SUCCESS;
}
static SLresult play_Ok1(void *self, SLuint32 v) { return SL_RESULT_SUCCESS; }
static SLresult play_Ok0(void *self) { return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtbl = {play_SetPlayState, play_GetPlayState, play_GetZero, play_GetPosition,
                                      play_RegisterCallback, play_Ok1, play_GetZero, play_Ok1,
                                      play_Ok0, play_GetZero, play_Ok1, play_GetZero};

static SLresult bq_Enqueue(void *self, const void *buf, SLuint32 size) {
  sl_object *o = OBJ_FROM(bq_itf, self);
  if (!buf || !size || size % (o->channels * sizeof(int16_t)))
    return SL_RESULT_PARAMETER_INVALID;
  sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
  if (o->bq_count >= o->bq_capacity) {
    sceKernelUnlockLwMutex(&o->bq_lock, 1);
    L_WARN("opensl: port %d queue full (%d buffers)", o->port, o->bq_capacity);
    return 7;  // SL_RESULT_BUFFER_INSUFFICIENT
  }
  int idx = (o->bq_head + o->bq_count) % o->bq_capacity;
  o->bq[idx].buf = buf;
  o->bq[idx].size = size;
  o->bq_count++;
  if (++o->enqueued == 1)
    L_INFO("opensl: port %d first enqueue %u bytes, state %u", o->port, size, o->play_state);
  sceKernelUnlockLwMutex(&o->bq_lock, 1);
  sceKernelSignalSema(o->sema, 1);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(void *self) {
  sl_object *o = OBJ_FROM(bq_itf, self);
  // The consumer owns a staging copy; generation prevents its completion
  // from removing a new buffer after Clear returns the old queue to CRI.
  sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
  o->bq_count = o->bq_head = 0;
  o->bq_play_index = 0;
  o->frames_played = 0;
  o->bq_generation++;
  sceKernelUnlockLwMutex(&o->bq_lock, 1);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(void *self, SLuint32 *state) {
  sl_object *o = OBJ_FROM(bq_itf, self);
  sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
  state[0] = o->bq_count;
  state[1] = o->bq_play_index;
  sceKernelUnlockLwMutex(&o->bq_lock, 1);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(void *self, sl_bq_callback cb, void *ctx) {
  sl_object *o = OBJ_FROM(bq_itf, self);
  sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
  o->cb = cb;
  o->cb_ctx = ctx;
  sceKernelUnlockLwMutex(&o->bq_lock, 1);
  L_INFO("opensl: port %d buffer callback registered", o->port);
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtbl = {bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback};

static SLresult vol_Set(void *self, int16_t mb) {
  OBJ_FROM(vol_itf, self)->volume_mb = mb;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_Get(void *self, int16_t *mb) {
  *mb = OBJ_FROM(vol_itf, self)->volume_mb;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMax(void *self, int16_t *mb) {
  *mb = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_U32(void *self, uint32_t v) { return SL_RESULT_SUCCESS; }
static SLresult vol_U32p(void *self, uint32_t *v) {
  *v = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_I16(void *self, int16_t v) { return SL_RESULT_SUCCESS; }
static SLresult vol_I16p(void *self, int16_t *v) {
  *v = 0;
  return SL_RESULT_SUCCESS;
}

static const SLVolumeItf_ vol_vtbl = {vol_Set, vol_Get, vol_GetMax, vol_U32, vol_U32p, vol_U32, vol_U32p,
                                       vol_I16, vol_I16p};

static int vita_rate(int hz) {
  static const int rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
    if (rates[i] == hz)
      return hz;
  }
  L_WARN("audio: unsupported rate %d, using 48000", hz);
  return 48000;
}

static int mb_to_vita_volume(int16_t mb) {
  // millibels (0 = full, -9600 = silent) -> 0..32768
  if (mb <= -9600)
    return 0;
  float lin = 1.0f;
  for (int m = mb; m < 0; m += 600)
    lin *= 0.5f;  // ~ -6 dB per halving
  return (int)(lin * SCE_AUDIO_VOLUME_0DB);
}

static int output_grain(sl_object *o, const int16_t *pcm) {
  if (o->port < 0) return 0;
  int rc = sceAudioOutOutput(o->port, pcm);
  if (rc < 0) {
    L_ERROR("opensl: output failed port %d error 0x%08X", o->port, rc);
    o->quit = 1;
  }
  return rc;
}

static int player_thread(SceSize args, void *argp) {
  sl_object *o = *(sl_object **)argp;
  // Keep the last submitted grain untouched while the device plays it.
  // sceAudioOutOutput queues this pointer; alternate grains before writing.
  int16_t *out_base = memalign(64, SL_GRAIN * 2 * 2 * sizeof(int16_t));
  int out_index = 0;
  int16_t *out = out_base;
  if (!out_base) {
    L_ERROR("opensl: PCM output allocation failed");
    o->quit = 1;
    return 0;
  }
  uint8_t *input = NULL;
  size_t input_capacity = 0;
  int out_frames = 0;
  int last_vol = -1;
  const int in_frame_bytes = o->channels * (o->bits / 8);
  // Fallback resampling when a native-frequency port is unavailable:
  // `phase` (16.16) is the
  // output position between the previous and current input frame.
  int port_rate = o->output_rate > 0 ? o->output_rate : SL_PORT_RATE;
  const uint32_t step = (uint32_t)(((uint64_t)o->rate << 16) / port_rate);
  unsigned generation = 0;
  uint32_t phase = 0;
  int16_t prev[2] = {0, 0};

  while (!o->quit) {
    sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
    if (o->play_state != SL_PLAYSTATE_PLAYING || o->bq_count == 0) {
      sceKernelUnlockLwMutex(&o->bq_lock, 1);
      SceUInt timeout = 10000;
      sceKernelWaitSema(o->sema, 1, &timeout);
      continue;
    }
    if (generation != o->bq_generation) {
      generation = o->bq_generation;
      out_frames = 0; phase = 0; prev[0] = prev[1] = 0;
    }
    int vol = mb_to_vita_volume(o->volume_mb);
    if (vol != last_vol) {
      int vols[2] = {vol, vol};
      sceAudioOutSetVolume(o->port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);
      last_vol = vol;
    }

    // Own the current PCM while the hardware blocks. Holding bq_lock across
    // sceAudioOutOutput made CRI's producer/GetState wait for the audio clock.
    // Clear may now return/reuse the guest buffer without racing this reader.
    size_t bytes = o->bq[o->bq_head].size;
    if (bytes > input_capacity) {
      uint8_t *grown = realloc(input, bytes);
      if (!grown) {
        sceKernelUnlockLwMutex(&o->bq_lock, 1);
        L_ERROR("opensl: PCM staging allocation failed (%u bytes)", (unsigned)bytes);
        o->quit = 1;
        break;
      }
      input = grown;
      input_capacity = bytes;
    }
    memcpy(input, o->bq[o->bq_head].buf, bytes);
    unsigned buffer_generation = o->bq_generation;
    sceKernelUnlockLwMutex(&o->bq_lock, 1);
    const uint8_t *buf = input;
    int frames = bytes / in_frame_bytes;
    int peak = 0;
    unsigned clipped = 0;
    for (int f = 0; f < frames && !o->quit; f++) {
      const int16_t *s = (const int16_t *)(buf + f * in_frame_bytes);
      int16_t cur[2] = {s[0], o->channels > 1 ? s[1] : s[0]};
      {
        int a = abs((int)cur[0]), b = abs((int)cur[1]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
        if (a >= 32760) clipped++;
        if (b >= 32760) clipped++;
      }
      if (o->rate == port_rate) {
        // Native-frequency output preserves every source sample exactly.
        out[out_frames * 2] = cur[0];
        out[out_frames * 2 + 1] = cur[1];
        if (++out_frames == SL_GRAIN) {
          int rc = output_grain(o, out);
          out_index ^= 1;
          out = out_base + out_index * SL_GRAIN * 2;
          out_frames = 0;
          if (rc < 0) break;
        }
      } else {
        while (phase < 0x10000) {
          int32_t w = phase >> 1;
          out[out_frames * 2] = prev[0] + (((cur[0] - prev[0]) * w) >> 15);
          out[out_frames * 2 + 1] = prev[1] + (((cur[1] - prev[1]) * w) >> 15);
          if (++out_frames == SL_GRAIN) {
            int rc = output_grain(o, out);
            out_index ^= 1;
            out = out_base + out_index * SL_GRAIN * 2;
            out_frames = 0;
            if (rc < 0) break;
          }
          phase += step;
        }
        phase -= 0x10000;
      }
      prev[0] = cur[0];
      prev[1] = cur[1];
    }
    sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
    o->pcm_samples += (uint64_t)frames * 2;
    o->pcm_clipped += clipped;
    if (peak > o->pcm_peak) o->pcm_peak = peak;
    if (peak) o->pcm_nonzero++;
    ++o->buffers_seen;
    if (peak && !o->heard_pcm) {
      o->heard_pcm = 1;
      L_INFO("opensl: port %d first nonzero PCM peak %d buffer %u volume %d", o->port, peak, o->buffers_seen, vol);
    } else if (o->buffers_seen == 512 && !o->heard_pcm) {
      L_WARN("opensl: port %d received 512 silent buffers", o->port);
    }
    if (buffer_generation != o->bq_generation) {
      // Clear already returned this buffer; do not consume a newly enqueued
      // one or deliver an obsolete completion callback.
      sceKernelUnlockLwMutex(&o->bq_lock, 1);
      out_frames = 0;
      continue;
    }
    o->bq_head = (o->bq_head + 1) % o->bq_capacity;
    o->bq_play_index++;
    o->frames_played += frames;
    o->bq_count--;
    sl_bq_callback cb = o->cb;
    void *ctx = o->cb_ctx;
    sceKernelUnlockLwMutex(&o->bq_lock, 1);
    if (cb && !o->quit) cb(&o->bq_itf, ctx);
  }
  if (o->port >= 0) sceAudioOutOutput(o->port, NULL); // drain before freeing queued PCM
  free(input);
  free(out_base);
  return 0;  // joined and deleted by obj_Destroy
}

static SLresult engine_CreateAudioPlayer(void *self, void *out, SLDataSource *src, void *snk, SLuint32 n,
                                         const void *ids, const void *req) {
  SLDataFormat_PCM *fmt = src ? src->pFormat : NULL;
  if (!fmt || fmt->formatType != SL_DATAFORMAT_PCM || fmt->bitsPerSample != 16 ||
      (fmt->numChannels != 1 && fmt->numChannels != 2) || fmt->samplesPerSec < 1000000) {
    L_ERROR("opensl: unsupported source format");
    return SL_RESULT_PARAMETER_INVALID;
  }
  const SLuint32 *locator = src->pLocator;
  SLuint32 capacity = locator ? locator[1] : BQ_MAX;
  if (!capacity || capacity > 1024) return SL_RESULT_PARAMETER_INVALID;
  sl_object *o = calloc(1, sizeof(sl_object));
  if (!o) return 3; // SL_RESULT_MEMORY_FAILURE
  o->bq_capacity = capacity;
  o->bq = calloc(capacity, sizeof(*o->bq));
  if (!o->bq) { free(o); return 3; }
  int lock_rc = sceKernelCreateLwMutex(&o->bq_lock, "sl_queue", 0, 0, NULL);
  if (lock_rc < 0) { free(o->bq); free(o); return 4; }
  o->obj_itf = &object_vtbl;
  o->play_itf = &play_vtbl;
  o->bq_itf = &bq_vtbl;
  o->vol_itf = &vol_vtbl;
  o->type = OBJ_PLAYER;
  o->channels = fmt->numChannels;
  o->rate = fmt->samplesPerSec / 1000;  // source rate (milliHz in OpenSL)
  o->bits = 16;
  o->play_state = SL_PLAYSTATE_STOPPED;
  // BGM ports accept 44.1 kHz directly, unlike MAIN's fixed 48 kHz.
  // This avoids the software interpolation that attenuated high frequencies.
  o->output_rate = vita_rate(o->rate);
  o->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, SL_GRAIN, o->output_rate, SCE_AUDIO_OUT_MODE_STEREO);
  if (o->port >= 0) {
    sceAudioOutSetAlcMode(SCE_AUDIO_ALC_OFF);
    L_INFO("opensl: native-frequency BGM output %d Hz", o->output_rate);
  } else {
    L_WARN("opensl: BGM port unavailable 0x%X; using MAIN resampling", o->port);
    o->output_rate = SL_PORT_RATE;
    o->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, SL_GRAIN, o->output_rate, SCE_AUDIO_OUT_MODE_STEREO);
  }
  if (o->port < 0)
    L_ERROR("opensl: sceAudioOutOpenPort failed 0x%08X", o->port);
  o->sema = sceKernelCreateSema("sl_player", 0, 0, 1024, NULL);
  o->thread = sceKernelCreateThread("sl_player", player_thread, 0x10000100 - 10, 0x10000, 0, thread_cpu_mask(), NULL);
  int start_rc = (o->port < 0 || o->sema < 0 || o->thread < 0) ? -1 : sceKernelStartThread(o->thread, sizeof(o), &o);
  if (o->port < 0 || o->sema < 0 || start_rc < 0) {
    L_ERROR("opensl: player resources failed port 0x%X sema 0x%X thread 0x%X start 0x%X", o->port, o->sema, o->thread, start_rc);
    // A successfully started consumer must be stopped before freeing it.
    if (start_rc >= 0) {
      o->quit = 1;
      sceKernelSignalSema(o->sema, 1);
      sceKernelWaitThreadEnd(o->thread, NULL, NULL);
    }
    if (o->thread >= 0) sceKernelDeleteThread(o->thread);
    if (o->sema >= 0) sceKernelDeleteSema(o->sema);
    if (o->port >= 0) sceAudioOutReleasePort(o->port);
    sceKernelDeleteLwMutex(&o->bq_lock);
    free(o->bq); free(o);
    return 4; // SL_RESULT_RESOURCE_ERROR
  }
  L_INFO("opensl: player %d ch %d Hz (port %d, queue %d buffers)", o->channels, o->rate, o->port, o->bq_capacity);
  sceKernelLockLwMutex(&players_lock, 1, NULL);
  o->next_player = players;
  players = o;
  sceKernelUnlockLwMutex(&players_lock, 1);
  *(void **)out = &o->obj_itf;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateOutputMix(void *self, void *out, SLuint32 n, const void *ids, const void *req) {
  sl_object *o = calloc(1, sizeof(sl_object));
  o->obj_itf = &object_vtbl;
  o->type = OBJ_MIX;
  *(void **)out = &o->obj_itf;
  return SL_RESULT_SUCCESS;
}

static SLEngineItf_ engine_vtbl = {.CreateAudioPlayer = engine_CreateAudioPlayer,
                                   .CreateOutputMix = engine_CreateOutputMix};

SLresult slCreateEngine_bridge(void *out, SLuint32 n, const void *opts, SLuint32 ni, const void *ids,
                               const void *req) {
  sl_object *o = calloc(1, sizeof(sl_object));
  o->obj_itf = &object_vtbl;
  o->engine_itf = &engine_vtbl;
  o->type = OBJ_ENGINE;
  *(void **)out = &o->obj_itf;
  L_INFO("opensl: engine created");
  return SL_RESULT_SUCCESS;
}

// ============================================================================
// FMOD Java audio device emulation
// ============================================================================

#define FMOD_INFO_SAMPLERATE 0
#define FMOD_INFO_DSPBUFFERLENGTH 1
#define FMOD_INFO_DSPNUMBUFFERS 2
#define FMOD_INFO_MIXERRUNNING 3


static SceUID fmod_thread_id = -1;
static volatile int fmod_quit;

static int fmod_thread(SceSize args, void *argp) {
  int (*getInfo)(void *, void *, int) = jni_find_native("org/fmod/FMODAudioDevice", "fmodGetInfo");
  int (*process)(void *, void *, void *) = jni_find_native("org/fmod/FMODAudioDevice", "fmodProcess");
  if (!getInfo || !process) {
    L_ERROR("fmod: JNI entry points not found");
    return sceKernelExitDeleteThread(0);
  }
  void *env = jni_get_env();

  // getInfo returns -1 until Unity has created the FMOD system, which happens
  // during the first frames of nativeRender.
  int rate = -1, block = -1;
  for (int waited = 0; !fmod_quit; waited++) {
    rate = getInfo(env, NULL, FMOD_INFO_SAMPLERATE);
    block = getInfo(env, NULL, FMOD_INFO_DSPBUFFERLENGTH);
    if (rate > 0 && block > 0 && getInfo(env, NULL, FMOD_INFO_MIXERRUNNING) > 0)
      break;
    if (waited % 6000 == 5999)  // once a minute
      L_INFO("fmod: still waiting for the mixer (rate %d, block %d)", rate, block);
    sceKernelDelayThread(10000);
  }
  if (fmod_quit)
    return sceKernelExitDeleteThread(0);
  L_INFO("fmod: mixer running, %d Hz, block %d frames", rate, block);
  int grain = (block + 63) & ~63;

  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, vita_rate(rate), SCE_AUDIO_OUT_MODE_STEREO);
  if (port < 0) {
    L_ERROR("fmod: sceAudioOutOpenPort failed 0x%08X", port);
    return sceKernelExitDeleteThread(0);
  }
  int16_t *pcm = memalign(64, grain * 2 * sizeof(int16_t));
  memset(pcm, 0, grain * 2 * sizeof(int16_t));
  void *bytebuf = jni_new_direct_buffer(pcm, block * 2 * sizeof(int16_t));

  while (!fmod_quit) {
    if (getInfo(env, NULL, FMOD_INFO_MIXERRUNNING) > 0) {
      process(env, NULL, bytebuf);
    } else {
      memset(pcm, 0, grain * 2 * sizeof(int16_t));
    }
    sceAudioOutOutput(port, pcm);
  }
  sceAudioOutReleasePort(port);
  free(pcm);
  return sceKernelExitDeleteThread(0);
}

void audio_fmod_start(void) {
  if (fmod_thread_id >= 0)
    return;
  fmod_quit = 0;
  fmod_thread_id = sceKernelCreateThread("fmod_device", fmod_thread, 0x10000100 - 10, 0x40000, 0, thread_cpu_mask(), NULL);
  sceKernelStartThread(fmod_thread_id, 0, NULL);
}

void audio_fmod_stop(void) {
  fmod_quit = 1;
  fmod_thread_id = -1;
}

void audio_init(void) {
  sceKernelCreateLwMutex(&players_lock, "sl_players", 0, 0, NULL);
}

void audio_diagnostics(void) {
  // This is CRI's own error buffer, which its Unity component can leave
  // unreported. It is a read-only query and does not clear the error.
  const char *(*first_error)(void) = (void *)so_symbol_global("criWareUnity_GetFirstError");
  if (first_error) {
    const char *error = first_error();
    if (error && *error) L_WARN("cri: first error: %.700s", error);
  }
  sceKernelLockLwMutex(&players_lock, 1, NULL);
  for (sl_object *o = players; o; o = o->next_player) {
    sceKernelLockLwMutex(&o->bq_lock, 1, NULL);
    L_INFO("audio: port %d state %u queue %d/%d enqueued %u consumed %u audible %d callback %d", o->port,
      o->play_state, o->bq_count, o->bq_capacity, o->enqueued, o->buffers_seen, o->heard_pcm, o->cb != NULL);
    L_INFO("audio: port %d PCM window peak %d nonzero buffers %u clipped %llu/%llu output %d Hz", o->port,
      o->pcm_peak, o->pcm_nonzero, (unsigned long long)o->pcm_clipped,
      (unsigned long long)o->pcm_samples, o->output_rate);
    o->pcm_peak = 0;
    o->pcm_nonzero = 0;
    o->pcm_samples = o->pcm_clipped = 0;
    sceKernelUnlockLwMutex(&o->bq_lock, 1);
  }
  sceKernelUnlockLwMutex(&players_lock, 1);
}
