// HTTP/1.1 client for Unity's WWW, entirely on the Vita: BSD sockets over
// sceNet, TLS 1.2/1.3 through mbedTLS (the Vita's own SSL can't talk to the
// Revival servers: Cloudflare with ECDSA certificates), certificates checked
// against the roots embedded in ca_roots.c.

#define _GNU_SOURCE  // strcasestr
#include "http.h"
#include "../util/log.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

extern const char ca_roots_pem[];

#define USER_AGENT "Dalvik/1.6.0 (Linux; U; Android 4.4.2; PS Vita Build/KOT49H)"
#define IO_TIMEOUT_S 30
#define MAX_REDIRECTS 5

// --- platform hooks mbedTLS asks for (see mbedtls_user_config.h) -----------

int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
  sceKernelGetRandomNumber(out, len);
  *olen = len;
  return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)(sceKernelGetProcessTimeWide() / 1000); }

// --- network + TLS setup ----------------------------------------------------

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt ca_chain;
static mbedtls_ssl_config tls_conf;
static int tls_ready;

static void tls_error(int ret, const char *what, char *err, size_t err_size) {
  char msg[128];
  mbedtls_strerror(ret, msg, sizeof(msg));
  snprintf(err, err_size, "%s: %s (-0x%04X)", what, msg, -ret);
}

int net_init(void) {
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  static uint8_t net_memory[1024 * 1024];
  SceNetInitParam param = {net_memory, sizeof(net_memory), 0};
  int r = sceNetInit(&param);
  if (r < 0 && r != (int)0x80410110)  // already initialised
    L_WARN("net: sceNetInit failed 0x%08X", r);
  sceNetCtlInit();

  int state = 0;
  sceNetCtlInetGetState(&state);
  SceNetCtlInfo info;
  memset(&info, 0, sizeof(info));
  if (state == SCE_NETCTL_STATE_CONNECTED && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
    L_INFO("net: Wi-Fi connected, IP %s", info.ip_address);
  else
    L_WARN("net: Wi-Fi not connected (state %d); online features will fail", state);

  psa_crypto_init();
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  const char *pers = "srr-vita";
  if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers,
                                 strlen(pers))) != 0) {
    L_ERROR("tls: RNG seed failed -0x%04X", -r);
    return -1;
  }
  mbedtls_x509_crt_init(&ca_chain);
  r = mbedtls_x509_crt_parse(&ca_chain, (const unsigned char *)ca_roots_pem, strlen(ca_roots_pem) + 1);
  if (r < 0)
    L_ERROR("tls: CA roots failed to parse -0x%04X", -r);
  else if (r > 0)
    L_WARN("tls: %d CA roots could not be parsed", r);

  mbedtls_ssl_config_init(&tls_conf);
  mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                              MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&tls_conf, &ca_chain, NULL);
  mbedtls_ssl_conf_rng(&tls_conf, mbedtls_ctr_drbg_random, &drbg);
  tls_ready = 1;
  L_INFO("tls: mbedTLS %s ready", MBEDTLS_VERSION_STRING);
  return 0;
}

// --- connections ------------------------------------------------------------

typedef struct {
  int fd;
  int tls;
  mbedtls_ssl_context ssl;
  uint8_t buf[8192];
  size_t pos, len;
} conn;

static int sock_send(void *ctx, const unsigned char *buf, size_t len) {
  int r = send(*(int *)ctx, buf, len, 0);
  return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
}

static int sock_recv(void *ctx, unsigned char *buf, size_t len) {
  int r = recv(*(int *)ctx, buf, len, 0);
  return r < 0 ? MBEDTLS_ERR_SSL_TIMEOUT : r;
}

static int conn_open(conn *c, const char *host, int port, int tls, char *err, size_t err_size) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%d", port);
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
    snprintf(err, err_size, "cannot resolve %s", host);
    return -1;
  }
  c->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (c->fd < 0 || connect(c->fd, res->ai_addr, res->ai_addrlen) < 0) {
    snprintf(err, err_size, "cannot connect to %s:%d", host, port);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  struct timeval tv = {IO_TIMEOUT_S, 0};
  setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (!tls)
    return 0;
  if (!tls_ready) {
    snprintf(err, err_size, "TLS not initialised");
    return -1;
  }
  c->tls = 1;
  mbedtls_ssl_init(&c->ssl);
  int r = mbedtls_ssl_setup(&c->ssl, &tls_conf);
  if (r == 0)
    r = mbedtls_ssl_set_hostname(&c->ssl, host);
  if (r != 0) {
    tls_error(r, "TLS setup", err, err_size);
    return -1;
  }
  mbedtls_ssl_set_bio(&c->ssl, &c->fd, sock_send, sock_recv, NULL);
  while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
    if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
      tls_error(r, "TLS handshake", err, err_size);
      uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
      if (flags) {
        char info[256];
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
        L_ERROR("tls: certificate check for %s failed: %s", host, info);
      }
      return -1;
    }
  }
  L_DEBUG("tls: %s connected (%s, %s)", host, mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
  return 0;
}

static void conn_close(conn *c) {
  if (c->tls) {
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
  }
  if (c->fd >= 0)
    close(c->fd);
  c->fd = -1;
}

static int conn_write(conn *c, const void *data, size_t len) {
  const uint8_t *p = data;
  while (len) {
    int r = c->tls ? mbedtls_ssl_write(&c->ssl, p, len) : send(c->fd, p, len, 0);
    if (c->tls && (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE))
      continue;
    if (r <= 0)
      return -1;
    p += r;
    len -= r;
  }
  return 0;
}

// Refills the buffer; returns bytes now available, 0 on EOF, -1 on error.
static int conn_fill(conn *c) {
  if (c->pos < c->len)
    return c->len - c->pos;
  c->pos = c->len = 0;
  for (;;) {
    int r = c->tls ? mbedtls_ssl_read(&c->ssl, c->buf, sizeof(c->buf)) : recv(c->fd, c->buf, sizeof(c->buf), 0);
    if (c->tls && (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE))
      continue;
    if (c->tls && r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
      return 0;
    if (r < 0)
      return -1;
    c->len = r;
    return r;
  }
}

static int read_line(conn *c, char *line, size_t size) {
  size_t n = 0;
  for (;;) {
    int r = conn_fill(c);
    if (r <= 0)
      return n ? (int)n : -1;
    char ch = c->buf[c->pos++];
    if (ch == '\n')
      break;
    if (ch != '\r' && n + 1 < size)
      line[n++] = ch;
  }
  line[n] = 0;
  return n;
}

// --- requests ---------------------------------------------------------------

typedef struct {
  int tls;
  char host[256];
  int port;
  char path[2048];
} url_parts;

static int parse_url(const char *url, url_parts *u) {
  memset(u, 0, sizeof(*u));
  const char *p;
  if (strncasecmp(url, "https://", 8) == 0) {
    u->tls = 1;
    u->port = 443;
    p = url + 8;
  } else if (strncasecmp(url, "http://", 7) == 0) {
    u->port = 80;
    p = url + 7;
  } else {
    return -1;
  }
  size_t hl = strcspn(p, ":/?");
  if (hl == 0 || hl >= sizeof(u->host))
    return -1;
  memcpy(u->host, p, hl);
  p += hl;
  if (*p == ':') {
    u->port = atoi(p + 1);
    p += strcspn(p, "/?");
  }
  snprintf(u->path, sizeof(u->path), "%s%s", *p == '/' ? "" : "/", p);
  return 0;
}

static int has_header(const http_header *h, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (strcasecmp(h[i].name, name) == 0)
      return 1;
  return 0;
}

static int send_request(conn *c, const char *method, const url_parts *u, const http_header *headers, int num_headers,
                        const void *body, size_t body_len) {
  char *req = malloc(8192 + 64 * num_headers + 4096);
  int n = 0;
  int default_port = u->port == (u->tls ? 443 : 80);
  n += sprintf(req + n, "%s %s HTTP/1.1\r\nHost: %s", method, u->path, u->host);
  if (!default_port)
    n += sprintf(req + n, ":%d", u->port);
  n += sprintf(req + n, "\r\nUser-Agent: %s\r\nAccept-Encoding: identity\r\nConnection: close\r\n", USER_AGENT);
  for (int i = 0; i < num_headers; i++) {
    if (!strcasecmp(headers[i].name, "Host") || !strcasecmp(headers[i].name, "Connection") ||
        !strcasecmp(headers[i].name, "Content-Length") || !strcasecmp(headers[i].name, "Accept-Encoding"))
      continue;
    n += snprintf(req + n, 4096, "%s: %s\r\n", headers[i].name, headers[i].value);
  }
  if (body) {
    if (!has_header(headers, num_headers, "Content-Type"))
      n += sprintf(req + n, "Content-Type: application/x-www-form-urlencoded\r\n");
    n += sprintf(req + n, "Content-Length: %u\r\n", (unsigned)body_len);
  }
  n += sprintf(req + n, "\r\n");
  int r = conn_write(c, req, n);
  free(req);
  if (r == 0 && body && body_len)
    r = conn_write(c, body, body_len);
  return r;
}

int http_request(const char *method, const char *url, const http_header *headers, int num_headers,
                 const void *body, size_t body_len, const http_callbacks *cb, char *err, size_t err_size) {
  char current[2048];
  strncpy(current, url, sizeof(current) - 1);
  current[sizeof(current) - 1] = 0;
  char cur_method[16];
  strncpy(cur_method, method, sizeof(cur_method) - 1);
  cur_method[sizeof(cur_method) - 1] = 0;

  for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
    url_parts u;
    if (parse_url(current, &u) < 0) {
      snprintf(err, err_size, "unsupported URL: %s", current);
      return -1;
    }
    conn *c = malloc(sizeof(conn));
    if (conn_open(c, u.host, u.port, u.tls, err, err_size) < 0 ||
        send_request(c, cur_method, &u, headers, num_headers, body, body_len) < 0) {
      if (!err[0])
        snprintf(err, err_size, "send failed");
      conn_close(c);
      free(c);
      return -1;
    }

    char line[4096];
    if (read_line(c, line, sizeof(line)) < 0 || strncmp(line, "HTTP/", 5) != 0) {
      snprintf(err, err_size, "no HTTP response from %s", u.host);
      conn_close(c);
      free(c);
      return -1;
    }
    int code = atoi(strchr(line, ' ') ? strchr(line, ' ') + 1 : "0");
    char status[256];
    strncpy(status, line, sizeof(status) - 1);
    status[sizeof(status) - 1] = 0;

    // Headers: kept until we know this isn't a redirect.
    http_header resp[64];
    char *storage[64];
    int nresp = 0;
    long content_length = -1;
    int chunked = 0;
    char location[2048] = "";
    while (read_line(c, line, sizeof(line)) > 0) {
      char *colon = strchr(line, ':');
      if (!colon)
        continue;
      *colon = 0;
      char *value = colon + 1;
      while (*value == ' ')
        value++;
      if (!strcasecmp(line, "Content-Length"))
        content_length = atol(value);
      else if (!strcasecmp(line, "Transfer-Encoding") && strcasestr(value, "chunked"))
        chunked = 1;
      else if (!strcasecmp(line, "Location"))
        strncpy(location, value, sizeof(location) - 1);
      if (nresp < 64) {
        size_t nl = strlen(line);
        storage[nresp] = malloc(nl + strlen(value) + 2);
        strcpy(storage[nresp], line);
        strcpy(storage[nresp] + nl + 1, value);
        resp[nresp].name = storage[nresp];
        resp[nresp].value = storage[nresp] + nl + 1;
        nresp++;
      }
    }

    int redirect = (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && location[0];
    if (redirect && hop < MAX_REDIRECTS) {
      if (location[0] == '/')
        snprintf(current, sizeof(current), "%s://%s%s", u.tls ? "https" : "http", u.host, location);
      else
        strncpy(current, location, sizeof(current) - 1);
      if (code == 303 || ((code == 301 || code == 302) && strcmp(cur_method, "GET") != 0)) {
        strcpy(cur_method, "GET");
        body = NULL;
        body_len = 0;
      }
      L_DEBUG("http: %d redirect to %s", code, current);
      for (int i = 0; i < nresp; i++)
        free(storage[i]);
      conn_close(c);
      free(c);
      continue;
    }

    int ok = !cb->on_status || cb->on_status(cb->user, code, status);
    for (int i = 0; i < nresp; i++) {
      if (ok && cb->on_header)
        ok = cb->on_header(cb->user, resp[i].name, resp[i].value);
      free(storage[i]);
    }

    // Body (none for 1xx/204/304, HEAD or an explicit zero length).
    size_t received = 0, total = content_length > 0 ? (size_t)content_length : 0;
    long remaining = chunked ? 0 : content_length;
    if (code == 204 || code == 304 || (code >= 100 && code < 200) || !strcmp(cur_method, "HEAD") ||
        (!chunked && content_length == 0))
      ok = 0;
    while (ok) {
      if (chunked && remaining == 0) {
        if (received && read_line(c, line, sizeof(line)) < 0)  // CRLF after previous chunk
          break;
        if (read_line(c, line, sizeof(line)) < 0)
          break;
        remaining = strtol(line, NULL, 16);
        if (remaining <= 0)
          break;
      }
      int avail = conn_fill(c);
      if (avail <= 0)
        break;
      size_t take = avail;
      if (remaining >= 0 && (long)take > remaining)
        take = remaining;
      if (take == 0)
        break;
      received += take;
      if (cb->on_data)
        ok = cb->on_data(cb->user, c->buf + c->pos, take, received, total);
      c->pos += take;
      if (remaining >= 0) {
        remaining -= take;
        if (!chunked && remaining == 0)
          break;
      }
    }
    conn_close(c);
    free(c);
    L_INFO("http: %s %s -> %d (%u bytes)", cur_method, current, code, (unsigned)received);
    return code;
  }
  snprintf(err, err_size, "too many redirects");
  return -1;
}
