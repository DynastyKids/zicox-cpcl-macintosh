/*
 * zcio.h - small I/O helpers shared by zicox-btrelay and zicoxctl.
 */
#ifndef ZCIO_H
#define ZCIO_H

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

/* Default pattern for the Bluetooth serial port macOS creates for a paired CC4. */
#define ZC_BT_PATTERN "/dev/cu.CC4*"

static inline long long zc_now_ms(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Resolve "auto" or a glob pattern to the first matching device path. */
static inline int zc_resolve_device(const char *spec, char *out, size_t outlen)
{
  const char *pattern = (!spec || !strcmp(spec, "auto")) ? ZC_BT_PATTERN : spec;
  if (!strpbrk(pattern, "*?[")) {
    snprintf(out, outlen, "%s", pattern);
    return access(out, F_OK) == 0 ? 0 : -1;
  }
  glob_t g;
  int rc = -1;
  if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
    snprintf(out, outlen, "%s", g.gl_pathv[0]);
    rc = 0;
  }
  globfree(&g);
  return rc;
}

/* Open a serial (Bluetooth SPP) device in raw 8N1 mode, non-blocking. */
static inline int zc_serial_open(const char *path)
{
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  struct termios t;
  if (tcgetattr(fd, &t) == 0) {
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS);
    cfsetspeed(&t, B115200);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
  }
  return fd;
}

/* Connect to host[:port] (default port 9100). Blocking socket. */
static inline int zc_tcp_connect(const char *hostport, int defport)
{
  char host[256], port[16];
  const char *colon = strrchr(hostport, ':');
  if (colon) {
    snprintf(host, sizeof(host), "%.*s", (int)(colon - hostport), hostport);
    snprintf(port, sizeof(port), "%s", colon + 1);
  } else {
    snprintf(host, sizeof(host), "%s", hostport);
    snprintf(port, sizeof(port), "%d", defport);
  }
  struct addrinfo hints = { 0 }, *res, *ai;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
  int fd = -1;
  for (ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

/* Write everything, waiting with poll() on non-blocking fds. timeout_ms <= 0: no limit. */
static inline int zc_write_all(int fd, const void *buf, size_t len, int timeout_ms)
{
  const unsigned char *p = buf;
  long long deadline = timeout_ms > 0 ? zc_now_ms() + timeout_ms : 0;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n > 0) { p += n; len -= (size_t)n; continue; }
    if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
    int wait = 500;
    if (deadline) {
      long long left = deadline - zc_now_ms();
      if (left <= 0) { errno = ETIMEDOUT; return -1; }
      if (left < wait) wait = (int)left;
    }
    struct pollfd pfd = { fd, POLLOUT, 0 };
    poll(&pfd, 1, wait);
  }
  return 0;
}

/* Read up to len bytes until timeout_ms passes with nothing new after the first byte (idle_ms). */
static inline ssize_t zc_read_timeout(int fd, void *buf, size_t len, int timeout_ms, int idle_ms)
{
  size_t got = 0;
  long long deadline = zc_now_ms() + timeout_ms;
  while (got < len) {
    long long left = deadline - zc_now_ms();
    if (left <= 0) break;
    struct pollfd pfd = { fd, POLLIN, 0 };
    int r = poll(&pfd, 1, (int)left);
    if (r <= 0) break;
    ssize_t n = read(fd, (unsigned char *)buf + got, len - got);
    if (n > 0) {
      got += (size_t)n;
      if (idle_ms > 0) deadline = zc_now_ms() + idle_ms;
    } else if (n == 0) {
      break;
    } else if (errno != EAGAIN && errno != EINTR) {
      return got ? (ssize_t)got : -1;
    }
  }
  return (ssize_t)got;
}

/* Status query used by the vendor SDK: 1D 99 00 00 -> 1D 99 <status> FF. */
#define ZC_STATUS_OK         0
#define ZC_STATUS_PAPER_OUT  1
#define ZC_STATUS_COVER_OPEN 2
#define ZC_STATUS_NO_REPLY  -1

static inline int zc_query_status(int fd, int timeout_ms, unsigned char *raw)
{
  static const unsigned char q[4] = { 0x1D, 0x99, 0x00, 0x00 };
  unsigned char r[64];
  if (zc_write_all(fd, q, sizeof(q), timeout_ms) < 0) return ZC_STATUS_NO_REPLY;
  ssize_t n = zc_read_timeout(fd, r, sizeof(r), timeout_ms, 150);
  for (ssize_t i = 0; i + 3 < n; i++) {
    if (r[i] == 0x1D && r[i + 1] == 0x99 && r[i + 3] == 0xFF) {
      if (raw) *raw = r[i + 2];
      if (r[i + 2] & 0x01) return ZC_STATUS_PAPER_OUT;
      if (r[i + 2] & 0x02) return ZC_STATUS_COVER_OPEN;
      return ZC_STATUS_OK;
    }
  }
  return ZC_STATUS_NO_REPLY;
}

#endif /* ZCIO_H */
