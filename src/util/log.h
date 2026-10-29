#ifndef SRR_LOG_H
#define SRR_LOG_H

#include <stdarg.h>

void log_init(void);
void log_enable_buffered(void);
void log_flush_pending(unsigned budget);
void log_finish(void);
unsigned log_dropped_records(void);
unsigned log_storage_ms(void);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);

#define L_INFO(...)  log_printf("[INFO] " __VA_ARGS__)
#define L_WARN(...)  log_printf("[WARN] " __VA_ARGS__)
#define L_ERROR(...) log_printf("[ERROR] " __VA_ARGS__)

#ifdef DEBUG
#define L_DEBUG(...) log_printf("[DEBUG] " __VA_ARGS__)
#else
#define L_DEBUG(...) do {} while (0)
#endif

// Shows an error on screen and exits back to LiveArea.
void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif
