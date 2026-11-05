#pragma once
void rijndael256_install(void);
unsigned rijndael256_fast_calls(void);
unsigned rijndael256_fallback_calls(void);
// Returns zero without mutation when inputs require the original CBC path.
int rijndael256_try_cbc(void *self, void *input, void *output, void *iv, void *scratch, void *keys);
int rijndael256_ready(void);
