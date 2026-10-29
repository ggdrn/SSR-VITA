#ifndef SRR_NATIVE_HEAP_H
#define SRR_NATIVE_HEAP_H
#include <stdint.h>
typedef struct { uint64_t chunks_free, unused_reserve, total_free; } native_heap_sample;
native_heap_sample native_heap_available(void);
#endif
