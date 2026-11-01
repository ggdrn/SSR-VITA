// NGUI's no-normal append path, expressed as three bounded bulk copies.
// Preflight every array before writing; caller runs the original on failure.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
typedef struct { uint8_t *data; int32_t used, capacity; } ui_copy_list;
static int ui_copy_geometry(ui_copy_list src[3], ui_copy_list dst[3], int32_t n) {
  static const unsigned stride[3] = {12, 8, 4};
  if (n <= 0) return n == 0;
  for (unsigned i = 0; i < 3; i++) {
    if (!src[i].data || !dst[i].data || src[i].capacity < n || dst[i].used < 0 ||
        dst[i].used > INT_MAX - n || dst[i].capacity < dst[i].used + n) return 0;
    if ((uint64_t)n * stride[i] > SIZE_MAX ||
        (uint64_t)(dst[i].used + n) * stride[i] > SIZE_MAX) return 0;
    for (unsigned j = 0; j < 3; j++)
      if (src[i].data == dst[j].data || (i != j && dst[i].data == dst[j].data)) return 0;
  }
  for (unsigned i = 0; i < 3; i++) {
    memcpy(dst[i].data + (size_t)dst[i].used * stride[i], src[i].data, (size_t)n * stride[i]);
    dst[i].used += n;
  }
  return 1;
}
