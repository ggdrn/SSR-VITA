// Fixed-memory, nonblocking producers. Only the background consumer logs.
#include "observe.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#define QUEUE 512
#define ASSETS 1024
#define NAME 128
#define BINS 2002
static const char *kinds[] = {"boot","file","apk","resource","clone","image","unload","shader","texture","network","input","gc","bundle","scene"};
typedef struct {
  uint64_t at, duration, bytes, total, maximum;
  uint32_t frame, phase, uid, hash, count, object;
  int status;
  unsigned kind, summary;
  char name[NAME];
} event;
typedef struct { event value; unsigned dirty; } asset;
static event queue[QUEUE];
static asset assets[ASSETS];
static unsigned head, tail, gate, dropped, overflow, frame, phase, enabled, running;
static uint64_t origin;
static char scene[NAME];
static int loading = -1;
static unsigned scan;
static uint64_t writer_us, longest_write;
// Frame histograms are owned exclusively by the render thread.
static uint32_t histogram[BINS], frames, slow50, slow100, gcs;
static uint64_t frame_start, render_total, frame_total, frame_max;
uint64_t observe_now(void) { return sceKernelGetProcessTimeWide(); }
static int lock(void) { return !__atomic_exchange_n(&gate, 1, __ATOMIC_ACQUIRE); }
static void unlock(void) { __atomic_store_n(&gate, 0, __ATOMIC_RELEASE); }
static unsigned hash_name(const char *s) {
  unsigned hash = 2166136261u;
  for (unsigned i = 0; s && s[i] && i < 2048; i++) hash = (hash ^ (unsigned char)s[i]) * 16777619u;
  hash &= 0x3fffffffu; // Avoid conservative-GC-looking heap addresses in telemetry.
  return hash ? hash : 1;
}
static void name_copy(char *out, const char *in) {
  unsigned i = 0;
  // ASCII asset identifiers are preserved; control characters are sanitized.
  // Hashes distinguish full names even when their display label is truncated.
  for (; in && in[i] && i < NAME - 1; i++) {
    unsigned char c = in[i];
    out[i] = c >= 32 && c < 127 ? c : '?';
  }
  out[i] = 0;
}
static void push(event *e) {
  if (head - tail == QUEUE) { __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED); return; }
  queue[head++ % QUEUE] = *e;
}
static struct { uintptr_t handle; char name[NAME]; } files[128];
static unsigned files_unreliable;
void observe_file_register(uintptr_t handle, const char *path) {
  if (!handle) return;
  if (!lock()) { __atomic_store_n(&files_unreliable, 1, __ATOMIC_RELEASE); return; }
  if (__atomic_exchange_n(&files_unreliable, 0, __ATOMIC_ACQ_REL)) memset(files, 0, sizeof(files));
  unsigned slot = ((unsigned)(handle >> 2)) % 128, target = slot, empty = 128;
  for (unsigned n = 0; n < 8; n++, slot = (slot + 1) % 128) {
    if (files[slot].handle == handle) files[slot].handle = 0;
    if (!files[slot].handle && empty == 128) empty = slot;
  }
  if (empty < 128) target = empty;
  files[target].handle = handle; name_copy(files[target].name, path);
  unlock();
}
void observe_file_label(uintptr_t handle, char *out, unsigned cap) {
  snprintf(out, cap, "handle=%p", (void *)handle);
  if (__atomic_load_n(&files_unreliable, __ATOMIC_ACQUIRE) || !lock()) return;
  unsigned slot = ((unsigned)(handle >> 2)) % 128;
  for (unsigned n = 0; n < 8; n++, slot = (slot + 1) % 128) {
    if (files[slot].handle == handle && handle) { snprintf(out, cap, "%s", files[slot].name); break; }
  }
  unlock();
}
void observe_file_forget(uintptr_t handle) {
  if (!lock()) { __atomic_store_n(&files_unreliable, 1, __ATOMIC_RELEASE); return; }
  unsigned slot = ((unsigned)(handle >> 2)) % 128;
  for (unsigned n = 0; n < 8; n++, slot = (slot + 1) % 128) {
    if (files[slot].handle == handle) files[slot].handle = 0;
  }
  unlock();
}
void observe_init(void) {
  origin = observe_now();
  __atomic_store_n(&enabled, 1, __ATOMIC_RELEASE);
  observe_mark("process.start");
}
void observe_begin_frame(unsigned n) { __atomic_store_n(&frame, n, __ATOMIC_RELAXED); }
void observe_span_at(unsigned kind, const char *name, uint64_t start, uint64_t now, uint64_t bytes, int status, uintptr_t object) {
  if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || kind >= OBS_KIND_COUNT) return;
  event e = {.at = start >= origin ? start - origin : 0, .duration = now >= start ? now - start : 0,
    .bytes = bytes, .kind = kind, .status = status, .object = (uint32_t)object & 0x7fffffffu,
    .frame = __atomic_load_n(&frame, __ATOMIC_RELAXED), .phase = __atomic_load_n(&phase, __ATOMIC_RELAXED),
    .uid = sceKernelGetThreadId(), .hash = hash_name(name)};
  name_copy(e.name, name);
  if (!lock()) { __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED); return; }
  int first = 1;
  if (kind != OBS_BOOT && kind != OBS_INPUT && kind != OBS_GC) {
    unsigned i = (e.hash ^ kind ^ (e.phase * 31)) % ASSETS;
    asset *a = NULL;
    for (unsigned n = 0; n < 16; n++, i = (i + 1) % ASSETS) {
      event *v = &assets[i].value;
      if ((v->count && v->phase != e.phase && !assets[i].dirty)) memset(v, 0, sizeof(*v));
      if (!v->count || (v->hash == e.hash && v->kind == kind && v->phase == e.phase)) { a = &assets[i]; break; }
    }
    if (a) {
      first = !a->value.count;
      if (first) { a->value = e; a->value.bytes = a->value.total = a->value.maximum = 0; }
      a->value.count++;
      a->value.bytes += bytes;
      a->value.total += e.duration;
      if (e.duration > a->value.maximum) a->value.maximum = e.duration;
      a->dirty = 1;
    } else __atomic_fetch_add(&overflow, 1, __ATOMIC_RELAXED);
  }
  // All first-seen named operations; repeat details only when >= 2 ms.
  // Repeated tiny operations remain in per-name cumulative aggregates.
  if (first || e.duration >= 50000 || kind == OBS_BOOT || kind == OBS_INPUT || kind == OBS_GC) push(&e);
  unlock();
}
void observe_span(unsigned kind, const char *name, uint64_t start, uint64_t bytes, int status, uintptr_t object) {
  observe_span_at(kind, name, start, observe_now(), bytes, status, object);
}
void observe_mark(const char *name) { observe_span(OBS_BOOT, name, observe_now(), 0, 0, 0); }
static unsigned percentile(unsigned percent) {
  unsigned target = (unsigned)(((uint64_t)frames * percent + 99) / 100), n = 0;
  for (unsigned i = 0; i < BINS; i++) { n += histogram[i]; if (n >= target) return i; }
  return BINS - 1;
}
static void frame_flush(void) {
  if (!frames) return;
  char label[NAME];
  snprintf(label, sizeof(label), "frames=%u p50_ms=%u p95_ms=%u p99_ms=%u slow50=%u slow100=%u gc=%u", frames,
           percentile(50), percentile(95), percentile(99), slow50, slow100, gcs);
  event e = {.kind = OBS_BOOT, .summary = 2, .at = frame_start >= origin ? frame_start - origin : 0,
    .duration = frame_total, .total = render_total, .maximum = frame_max, .count = frames,
    .frame = __atomic_load_n(&frame, __ATOMIC_RELAXED), .phase = __atomic_load_n(&phase, __ATOMIC_RELAXED)};
  name_copy(e.name, label);
  if (lock()) { push(&e); unlock(); } else __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
  memset(histogram, 0, sizeof(histogram));
  frames = slow50 = slow100 = gcs = 0;
  frame_total = render_total = frame_max = frame_start = 0;
}
void observe_frame(uint64_t render_us, uint64_t frame_us, unsigned collections) {
  uint64_t now = observe_now();
  if (!frames) frame_start = now >= frame_us ? now - frame_us : 0;
  unsigned bin = (unsigned)(frame_us / 1000 + !!(frame_us % 1000));
  if (bin >= BINS) bin = BINS - 1;
  histogram[bin]++; frames++; gcs += collections;
  slow50 += frame_us > 50000; slow100 += frame_us > 100000;
  frame_total += frame_us; render_total += render_us;
  if (frame_us > frame_max) frame_max = frame_us;
  if (now - frame_start >= 5000000) frame_flush();
}
void observe_phase(const char *name, int is_loading) {
  // Main thread only; this is an observed state change, not an exact scene load request.
  char label[NAME]; name_copy(label, name);
  if (!strcmp(scene, label) && loading == is_loading) return;
  frame_flush();
  name_copy(scene, label); loading = is_loading;
  __atomic_fetch_add(&phase, 1, __ATOMIC_RELAXED);
  char marker[NAME]; snprintf(marker, sizeof(marker), "scene=%.95s loading=%d", scene, loading);
  observe_mark(marker);
}
static void escape(char out[NAME * 2], const char *in) {
  unsigned j = 0;
  for (unsigned i = 0; in[i] && i < NAME - 1; i++) {
    if (in[i] == '"' || in[i] == '\\') out[j++] = '\\';
    out[j++] = in[i];
  }
  out[j] = 0;
}
static void emit(event *e) {
  char label[NAME * 2]; escape(label, e->name);
  uint64_t write_start = observe_now();
  L_INFO("OBS {\"type\":\"%s\",\"kind\":\"%s\",\"start_us\":%llu,\"duration_us\":%llu,\"frame\":%u,\"phase\":%u,\"thread\":%u,\"key\":%u,\"object\":%u,\"bytes\":%llu,\"status\":%d,\"count\":%u,\"total_us\":%llu,\"max_us\":%llu,\"name\":\"%s\"}",
    e->summary == 2 ? "frames" : e->summary ? "asset" : "span", kinds[e->kind],
    (unsigned long long)e->at, (unsigned long long)e->duration, e->frame, e->phase, e->summary == 1 || e->kind == OBS_GC ? 0 : e->uid, e->hash,
    e->object, (unsigned long long)e->bytes, e->status, e->count,
    (unsigned long long)e->total, (unsigned long long)e->maximum, label);
  uint64_t spent = observe_now() - write_start;
  writer_us += spent;
  if (spent > longest_write) longest_write = spent;
}
// Copy under the short gate, perform all formatting/file writes after release.
static unsigned drain(unsigned limit) {
  unsigned n = 0;
  while (n < limit) {
    event e; int have = 0;
    if (lock()) { if (tail != head) { e = queue[tail++ % QUEUE]; have = 1; } unlock(); }
    if (!have) break;
    emit(&e); n++;
  }
  return n;
}
static void aggregates(unsigned count) {
  // Incremental scan: at most four aggregate log lines per consumer tick.
  unsigned emitted = 0;
  for (unsigned n = 0; n < count; n++, scan = (scan + 1) % ASSETS) {
    event e; int have = 0;
    if (lock()) {
      asset *a = &assets[scan];
      if (a->dirty) { e = a->value; e.summary = 1; a->dirty = 0; have = 1; }
      unlock();
    }
    if (have && (e.total >= 50000 || e.count >= 128 || e.maximum >= 50000)) { emit(&e); if (++emitted == 4) { scan = (scan + 1) % ASSETS; break; } }
  }
}
static int writer(unsigned args, void *argp) {
  unsigned ticks = 0;
  while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
    drain(16);
    aggregates(64);
    ++ticks;
    log_flush_pending(32768);
    if (ticks % 50 == 0) {
      unsigned pending = 0;
      if (lock()) { pending = head - tail; unlock(); }
      L_INFO("OBS health dropped=%u aggregate_overflow=%u queue=%u writer_us=%llu max_write_us=%llu log_dropped=%u storage_ms=%u", __atomic_load_n(&dropped, __ATOMIC_RELAXED), __atomic_load_n(&overflow, __ATOMIC_RELAXED), pending, (unsigned long long)writer_us, (unsigned long long)longest_write, log_dropped_records(), log_storage_ms());
    }
    sceKernelDelayThread(100000);
  }
  drain(QUEUE); for (unsigned i = 0; i < ASSETS / 4; i++) { aggregates(ASSETS); log_flush_pending(32768); }
  L_INFO("OBS health dropped=%u aggregate_overflow=%u queue=0 writer_us=%llu max_write_us=%llu log_dropped=%u storage_ms=%u", __atomic_load_n(&dropped, __ATOMIC_RELAXED), __atomic_load_n(&overflow, __ATOMIC_RELAXED), (unsigned long long)writer_us, (unsigned long long)longest_write, log_dropped_records(), log_storage_ms());
  log_flush_pending(131072);
  __atomic_store_n(&running, 2, __ATOMIC_RELEASE);
  return 0;
}
void observe_start_writer(void) {
  if (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) return;
  // Use the priority already accepted for the port's networking threads.
  // 0x10000120 is outside the allowed range on the user's Vita (0x80028023).
  int priority = 0x10000100 + 10;
  int tid = sceKernelCreateThread("srr_observe", writer, priority, 0x10000, 0, 0, NULL);
  if (tid < 0) {
    L_WARN("OBS writer priority %08X rejected %08X; using watchdog priority", priority, tid);
    priority = 0x10000100 - 20;
    tid = sceKernelCreateThread("srr_observe", writer, priority, 0x10000, 0, 0, NULL);
  }
  if (tid < 0) { L_WARN("OBS writer unavailable %08X", tid); return; }
  __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
  if (sceKernelStartThread(tid, 0, NULL) < 0) {
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE); sceKernelDeleteThread(tid);
    L_WARN("OBS writer failed to start");
  } else {
    log_enable_buffered();
    L_INFO("OBS writer ready uid=%08X priority=%08X queue=%u assets=%u buffered_log=1", tid, priority, QUEUE, ASSETS);
  }
}
void observe_stop(void) {
  frame_flush(); observe_mark("process.stop");
  __atomic_store_n(&enabled, 0, __ATOMIC_RELEASE);
  if (__atomic_load_n(&running, __ATOMIC_ACQUIRE) == 1) {
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 50 && __atomic_load_n(&running, __ATOMIC_ACQUIRE) != 2; i++) sceKernelDelayThread(10000);
  }
}
