#ifndef SRR_BUNDLE_MIPS_H
#define SRR_BUNDLE_MIPS_H
#include <stddef.h>
#include <stdio.h>
// Unity drops mip 0 of every mipmapped texture while masterTextureLimit >= 1,
// yet still reads it from the bundle. For cached .sharedAssets streams, the
// byte ranges of those top mips (non-readable, uncompressed, >= 64 KiB) are
// located once from the serialized file and not read from the card.
void bundle_mips_open(FILE *f, const char *path);
void bundle_mips_close(FILE *f);
// Returns 1 and stores the fread result when the read crossed a skipped mip.
int bundle_mips_fread(void *p, size_t size, size_t count, FILE *f, size_t *result);
// Unity's current masterTextureLimit; only >= 1 enables skipping.
void bundle_mips_set_limit(int limit);
#endif
