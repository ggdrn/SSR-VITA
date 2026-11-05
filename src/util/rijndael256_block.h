// Equivalent to the supported Mono Decrypt256 block operation. Shared by
// block and CBC hooks; key expansion, padding and text decoding stay managed.
#pragma once
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t inverse[4][256]; uint8_t sbox[256]; } rijndael256_tables;
static void rijndael256_block(const uint8_t *input, uint8_t *output, const uint32_t *keys,
                              const rijndael256_tables *t) {
  uint32_t state[8], next[8];
  for (unsigned c = 0; c < 8; c++)
    state[c] = ((uint32_t)input[c*4] << 24 | (uint32_t)input[c*4+1] << 16 |
                (uint32_t)input[c*4+2] << 8 | input[c*4+3]) ^ keys[c];
  for (unsigned round = 1; round < 14; round++) {
    for (unsigned c = 0; c < 8; c++)
      next[c] = t->inverse[0][state[c] >> 24] ^
                t->inverse[1][(state[(c+7)&7] >> 16) & 255] ^
                t->inverse[2][(state[(c+5)&7] >> 8) & 255] ^
                t->inverse[3][state[(c+4)&7] & 255] ^ keys[round*8+c];
    memcpy(state, next, sizeof(state));
  }
  // Read all input before writing, preserving the original in-place behavior.
  for (unsigned c = 0; c < 8; c++) {
    output[c*4]   = t->sbox[state[c] >> 24] ^ (keys[112+c] >> 24);
    output[c*4+1] = t->sbox[(state[(c+7)&7] >> 16) & 255] ^ (keys[112+c] >> 16);
    output[c*4+2] = t->sbox[(state[(c+5)&7] >> 8) & 255] ^ (keys[112+c] >> 8);
    output[c*4+3] = t->sbox[state[(c+4)&7] & 255] ^ keys[112+c];
  }
}

// One decrypting CBC block. Caller validates disjoint IV/scratch buffers.
// Save ciphertext first so input and output may be the same array. Keep both
// managed state buffers byte-identical to SymmetricTransform.CBC.
static void rijndael256_cbc_block(const uint8_t *input, uint8_t *output,
                                 uint8_t *iv, uint8_t *scratch,
                                 const uint32_t *keys, const rijndael256_tables *t) {
  memcpy(scratch, input, 32);
  rijndael256_block(input, output, keys, t);
  for (unsigned i = 0; i < 32; i++) output[i] ^= iv[i];
  memcpy(iv, scratch, 32);
}
