#ifndef SRR_HTTP_H
#define SRR_HTTP_H

#include <stddef.h>
#include <stdint.h>

// Brings up the Vita network stack and the TLS context (CA roots, RNG).
int net_init(void);

typedef struct {
  void *user;
  // Status line, e.g. "HTTP/1.1 200 OK". Return 0 to abort.
  int (*on_status)(void *user, int code, const char *line);
  int (*on_header)(void *user, const char *name, const char *value);
  int (*on_data)(void *user, const uint8_t *data, size_t len, size_t received, size_t total);
} http_callbacks;

typedef struct {
  const char *name;
  const char *value;
} http_header;

// Blocking HTTP/1.1 request over plain TCP or TLS (mbedTLS), following up to
// five redirects. Returns the final status code, or -1 with `err` filled.
int http_request(const char *method, const char *url, const http_header *headers, int num_headers,
                 const void *body, size_t body_len, const http_callbacks *cb, char *err, size_t err_size);

#endif
