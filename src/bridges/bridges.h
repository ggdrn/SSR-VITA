#ifndef SRR_BRIDGES_H
#define SRR_BRIDGES_H

#include <stdint.h>
#include <stddef.h>

// Android armeabi-v7a code uses the soft-float calling convention while the
// VitaSDK build is hard-float: every bridged function with float/double
// arguments or return values must carry this attribute.
#define SOFTFP __attribute__((pcs("aapcs")))

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

// input_bridge.c
void input_init(void);
void input_poll(void);

// audio_bridge.c
void audio_init(void);

// format_bridge.c
// Rewrites C99 length modifiers (z, t, j) that newlib's printf/scanf lack.
const char *fmt_c99(const char *fmt, char *buf, size_t size);
void log_heap_usage(const char *when);

// Errno conversion between newlib and bionic (Linux ARM numbering).
int errno_to_bionic(int e);

#endif
