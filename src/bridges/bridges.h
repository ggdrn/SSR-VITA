#ifndef SRR_BRIDGES_H
#define SRR_BRIDGES_H

#include <stdint.h>
#include <stddef.h>

// Android armeabi-v7a code uses the soft-float calling convention while the
// VitaSDK build is hard-float: every bridged function with float/double
// arguments or return values must carry this attribute.
#define SOFTFP __attribute__((pcs("aapcs")))

// libc_bridge.c
// Fills `out` from the Vita's hardware RNG (any length).
void vita_random(void *out, size_t len);

// fs_bridge.c
// Maps an Android path to its ux0 location. Returns `path` untouched when no
// mapping applies, otherwise writes into `out`.
const char *fs_translate(const char *path, char *out, size_t out_size);
void fs_init(const char *apk_ux0_path);
const char *fs_apk_android_path(void);

// proc_bridge.c
// Generates the requested /proc file and returns its ux0 path, or NULL.
const char *proc_translate(const char *path, char *out, size_t out_size);
// Must run on the thread that later initialises Mono (its stack is "[stack]").
void proc_init(void);

// mem_bridge.c
void mem_init(void);

// signal_bridge.c
void signal_init(void);
// Runs signals queued for the calling thread by pthread_kill(); called from
// every blocking bridge. Returns how many handlers ran.
int signal_poll(void);
int signal_pending_self(void);

// input_bridge.c
void input_init(void);
void input_poll(void);

// audio_bridge.c
void audio_init(void);

// format_bridge.c
// Rewrites C99 length modifiers (z, t, j) that newlib's printf/scanf lack.
const char *fmt_c99(const char *fmt, char *buf, size_t size);
void log_heap_usage(const char *when);

// watchdog.c
#define WAIT_BEGIN(where, obj) thread_wait_begin(where, obj, (uintptr_t)__builtin_return_address(0))
void thread_wait_begin(const char *where, const void *obj, uintptr_t caller);
void thread_wait_end(void);
void watchdog_start(void);
extern volatile unsigned watchdog_frames;

// Errno conversion between newlib and bionic (Linux ARM numbering).
int errno_to_bionic(int e);

#endif
