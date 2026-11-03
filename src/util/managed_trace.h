#ifndef SRR_MANAGED_TRACE_H
#define SRR_MANAGED_TRACE_H
void managed_trace_install(void);
void managed_trace_focus(void *method);
// Registered once after exact IL/layout validation. The filter is allocation-free.
void managed_trace_idle_method(void *method, int (*idle)(void *));
void managed_trace_menu_methods(void *panel, void *cell, void *camera, void *init);
void managed_trace_menu_mode(int enabled);
void managed_trace_frame_begin(unsigned frame);
// Render thread: expand focused race timing for 120 frames after a >100ms render.
void managed_trace_frame_end(unsigned long long render_us);
unsigned managed_trace_idle_skipped(void);
unsigned managed_trace_idle_forwarded(void);
#endif
