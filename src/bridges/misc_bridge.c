// Small leftovers: SHA-1 (bionic exports it from libc), libgcc helpers that
// bionic's libc re-exported, and neutral stubs for APIs the Vita can't offer.

#include "bridges.h"
#include "../util/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int *__errno_bionic(void);

// --- SHA-1 (layout of bionic's SHA1_CTX) ------------------------------------

typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  uint8_t buffer[64];
} SHA1_CTX;

#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

static void sha1_transform(uint32_t state[5], const uint8_t buf[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (buf[i * 4] << 24) | (buf[i * 4 + 1] << 16) | (buf[i * 4 + 2] << 8) | buf[i * 4 + 3];
  for (int i = 16; i < 80; i++)
    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
    else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
    else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
    else f = b ^ c ^ d, k = 0xCA62C1D6;
    uint32_t t = ROL(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = ROL(b, 30);
    b = a;
    a = t;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void SHA1Init_bridge(SHA1_CTX *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count[0] = ctx->count[1] = 0;
}

void SHA1Update_bridge(SHA1_CTX *ctx, const uint8_t *data, uint32_t len) {
  uint32_t j = (ctx->count[0] >> 3) & 63;
  if ((ctx->count[0] += len << 3) < (len << 3))
    ctx->count[1]++;
  ctx->count[1] += len >> 29;
  uint32_t i = 0;
  if (j + len > 63) {
    i = 64 - j;
    memcpy(&ctx->buffer[j], data, i);
    sha1_transform(ctx->state, ctx->buffer);
    for (; i + 63 < len; i += 64)
      sha1_transform(ctx->state, data + i);
    j = 0;
  }
  memcpy(&ctx->buffer[j], data + i, len - i);
}

void SHA1Final_bridge(uint8_t digest[20], SHA1_CTX *ctx) {
  uint8_t count[8];
  for (int i = 0; i < 8; i++)
    count[i] = (ctx->count[i >= 4 ? 0 : 1] >> ((3 - (i & 3)) * 8)) & 255;
  uint8_t c = 0x80;
  SHA1Update_bridge(ctx, &c, 1);
  c = 0;
  while ((ctx->count[0] & 504) != 448)
    SHA1Update_bridge(ctx, &c, 1);
  SHA1Update_bridge(ctx, count, 8);
  for (int i = 0; i < 20; i++)
    digest[i] = (ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255;
}

// --- libgcc helper bionic re-exported --------------------------------------

uint64_t __gnu_uldivmod_helper_bridge(uint64_t a, uint64_t b, uint64_t *rem) {
  uint64_t q = a / b;
  *rem = a - q * b;
  return q;
}

// --- network (not wired yet) ------------------------------------------------

static int h_errno_value;
int *__get_h_errno_bridge(void) { return &h_errno_value; }

int gethostname_bridge(char *name, size_t len) {
  strncpy(name, "vita", len);
  return 0;
}

const char *gai_strerror_bridge(int code) { return "getaddrinfo not available"; }

int net_unavailable_bridge(void) {
  signal_poll();  // Mono's I/O threads retry these in loops
  static int warned;
  if (!warned++)
    L_WARN("network call before the socket bridge is implemented (from %p)", __builtin_return_address(0));
  *__errno_bionic() = 101;  // ENETUNREACH
  return -1;
}

// --- neutral stubs ----------------------------------------------------------

int ret0_bridge(void) {
  signal_poll();
  return 0;
}
