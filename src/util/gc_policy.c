#include "gc_policy.h"
#include <stddef.h>
#define MIB (1024ULL * 1024)
static uint32_t *setting;
static unsigned desired = 3, applied = 3;
static int conflict;

int gc_policy_attach(uint32_t *divisor) {
  if (!divisor || __atomic_load_n(divisor, __ATOMIC_RELAXED) != 3) return 0;
  __atomic_store_n(&setting, divisor, __ATOMIC_RELEASE);
  return 1;
}
int gc_policy_bound(void) { return __atomic_load_n(&setting, __ATOMIC_ACQUIRE) != NULL; }
unsigned gc_policy_desired(void) { return __atomic_load_n(&desired, __ATOMIC_ACQUIRE); }
unsigned gc_policy_applied(void) { return __atomic_load_n(&applied, __ATOMIC_ACQUIRE); }
int gc_policy_conflict(void) { return __atomic_load_n(&conflict, __ATOMIC_ACQUIRE); }

unsigned gc_policy_request(int playing, int loading, uint64_t heap, uint64_t used, uint64_t native_free) {
  unsigned next = gc_policy_desired();
  uint64_t spare = heap >= used ? heap - used : 0;
  // The measured stage working set is ~86 MiB. A 64 MiB ceiling never
  // activated, even with free native reserve and ~30 MiB spare in that heap.
  // Scale managed headroom with heap size; keep native pressure hysteresis.
  uint64_t enter_spare = heap / 4 > 12 * MIB ? heap / 4 : 12 * MIB;
  uint64_t exit_spare = heap / 8 > 8 * MIB ? heap / 8 : 8 * MIB;
  if (!gc_policy_bound() || gc_policy_conflict() || !playing || loading || heap > 96 * MIB ||
      native_free < 32 * MIB || spare < exit_spare) next = 3;
  else if (native_free >= 48 * MIB && spare >= enter_spare) next = 2;
  __atomic_store_n(&desired, next, __ATOMIC_RELEASE);
  return next;
}

void gc_policy_event(int event) {
  if (event != 0 || gc_policy_conflict()) return;
  uint32_t *divisor = __atomic_load_n(&setting, __ATOMIC_ACQUIRE);
  if (!divisor) return;
  // The collector's own lock synchronizes this ordinary configuration field
  // with its readers. Main-thread policy requests never write the field.
  unsigned current = *divisor;
  if (current != gc_policy_applied()) {
    __atomic_store_n(&applied, current, __ATOMIC_RELEASE);
    __atomic_store_n(&conflict, 1, __ATOMIC_RELEASE);
    return; // Respect changes made by another engine component.
  }
  unsigned next = gc_policy_desired();
  *divisor = next;
  __atomic_store_n(&applied, next, __ATOMIC_RELEASE);
}

// Collection pacing. Boehm 7.1 starts an automatic collection once the words
// allocated since the last one reach min_words_allocd(), a fraction of the
// heap. Each stop-the-world collection marks the whole live set (~360 ms in a
// race, ~650-1000 ms in menus), so the pause length cannot be shortened from
// here; the number of pauses can. While the native heap has room, a cycle may
// allocate up to the latched budget before collecting. Explicit collections
// (GC.Collect, UnloadUnusedAssets) are not affected.
static const uint32_t *gc_arrays, *gc_root_size, *gc_incremental, *gc_time_limit;
static uint32_t budget_words;
static int pacing_on;

void gc_policy_attach_pacing(const uint32_t *arrays, const uint32_t *root_size, const uint32_t *incremental,
                             const uint32_t *time_limit) {
  gc_arrays = arrays;
  gc_root_size = root_size;
  gc_incremental = incremental;
  gc_time_limit = time_limit;
}

// Original formula, word for word (GC_arrays[0] heapsize, [5] large free).
uint32_t gc_policy_base_words(void) {
  uint32_t heap = gc_arrays[0], large_free = gc_arrays[5];
  uint32_t scan = (heap - large_free + (large_free >> 2) + 2 * 10000 + *gc_root_size) >> 2;
  uint32_t divisor = *(volatile uint32_t *)setting;
  if (*gc_incremental && *gc_time_limit != 999999) divisor <<= 1;
  return divisor ? scan / divisor : scan;
}

// Replaces min_words_allocd; called by the collector under its own lock.
uint32_t gc_policy_min_words(void) {
  uint32_t result = gc_policy_base_words();
  uint32_t budget = __atomic_load_n(&budget_words, __ATOMIC_RELAXED);
  if (budget > result && gc_arrays[0] < 96 * MIB) return budget;
  return result;
}

int gc_policy_pacing_bound(void) { return gc_arrays != NULL; }
uint32_t gc_policy_pacing_heap(void) { return gc_arrays ? gc_arrays[0] : 0; }
uint32_t gc_policy_pacing_large_free(void) { return gc_arrays ? gc_arrays[5] : 0; }

// Main thread, every ~0.5 s. The budget is latched once per collection cycle:
// recomputing it from the shrinking free space would end the cycle early.
// Safety conditions withdraw it at any time; the next block allocation then
// collects as before.
unsigned gc_policy_pacing_tick(int collections, uint64_t heap, uint64_t used, uint64_t native_free) {
  static int latched_cycle = -1;
  if (!gc_arrays) return 0;
  if (pacing_on && (native_free < 48 * MIB || heap > 96 * MIB)) pacing_on = 0;
  else if (!pacing_on && native_free >= 72 * MIB && heap <= 88 * MIB) pacing_on = 1;
  uint32_t current = __atomic_load_n(&budget_words, __ATOMIC_RELAXED);
  if (!pacing_on) {
    __atomic_store_n(&budget_words, 0, __ATOMIC_RELAXED);
    latched_cycle = -1;
    return 0;
  }
  if (current && collections == latched_cycle) return current;
  // Spare heap blocks first, then at most 24 MiB of new heap, keeping 64 MiB
  // of native memory for Unity's own assets.
  uint64_t spare = heap > used ? heap - used : 0;
  uint64_t grow = native_free > 64 * MIB ? native_free - 64 * MIB : 0;
  if (grow > 24 * MIB) grow = 24 * MIB;
  uint64_t bytes = spare + grow;
  if (bytes > 64 * MIB) bytes = 64 * MIB;
  __atomic_store_n(&budget_words, (uint32_t)(bytes >> 2), __ATOMIC_RELAXED);
  latched_cycle = collections;
  return (uint32_t)(bytes >> 2);
}
