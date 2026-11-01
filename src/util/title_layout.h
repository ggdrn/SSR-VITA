#ifndef SRR_TITLE_LAYOUT_H
#define SRR_TITLE_LAYOUT_H
#include "apk.h"
void title_layout_prepare(void);
void title_layout_apply(const apk_entry *entry, uint32_t pos, void *data, uint32_t size);
#endif
