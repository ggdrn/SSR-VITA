#!/usr/bin/env python3
"""Exercise the actual OpenSL queue/consumer code with host mutex/audio stubs."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1] / 'src/bridges/audio_bridge.c').read_text()
harness = r'''
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
typedef int SceUID;
typedef size_t SceSize;
typedef unsigned SceUInt;
typedef pthread_mutex_t SceKernelLwMutexWork;
#define SCE_AUDIO_VOLUME_0DB 32768
#define SCE_AUDIO_VOLUME_FLAG_L_CH 1
#define SCE_AUDIO_VOLUME_FLAG_R_CH 2
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
#define L_ERROR(...) ((void)0)
static void *host_memalign(size_t align, size_t size) {
  void *p = NULL; return posix_memalign(&p, align, size) ? NULL : p;
}
#define memalign host_memalign
static _Thread_local int queue_lock_depth;
static void sceKernelLockLwMutex(pthread_mutex_t *m, int n, void *t) { assert(pthread_mutex_lock(m) == 0); queue_lock_depth++; }
static void sceKernelUnlockLwMutex(pthread_mutex_t *m, int n) { queue_lock_depth--; assert(pthread_mutex_unlock(m) == 0); }
static int sceKernelSignalSema(int id, int n) { return 0; }
static int sceKernelWaitSema(int id, int n, unsigned *timeout) { usleep(100); return 0; }
static int sceAudioOutSetVolume(int port, int mask, const int *v) { return 0; }
static void (*on_output)(void);
static int16_t captured[1024];
static unsigned outputs;
static const void *pending_pcm;
static int16_t pending_copy[1024];
static int sceAudioOutOutput(int port, const void *buf) {
  // The device can block for a full audio grain; producers must remain free.
  assert(queue_lock_depth == 0);
  // Simulate the device retaining the submitted pointer until the next call.
  if (pending_pcm) assert(memcmp(pending_pcm, pending_copy, sizeof(pending_copy)) == 0);
  if (!buf) { pending_pcm = NULL; return 0; }
  pending_pcm = buf;
  memcpy(pending_copy, buf, sizeof(pending_copy));
  memcpy(captured, buf, sizeof(captured)); outputs++;
  if (on_output) { void (*f)(void) = on_output; on_output = NULL; f(); }
  usleep(100); return 0;
}
'''
a=src.index('typedef uint32_t SLresult;')
b=src.index('static SLresult obj_Realize',a)
harness += src[a:b]
a=src.index('static SLresult bq_Enqueue(')
b=src.index('static const SLBufferQueueItf_ bq_vtbl',a)
harness += src[a:b]
a=src.index('static int mb_to_vita_volume(')
b=src.index('static SLresult engine_CreateAudioPlayer',a)
harness += src[a:b]
harness += r'''
static sl_object *clear_target;
static void clear_during_output(void) {
  bq_Clear(&clear_target->bq_itf);
  // A new queue must survive completion of the old staged buffer.
  static int16_t replacement[1024];
  assert(bq_Enqueue(&clear_target->bq_itf, replacement, sizeof(replacement)) == 0);
  clear_target->quit = 1;
}
static void stop_output(void) { clear_target->quit = 1; }
static void native_pcm_test(int channels) {
  sl_object o = {0};
  pthread_mutex_init(&o.bq_lock, NULL);
  o.channels = channels; o.bits = 16; o.rate = o.output_rate = 44100; o.port = 7;
  o.bq_capacity = 1; o.bq = calloc(1, sizeof(*o.bq));
  int16_t pcm[1024];
  for (int i = 0; i < 1024; i++) pcm[i] = (int16_t)(i * 127 - 32768);
  pcm[0] = -32768; pcm[1] = 32767;
  o.play_state = SL_PLAYSTATE_PLAYING;
  clear_target = &o; on_output = stop_output; outputs = 0;
  assert(bq_Enqueue(&o.bq_itf, pcm, 512 * channels * 2) == 0);
  player_thread(sizeof(clear_target), &clear_target);
  assert(outputs == 1);
  for (int f = 0; f < 512; f++) {
    assert(captured[2*f] == pcm[f*channels]);
    assert(captured[2*f+1] == pcm[f*channels+(channels == 2)]);
  }
  free(o.bq); pthread_mutex_destroy(&o.bq_lock);
}
static void *consume(void *p) { sl_object *o = p; player_thread(sizeof(o), &o); return NULL; }
int main(void) {
  native_pcm_test(2); native_pcm_test(1);
  sl_object o = {0};
  pthread_mutex_init(&o.bq_lock, NULL);
  o.channels = 2; o.bits = 16; o.rate = 44100; o.port = 7;
  o.bq_capacity = 64; o.bq = calloc(64, sizeof(*o.bq));
  int16_t pcm[1024]; for (int i = 0; i < 1024; ++i) pcm[i] = i;
  for (int i = 0; i < 64; ++i) assert(bq_Enqueue(&o.bq_itf, pcm, sizeof(pcm)) == 0);
  assert(bq_Enqueue(&o.bq_itf, pcm, sizeof(pcm)) == 7);
  uint32_t state[2]; bq_GetState(&o.bq_itf, state); assert(state[0] == 64);
  bq_Clear(&o.bq_itf); bq_GetState(&o.bq_itf, state); assert(state[0] == 0 && state[1] == 0);
  assert(bq_Enqueue(&o.bq_itf, NULL, 4) == SL_RESULT_PARAMETER_INVALID);
  assert(bq_Enqueue(&o.bq_itf, pcm, 3) == SL_RESULT_PARAMETER_INVALID);
  o.play_state = SL_PLAYSTATE_PLAYING;
  clear_target = &o;
  on_output = clear_during_output;
  assert(bq_Enqueue(&o.bq_itf, pcm, sizeof(pcm)) == 0);
  player_thread(sizeof(clear_target), &clear_target);
  bq_GetState(&o.bq_itf, state);
  assert(state[0] == 1 && state[1] == 0);
  bq_Clear(&o.bq_itf);
  o.quit = 0;
  pthread_t worker; pthread_create(&worker, NULL, consume, &o);
  for (int i = 0; i < 2000; ++i) {
    bq_Enqueue(&o.bq_itf, pcm, sizeof(pcm));
    if (i % 3 == 0) bq_Clear(&o.bq_itf);
    bq_GetState(&o.bq_itf, state); assert(state[0] <= 64);
    if (i % 20 == 0) usleep(150);
  }
  bq_Clear(&o.bq_itf);
  assert(bq_Enqueue(&o.bq_itf, pcm, sizeof(pcm)) == 0);
  for (int i = 0; i < 1000; ++i) {
    bq_GetState(&o.bq_itf, state);
    if (!state[0]) break;
    usleep(100);
  }
  bq_GetState(&o.bq_itf, state); assert(state[0] == 0);
  o.quit = 1; pthread_join(worker, NULL);
  assert(o.buffers_seen > 0 && o.heard_pcm);
  free(o.bq); pthread_mutex_destroy(&o.bq_lock);
  puts("PASS: 64 buffers, queue overflow, invalid PCM, concurrent clear/consume, clear during hardware output, resampling, bit-exact 44.1 kHz stereo/mono, queued PCM lifetime and drain");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-pthread', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
