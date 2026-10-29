#ifndef SRR_GC_POLICY_H
#define SRR_GC_POLICY_H
#include <stdint.h>
int gc_policy_attach(uint32_t *divisor);
int gc_policy_bound(void);
unsigned gc_policy_request(int playing, int loading, uint64_t heap, uint64_t used, uint64_t native_free);
// Called only from GC_EVENT_START, while Boehm holds its allocation lock.
void gc_policy_event(int event);
unsigned gc_policy_desired(void);
unsigned gc_policy_applied(void);
int gc_policy_conflict(void);

// Collection pacing (replacement for Boehm's min_words_allocd).
void gc_policy_attach_pacing(const uint32_t *arrays, const uint32_t *root_size, const uint32_t *incremental,
                             const uint32_t *time_limit);
uint32_t gc_policy_min_words(void);
uint32_t gc_policy_base_words(void);
int gc_policy_pacing_bound(void);
uint32_t gc_policy_pacing_heap(void);
uint32_t gc_policy_pacing_large_free(void);
// Returns the latched budget in words (0 = original policy).
unsigned gc_policy_pacing_tick(int collections, uint64_t heap, uint64_t used, uint64_t native_free);
#endif
