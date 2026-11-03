// Cooperatively split each XML layer without changing its object parser.
// Install the paired IL edits together before gameplay methods are JITted.
#include "stage_parse.h"
#include "stage_parse_il.h"
#include "../loader/so_util.h"
#include "../config.h"
#include "log.h"
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static int attempted;
void stage_parse_install(void *image) {
  if (!image || attempted) return;
  attempted = 1;
  FILE *marker = fopen(DATA_PATH "/original_stage_parse", "rb");
  if (marker) { fclose(marker); L_INFO("stage: original XML layer parsing requested"); return; }
  so_module *mono = so_find_module("libmono.so");
  if (!mono) return;
#define API(type, name) ((type)so_symbol(mono, name))
  const char *(*guid)(void *) = API(const char *(*)(void *), "mono_image_get_guid");
  void *(*method)(void *, uint32_t, void *) = API(void *(*)(void *, uint32_t, void *), "mono_get_method");
  void *(*header)(void *) = API(void *(*)(void *), "mono_method_get_header");
  const uint8_t *(*code)(void *, uint32_t *, uint32_t *) = API(const uint8_t *(*)(void *, uint32_t *, uint32_t *), "mono_method_header_get_code");
  void (*free_header)(void *) = API(void (*)(void *), "mono_metadata_free_mh");
  const char *id = guid ? guid(image) : NULL;
  if (!id || strcasecmp(id, STAGE_PARSE_MVID) || !method || !header || !code || !free_header) {
    L_WARN("stage: XML parser assembly/API differs; original parsing preserved"); return;
  }
  struct { uint32_t token; const uint8_t *expected; uint32_t size; void *header; const uint8_t *code; } targets[] = {
    {0x06003c18, stage_objects_il, sizeof(stage_objects_il), NULL, NULL},
    {0x06004c80, stage_layer_il, sizeof(stage_layer_il), NULL, NULL}
  };
  int valid = 1;
  for (unsigned i = 0; i < 2; i++) {
    void *m = method(image, targets[i].token, NULL);
    targets[i].header = m ? header(m) : NULL;
    uint32_t size = 0, stack = 0;
    targets[i].code = targets[i].header ? code(targets[i].header, &size, &stack) : NULL;
    if (!targets[i].code || size != targets[i].size || stack < 8 ||
        memcmp(targets[i].code, targets[i].expected, size)) valid = 0;
  }
  // No writes unless BOTH bodies match. The original ReadObjects loop goes
  // back to offset zero and resets its locals each time. Removing just that
  // tail lets the coroutine invoke the same body up to 16 times per step.
  if (valid) {
    static const uint8_t nops[STAGE_PARSE_LOOP_SIZE] = {0};
    kuKernelCpuUnrestrictedMemcpy((void *)(targets[0].code + STAGE_PARSE_LOOP_AT), nops, sizeof(nops));
    kuKernelCpuUnrestrictedMemcpy((void *)targets[1].code, stage_layer_batched_il, sizeof(stage_layer_batched_il));
    kuKernelFlushCaches((void *)(targets[0].code + STAGE_PARSE_LOOP_AT), sizeof(nops));
    kuKernelFlushCaches((void *)targets[1].code, sizeof(stage_layer_batched_il));
    L_INFO("stage: XML layer parsing yields every %u objects (complete layers published only; future JIT)", STAGE_PARSE_BATCH);
  } else L_WARN("stage: XML parser IL/stack differs; both original methods preserved");
  for (unsigned i = 0; i < 2; i++) if (targets[i].header) free_header(targets[i].header);
#undef API
}
