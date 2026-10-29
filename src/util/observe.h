#ifndef SRR_OBSERVE_H
#define SRR_OBSERVE_H
#include <stdint.h>
enum { OBS_BOOT, OBS_FILE, OBS_APK, OBS_RESOURCE, OBS_CLONE, OBS_IMAGE,
       OBS_UNLOAD, OBS_SHADER, OBS_TEXTURE, OBS_NETWORK, OBS_INPUT, OBS_GC, OBS_BUNDLE, OBS_SCENE, OBS_KIND_COUNT };
void observe_init(void);
void observe_start_writer(void);
uint64_t observe_now(void);
void observe_span(unsigned kind, const char *name, uint64_t start, uint64_t bytes, int status, uintptr_t object);
void observe_span_at(unsigned kind, const char *name, uint64_t start, uint64_t end, uint64_t bytes, int status, uintptr_t object);
void observe_mark(const char *name);
void observe_phase(const char *scene, int loading);
void observe_begin_frame(unsigned frame);
void observe_frame(uint64_t render_us, uint64_t frame_us, unsigned collections);
void observe_file_register(uintptr_t handle, const char *path);
void observe_file_label(uintptr_t handle, char *out, unsigned cap);
void observe_file_forget(uintptr_t handle);
void observe_stop(void);
#endif
