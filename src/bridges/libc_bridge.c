// Bionic (Android libc) compatibility on top of VitaSDK newlib: structure
// layouts, flag/errno numbering, stdio stream handles and path translation.

#include "bridges.h"
#include "../config.h"
#include "../util/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

// --- errno ------------------------------------------------------------------

static const struct { int newlib, bionic; } errno_map[] = {
    {EDEADLK, 35},     {ENAMETOOLONG, 36}, {ENOLCK, 37},       {ENOSYS, 38},     {ENOTEMPTY, 39},
    {ELOOP, 40},       {ENOMSG, 42},       {EIDRM, 43},        {ENOSTR, 60},     {ENODATA, 61},
    {ETIME, 62},       {ENOSR, 63},        {ENOLINK, 67},      {EPROTO, 71},     {EMULTIHOP, 72},
    {EBADMSG, 74},     {EOVERFLOW, 75},    {EILSEQ, 84},       {ENOTSOCK, 88},   {EDESTADDRREQ, 89},
    {EMSGSIZE, 90},    {EPROTOTYPE, 91},   {ENOPROTOOPT, 92},  {EPROTONOSUPPORT, 93},
    {EOPNOTSUPP, 95},  {ENOTSUP, 95},      {EAFNOSUPPORT, 97}, {EADDRINUSE, 98}, {EADDRNOTAVAIL, 99},
    {ENETDOWN, 100},   {ENETUNREACH, 101}, {ENETRESET, 102},   {ECONNABORTED, 103},
    {ECONNRESET, 104}, {ENOBUFS, 105},     {EISCONN, 106},     {ENOTCONN, 107},  {ETIMEDOUT, 110},
    {ECONNREFUSED, 111}, {EHOSTUNREACH, 113}, {EALREADY, 114}, {EINPROGRESS, 115}, {ECANCELED, 125},
};

int errno_to_bionic(int e) {
  if (e < 35)
    return e;
  for (size_t i = 0; i < sizeof(errno_map) / sizeof(errno_map[0]); i++) {
    if (errno_map[i].newlib == e)
      return errno_map[i].bionic;
  }
  return e;
}

static pthread_key_t errno_key;
static pthread_once_t errno_once = PTHREAD_ONCE_INIT;
static void errno_key_init(void) { pthread_key_create(&errno_key, free); }

int *__errno_bionic(void) {
  pthread_once(&errno_once, errno_key_init);
  int *e = pthread_getspecific(errno_key);
  if (!e) {
    e = calloc(1, sizeof(int));
    pthread_setspecific(errno_key, e);
  }
  return e;
}

#define SET_ERRNO() (*__errno_bionic() = errno_to_bionic(errno))
#define RET_ERRNO(expr) ({ __typeof__(expr) _r = (expr); if (_r < 0) SET_ERRNO(); _r; })

// --- ctype tables (BSD layout: index 0 is EOF) -------------------------------

static char ctype_table[1 + 256];
static short tolower_table[1 + 256];
static short toupper_table[1 + 256];
const char *_ctype_bionic = ctype_table;
const short *_tolower_tab_bionic = tolower_table;
const short *_toupper_tab_bionic = toupper_table;

static void ctype_init(void) {
  tolower_table[0] = toupper_table[0] = -1;
  for (int c = 0; c < 256; c++) {
    char f = 0;
    if (c < 128) {
      if (isupper(c)) f |= 0x01;
      if (islower(c)) f |= 0x02;
      if (isdigit(c)) f |= 0x04;
      if (isspace(c)) f |= 0x08;
      if (ispunct(c)) f |= 0x10;
      if (iscntrl(c)) f |= 0x20;
      if (isxdigit(c) && !isdigit(c)) f |= 0x40;
      if (c == ' ') f |= 0x80;
    }
    ctype_table[1 + c] = f;
    tolower_table[1 + c] = c < 128 ? tolower(c) : c;
    toupper_table[1 + c] = c < 128 ? toupper(c) : c;
  }
}

// --- stdio ------------------------------------------------------------------

#define BIONIC_FILE_SIZE 84
uint8_t __sF_bionic[3 * BIONIC_FILE_SIZE];

static FILE *F(void *p) {
  uintptr_t a = (uintptr_t)p, b = (uintptr_t)__sF_bionic;
  if (a >= b && a < b + sizeof(__sF_bionic)) {
    switch ((a - b) / BIONIC_FILE_SIZE) {
    case 0: return stdin;
    case 1: return stdout;
    default: return stderr;
    }
  }
  return p;
}

static int is_std_stream(FILE *f) { return f == stdout || f == stderr; }

FILE *fopen_bridge(const char *path, const char *mode) {
  char buf[512];
  const char *p = fs_translate(path, buf, sizeof(buf));
  FILE *f = fopen(p, mode);
  if (!f) {
    SET_ERRNO();
    L_DEBUG("fopen(%s -> %s, %s) failed", path, p, mode);
  }
  return f;
}

FILE *fdopen_bridge(int fd, const char *mode) { return fdopen(fd, mode); }
int fclose_bridge(void *f) { return fclose(F(f)); }
int fflush_bridge(void *f) { return f ? fflush(F(f)) : fflush(NULL); }
int ferror_bridge(void *f) { return ferror(F(f)); }
void clearerr_bridge(void *f) { clearerr(F(f)); }
size_t fread_bridge(void *p, size_t s, size_t n, void *f) { return fread(p, s, n, F(f)); }
int fseek_bridge(void *f, long off, int whence) { return RET_ERRNO(fseek(F(f), off, whence)); }
long ftell_bridge(void *f) { return ftell(F(f)); }
char *fgets_bridge(char *s, int n, void *f) { return fgets(s, n, F(f)); }
int getc_bridge(void *f) { return getc(F(f)); }
int ungetc_bridge(int c, void *f) { return ungetc(c, F(f)); }
wint_t getwc_bridge(void *f) { return getwc(F(f)); }
wint_t ungetwc_bridge(wint_t c, void *f) { return ungetwc(c, F(f)); }
int setvbuf_bridge(void *f, char *buf, int mode, size_t size) { return setvbuf(F(f), buf, mode, size); }

static void log_stream(const char *s, size_t len) {
  char buf[512];
  if (len >= sizeof(buf))
    len = sizeof(buf) - 1;
  memcpy(buf, s, len);
  buf[len] = 0;
  log_printf("[stdout] %s", buf);
}

size_t fwrite_bridge(const void *p, size_t s, size_t n, void *f) {
  FILE *ff = F(f);
  if (is_std_stream(ff)) {
    log_stream(p, s * n);
    return n;
  }
  return fwrite(p, s, n, ff);
}

int fputs_bridge(const char *s, void *f) {
  FILE *ff = F(f);
  if (is_std_stream(ff)) {
    log_stream(s, strlen(s));
    return 0;
  }
  return fputs(s, ff);
}

int fputc_bridge(int c, void *f) {
  FILE *ff = F(f);
  return is_std_stream(ff) ? c : fputc(c, ff);
}

wint_t putwc_bridge(wchar_t c, void *f) {
  FILE *ff = F(f);
  return is_std_stream(ff) ? (wint_t)c : putwc(c, ff);
}

int vfprintf_bridge(void *f, const char *fmt, va_list ap) {
  char fixbuf[1024];
  fmt = fmt_c99(fmt, fixbuf, sizeof(fixbuf));
  FILE *ff = F(f);
  if (is_std_stream(ff)) {
    log_vprintf(fmt, ap);
    return 0;
  }
  return vfprintf(ff, fmt, ap);
}

int fprintf_bridge(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf_bridge(f, fmt, ap);
  va_end(ap);
  return r;
}

int vprintf_bridge(const char *fmt, va_list ap) {
  char fixbuf[1024];
  fmt = fmt_c99(fmt, fixbuf, sizeof(fixbuf));
  log_vprintf(fmt, ap);
  return 0;
}

int printf_bridge(const char *fmt, ...) {
  char fixbuf[1024];
  fmt = fmt_c99(fmt, fixbuf, sizeof(fixbuf));
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
  return 0;
}

int puts_bridge(const char *s) {
  log_printf("[stdout] %s", s);
  return 0;
}

int putchar_bridge(int c) { return c; }

int fscanf_bridge(void *f, const char *fmt, ...) {
  char fixbuf[1024];
  fmt = fmt_c99(fmt, fixbuf, sizeof(fixbuf));
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf(F(f), fmt, ap);
  va_end(ap);
  return r;
}

void perror_bridge(const char *s) { L_ERROR("perror: %s: %s", s ? s : "", strerror(errno)); }

int fileno_bridge(void *f) { return fileno(F(f)); }

// --- file descriptors -------------------------------------------------------

#define B_O_WRONLY    0x0001
#define B_O_RDWR      0x0002
#define B_O_CREAT     0x0040
#define B_O_EXCL      0x0080
#define B_O_TRUNC     0x0200
#define B_O_APPEND    0x0400
#define B_O_NONBLOCK  0x0800
#define B_O_DIRECTORY 0x4000

static int oflags_from_bionic(int f) {
  int r = f & 3;
  if (f & B_O_CREAT) r |= O_CREAT;
  if (f & B_O_EXCL) r |= O_EXCL;
  if (f & B_O_TRUNC) r |= O_TRUNC;
  if (f & B_O_APPEND) r |= O_APPEND;
  if (f & B_O_NONBLOCK) r |= O_NONBLOCK;
  return r;
}

int open_bridge(const char *path, int flags, ...) {
  char buf[512];
  int mode = 0666;
  if (flags & B_O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  const char *p = fs_translate(path, buf, sizeof(buf));
  if (flags & B_O_DIRECTORY) {
    // newlib can't open directories as fds; fake a handle only if it exists.
    struct stat st;
    if (stat(p, &st) < 0 || !S_ISDIR(st.st_mode)) {
      *__errno_bionic() = ENOENT;
      return -1;
    }
    return open("ux0:", O_RDONLY);
  }
  int fd = open(p, oflags_from_bionic(flags), mode);
  if (fd < 0) {
    SET_ERRNO();
    L_DEBUG("open(%s -> %s, 0x%x) failed", path, p, flags);
  }
  return fd;
}

int fcntl_bridge(int fd, int cmd, ...) {
  // F_GETFD/F_SETFD/F_GETFL/F_SETFL: nothing meaningful on Vita files.
  (void)fd;
  (void)cmd;
  return 0;
}

int ioctl_bridge(int fd, int req, ...) {
  L_DEBUG("ioctl(%d, 0x%x)", fd, req);
  *__errno_bionic() = 25;  // ENOTTY
  return -1;
}

int close_bridge(int fd) { return RET_ERRNO(close(fd)); }
ssize_t read_bridge(int fd, void *b, size_t n) {
  signal_poll();
  return RET_ERRNO(read(fd, b, n));
}
ssize_t write_bridge(int fd, const void *b, size_t n) {
  if (fd == 1 || fd == 2) {
    log_stream(b, n);
    return n;
  }
  return RET_ERRNO(write(fd, b, n));
}
off_t lseek_bridge(int fd, off_t off, int whence) { return RET_ERRNO(lseek(fd, off, whence)); }
int fsync_bridge(int fd) { return 0; }
int ftruncate_bridge(int fd, off_t len) { return RET_ERRNO(ftruncate(fd, len)); }
int flock_bridge(int fd, int op) { return 0; }
int dup2_bridge(int a, int b) { return b; }
int isatty_bridge(int fd) { return 0; }

typedef struct {
  int iov_base;
  size_t iov_len;
} bionic_iovec;

ssize_t writev_bridge(int fd, const bionic_iovec *iov, int cnt) {
  ssize_t total = 0;
  for (int i = 0; i < cnt; i++) {
    ssize_t r = write_bridge(fd, (void *)iov[i].iov_base, iov[i].iov_len);
    if (r < 0)
      return total ? total : r;
    total += r;
  }
  return total;
}

// --- stat -------------------------------------------------------------------

typedef struct {
  unsigned long long st_dev;
  unsigned char __pad0[4];
  unsigned long __st_ino;
  unsigned int st_mode;
  unsigned int st_nlink;
  unsigned long st_uid;
  unsigned long st_gid;
  unsigned long long st_rdev;
  unsigned char __pad3[4];
  long long st_size;
  unsigned long st_blksize;
  unsigned long long st_blocks;
  unsigned long st_atime_;
  unsigned long st_atime_nsec;
  unsigned long st_mtime_;
  unsigned long st_mtime_nsec;
  unsigned long st_ctime_;
  unsigned long st_ctime_nsec;
  unsigned long long st_ino;
} bionic_stat;

_Static_assert(sizeof(bionic_stat) == 104, "bionic stat layout");

static void stat_to_bionic(const struct stat *s, bionic_stat *b) {
  memset(b, 0, sizeof(*b));
  b->st_mode = s->st_mode;
  b->st_nlink = 1;
  b->st_size = s->st_size;
  b->st_blksize = 512;
  b->st_blocks = (s->st_size + 511) / 512;
  b->st_atime_ = s->st_atime;
  b->st_mtime_ = s->st_mtime;
  b->st_ctime_ = s->st_ctime;
  b->st_ino = b->__st_ino = s->st_ino;
}

int stat_bridge(const char *path, bionic_stat *out) {
  char buf[512];
  const char *p = fs_translate(path, buf, sizeof(buf));
  struct stat st;
  if (stat(p, &st) < 0) {
    SET_ERRNO();
    return -1;
  }
  stat_to_bionic(&st, out);
  return 0;
}

int fstat_bridge(int fd, bionic_stat *out) {
  struct stat st;
  if (fstat(fd, &st) < 0) {
    SET_ERRNO();
    return -1;
  }
  stat_to_bionic(&st, out);
  return 0;
}

// --- paths ------------------------------------------------------------------

#define PATH_WRAP1(name, call)                        \
  int name##_bridge(const char *path) {               \
    char buf[512];                                    \
    const char *p = fs_translate(path, buf, sizeof(buf)); \
    return RET_ERRNO(call);                           \
  }

PATH_WRAP1(unlink, unlink(p))
PATH_WRAP1(rmdir, rmdir(p))
PATH_WRAP1(remove, remove(p))

int chdir_bridge(const char *path) { return 0; }

int mkdir_bridge(const char *path, int mode) {
  char buf[512];
  const char *p = fs_translate(path, buf, sizeof(buf));
  return RET_ERRNO(mkdir(p, mode));
}

int access_bridge(const char *path, int mode) {
  char buf[512];
  const char *p = fs_translate(path, buf, sizeof(buf));
  struct stat st;
  if (stat(p, &st) < 0) {
    *__errno_bionic() = ENOENT;
    return -1;
  }
  return 0;
}

int rename_bridge(const char *a, const char *b) {
  char ba[512], bb[512];
  const char *pa = fs_translate(a, ba, sizeof(ba));
  const char *pb = fs_translate(b, bb, sizeof(bb));
  remove(pb);
  return RET_ERRNO(rename(pa, pb));
}

int chmod_bridge(const char *path, int mode) { return 0; }
int utime_bridge(const char *path, const void *times) { return 0; }

int truncate_bridge(const char *path, off_t len) {
  char buf[512];
  const char *p = fs_translate(path, buf, sizeof(buf));
  return RET_ERRNO(truncate(p, len));
}

char *getcwd_bridge(char *buf, size_t size) {
  const char *cwd = ANDROID_DATA_DIR "/files";
  if (!buf) {
    buf = malloc(size ? size : strlen(cwd) + 1);
    size = size ? size : strlen(cwd) + 1;
  }
  strncpy(buf, cwd, size);
  return buf;
}

ssize_t readlink_bridge(const char *path, char *buf, size_t size) {
  *__errno_bionic() = EINVAL;
  return -1;
}

int mkstemp_bridge(char *tmpl) {
  char buf[512];
  static int counter;
  size_t len = strlen(tmpl);
  if (len >= 6)
    snprintf(tmpl + len - 6, 7, "%06d", ++counter % 1000000);
  const char *p = fs_translate(tmpl, buf, sizeof(buf));
  return RET_ERRNO(open(p, O_RDWR | O_CREAT | O_EXCL, 0600));
}

// --- dirent -----------------------------------------------------------------

typedef struct {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
} bionic_dirent;

typedef struct {
  DIR *dir;
  bionic_dirent ent;
} bionic_DIR;

void *opendir_bridge(const char *path) {
  char buf[512];
  const char *p = fs_translate(path, buf, sizeof(buf));
  DIR *d = opendir(p);
  if (!d) {
    SET_ERRNO();
    return NULL;
  }
  bionic_DIR *bd = calloc(1, sizeof(*bd));
  bd->dir = d;
  return bd;
}

bionic_dirent *readdir_bridge(bionic_DIR *bd) {
  struct dirent *e = readdir(bd->dir);
  if (!e)
    return NULL;
  memset(&bd->ent, 0, sizeof(bd->ent));
  strncpy(bd->ent.d_name, e->d_name, sizeof(bd->ent.d_name) - 1);
  bd->ent.d_type = SCE_S_ISDIR(e->d_stat.st_mode) ? 4 : 8;
  bd->ent.d_reclen = sizeof(bd->ent);
  bd->ent.d_ino = 1;
  return &bd->ent;
}

int closedir_bridge(bionic_DIR *bd) {
  int r = closedir(bd->dir);
  free(bd);
  return r;
}

// --- time -------------------------------------------------------------------

#define B_CLOCK_REALTIME 0
#define B_CLOCK_MONOTONIC 1

int clock_gettime_bridge(int clk, struct timespec *ts) {
  signal_poll();  // busy loops poll the clock: a good place to take signals
  static uint32_t clocks_seen;
  if (clk >= 0 && clk < 32 && !(clocks_seen & (1u << clk))) {
    clocks_seen |= 1u << clk;
    L_INFO("time: clock_gettime(%d) first used from %p", clk, __builtin_return_address(0));
  }
  if (clk == B_CLOCK_REALTIME) {
    SceDateTime t;
    sceRtcGetCurrentClockUtc(&t);
    SceRtcTick tick;
    sceRtcGetTick(&t, &tick);
    // Ticks are microseconds since 0001-01-01; Unix epoch is 62135596800 s later.
    uint64_t us = tick.tick - 62135596800000000ULL;
    ts->tv_sec = us / 1000000;
    ts->tv_nsec = (us % 1000000) * 1000;
  } else {
    uint64_t us = sceKernelGetProcessTimeWide();
    ts->tv_sec = us / 1000000;
    ts->tv_nsec = (us % 1000000) * 1000;
  }
  return 0;
}

int clock_getres_bridge(int clk, struct timespec *ts) {
  if (ts) {
    ts->tv_sec = 0;
    ts->tv_nsec = 1000;
  }
  return 0;
}

int gettimeofday_bridge(struct timeval *tv, void *tz) {
  struct timespec ts;
  clock_gettime_bridge(B_CLOCK_REALTIME, &ts);
  if (tv) {
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
  }
  return 0;
}

time_t time_bridge(time_t *t) {
  struct timespec ts;
  clock_gettime_bridge(B_CLOCK_REALTIME, &ts);
  if (t)
    *t = ts.tv_sec;
  return ts.tv_sec;
}

// Sleeps in slices so queued signals still get handled (see signal_bridge.c).
static void sleep_us(uint64_t us) {
  do {
    uint64_t slice = us > 20000 ? 20000 : us;
    sceKernelDelayThreadCB(slice);
    us -= slice;
    signal_poll();
  } while (us > 0);
}

int nanosleep_bridge(const struct timespec *req, struct timespec *rem) {
  WAIT_BEGIN("nanosleep", req);
  sleep_us((uint64_t)req->tv_sec * 1000000 + req->tv_nsec / 1000);
  thread_wait_end();
  if (rem)
    rem->tv_sec = rem->tv_nsec = 0;
  return 0;
}

int clock_nanosleep_bridge(int clk, int flags, const struct timespec *req, struct timespec *rem) {
  return nanosleep_bridge(req, rem);
}

int usleep_bridge(unsigned int us) {
  WAIT_BEGIN("usleep", (void *)us);
  sleep_us(us);
  thread_wait_end();
  return 0;
}

unsigned int sleep_bridge(unsigned int s) {
  sleep_us((uint64_t)s * 1000000);
  return 0;
}

long clock_bridge(void) { return sceKernelGetProcessTimeWide(); }

// --- process / system -------------------------------------------------------

int __system_property_get_bridge(const char *name, char *value) {
  static const struct { const char *k, *v; } props[] = {
      {"ro.product.model", "PS Vita"},     {"ro.product.manufacturer", "Sony"},
      {"ro.product.brand", "Sony"},        {"ro.product.device", "vita"},
      {"ro.build.version.sdk", "19"},      {"ro.build.version.release", "4.4.2"},
      {"ro.product.cpu.abi", "armeabi-v7a"}, {"ro.hardware", "vita"},
      {"ro.build.fingerprint", "sony/vita/vita:4.4.2/KOT49H/1:user/release-keys"},
  };
  for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
    if (strcmp(name, props[i].k) == 0) {
      strcpy(value, props[i].v);
      return strlen(value);
    }
  }
  value[0] = 0;
  return 0;
}

long sysconf_bridge(int name) {
  switch (name) {
  case 0x27: return 4096;  // _SC_PAGESIZE
  case 0x28: return 4096;  // _SC_PAGE_SIZE
  case 0x60: return 3;     // _SC_NPROCESSORS_CONF
  case 0x61: return 3;     // _SC_NPROCESSORS_ONLN
  case 0x62: return 128 * 1024 * 1024 / 4096;  // _SC_PHYS_PAGES
  case 0x06: return 100;   // _SC_CLK_TCK
  case 0x07: return 1024;  // _SC_OPEN_MAX
  default:
    L_DEBUG("sysconf(%d)", name);
    return 0;
  }
}

int getpid_bridge(void) { return 1000; }
int gettid_bridge(void) { return sceKernelGetThreadId(); }
int getuid_bridge(void) { return 10000; }
int getpriority_bridge(int which, int who) { return 0; }
int setpriority_bridge(int which, int who, int prio) { return 0; }
int getdtablesize_bridge(void) { return 1024; }
int prctl_bridge(int opt, ...) { return 0; }
int getrusage_bridge(int who, void *usage) {
  memset(usage, 0, 72);
  return 0;
}

typedef struct {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
} bionic_utsname;

int uname_bridge(bionic_utsname *u) {
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "Linux");
  strcpy(u->nodename, "vita");
  strcpy(u->release, "3.4.0");
  strcpy(u->version, "#1 SMP PREEMPT");
  strcpy(u->machine, "armv7l");
  return 0;
}

char *getenv_bridge(const char *name) {
  if (strcmp(name, "HOME") == 0) return ANDROID_DATA_DIR "/files";
  if (strcmp(name, "TMPDIR") == 0) return ANDROID_DATA_DIR "/cache";
  // Mono tuning: keep the managed heap small and the JIT predictable.
  if (strcmp(name, "MONO_GC_PARAMS") == 0) return NULL;
  return getenv(name);
}

void abort_bridge(void) {
  L_ERROR("abort() called from %p", __builtin_return_address(0));
  sceKernelExitProcess(0);
}

void exit_bridge(int code) {
  L_INFO("exit(%d) called from %p", code, __builtin_return_address(0));
  sceKernelExitProcess(0);
}

int fail_bridge(void) {
  signal_poll();
  *__errno_bionic() = ENOSYS;
  return -1;
}

// --- logging ----------------------------------------------------------------

static const char *prio_name(int prio) {
  static const char *names[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
  return prio >= 0 && prio <= 8 ? names[prio] : "?";
}

int __android_log_print_bridge(int prio, const char *tag, const char *fmt, ...) {
  char fixbuf[1024];
  fmt = fmt_c99(fmt, fixbuf, sizeof(fixbuf));
  char buf[768];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_printf("[%s/%s] %s", prio_name(prio), tag ? tag : "", buf);
  return 0;
}

int __android_log_vprint_bridge(int prio, const char *tag, const char *fmt, va_list ap) {
  char fixbuf[1024];
  fmt = fmt_c99(fmt, fixbuf, sizeof(fixbuf));
  char buf[768];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  log_printf("[%s/%s] %s", prio_name(prio), tag ? tag : "", buf);
  return 0;
}

int __android_log_write_bridge(int prio, const char *tag, const char *text) {
  log_printf("[%s/%s] %s", prio_name(prio), tag ? tag : "", text);
  return 0;
}

// --- misc -------------------------------------------------------------------

uintptr_t __stack_chk_guard_bridge = 0x2F1A7C93;
void __stack_chk_fail_bridge(void) {
  L_ERROR("stack smashing detected near %p", __builtin_return_address(0));
  sceKernelExitProcess(0);
}

int __page_size_bridge = 4096;
static char *environ_list[] = {NULL};
char **environ_bridge = environ_list;
uint32_t data_start_bridge[4];

int __cxa_atexit_bridge(void (*f)(void *), void *arg, void *dso) { return 0; }
void __cxa_finalize_bridge(void *dso) {}

// sigsetjmp/siglongjmp ignore the signal mask; they must tail-call so the
// saved frame is the caller's.
__asm__(
    ".arm\n"
    ".global sigsetjmp_bridge\n"
    ".type sigsetjmp_bridge, %function\n"
    "sigsetjmp_bridge:\n"
    "  b setjmp\n"
    ".global siglongjmp_bridge\n"
    ".type siglongjmp_bridge, %function\n"
    "siglongjmp_bridge:\n"
    "  b longjmp\n");

void libc_bridge_init(void) {
  ctype_init();
  __errno_bionic();
}
