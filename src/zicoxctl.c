/*
 * zicoxctl - talk to a Zicox CC4 directly (Bluetooth serial port or TCP).
 *
 * Usage:
 *   zicoxctl [-d device|auto] [-n host[:port]] <command> [args]
 *
 * Targets:
 *   (default)          Bluetooth, via the ZicoxBluetooth agent's control port (CC4: 127.0.0.1:9102)
 *   -m MODEL           CC4 (default), CC2, CC3, M35 or HDT334: picks that model's agent port
 *                      and print head width (used by "text")
 *   -n host[:port]     Wi-Fi printer (port 9100) or another relay
 *   -d /dev/cu.XXX     a serial port (does not bring up Bluetooth links on current macOS)
 *
 * Commands:
 *   status             paper / cover status (1D 99 00 00)
 *   sn                 serial number (1D 49 44 00)
 *   selftest           print the self-test page
 *   feed [gap|mark]    feed to the next label (GAP-SENSE/BAR-SENSE + FORM)
 *   send FILE          send a file unchanged (CPCL, ESC/POS ...); "-" = stdin
 *   text [opts] TEXT   print text lines with a built-in printer font
 *        -f font       font number or name (default 24; e.g. 55, 24, 思源黑体)
 *        -s size       font size argument (default 0; for 思源黑体 use the dot height, e.g. 32)
 *        -l dots       line height (default: 30, or size+8 when size > 0)
 *        -x dots -y dots  start position (default 8, 8)
 *        -m gap|cont   media (default cont)
 *        -e enc        text encoding sent to the printer (default GBK; or UTF-8)
 *        TEXT lines are separated by "\n" or given as several arguments; "-" reads stdin.
 */

#include "zcio.h"

#include <iconv.h>

/* Per-model Bluetooth agent control port (see install.sh) and print head width in dots. */
static const struct { const char *name; const char *ctl; int head; } models[] = {
  { "CC4", "127.0.0.1:9102", 832 }, { "CC2", "127.0.0.1:9112", 384 }, { "CC3", "127.0.0.1:9122", 576 },
  { "M35", "127.0.0.1:9132", 576 }, { "HDT334", "127.0.0.1:9142", 832 },
};
static int g_head = 832;
static const char *g_ctl = "127.0.0.1:9102";

static int open_target(const char *dev, const char *net)
{
  if (net) {
    int fd = zc_tcp_connect(net, 9100);
    if (fd < 0) fprintf(stderr, "zicoxctl: cannot connect to %s%s\n", net,
                        !strcmp(net, g_ctl) ? " (is the ZicoxBluetooth agent running? see README)" : "");
    return fd;
  }
  char path[256];
  if (zc_resolve_device(dev, path, sizeof(path)) < 0) {
    fprintf(stderr, "zicoxctl: no device matches %s (pair the CC4 in System Settings > Bluetooth)\n",
            dev ? dev : ZC_BT_PATTERN);
    return -1;
  }
  int fd = zc_serial_open(path);
  if (fd < 0) fprintf(stderr, "zicoxctl: cannot open %s: %s\n", path, strerror(errno));
  return fd;
}

static int send_bytes(int fd, const void *p, size_t n)
{
  if (zc_write_all(fd, p, n, 60000) < 0) {
    fprintf(stderr, "zicoxctl: write failed: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

static void finish(int fd)
{
  tcdrain(fd);           /* no-op on sockets */
  usleep(1000 * 1000);   /* vendor SDK waits 1 s before disconnecting */
  close(fd);
}

static char *read_stream(FILE *f, size_t *len)
{
  size_t cap = 65536, n = 0;
  char *b = malloc(cap);
  size_t r;
  while (b && (r = fread(b + n, 1, cap - n, f)) > 0) {
    n += r;
    if (n == cap) { cap *= 2; b = realloc(b, cap); }
  }
  *len = n;
  return b;
}

static char *convert(const char *in, size_t inlen, const char *enc, size_t *outlen)
{
  if (!strcasecmp(enc, "UTF-8") || !strcasecmp(enc, "UTF8")) {
    char *o = malloc(inlen + 1);
    memcpy(o, in, inlen);
    *outlen = inlen;
    return o;
  }
  iconv_t cd = iconv_open(enc, "UTF-8");
  if (cd == (iconv_t)-1) { fprintf(stderr, "zicoxctl: unknown encoding %s\n", enc); return NULL; }
  size_t cap = inlen * 4 + 16, left = cap;
  char *o = malloc(cap), *op = o;
  char *ip = (char *)in;
  size_t il = inlen;
  while (il > 0) {
    if (iconv(cd, &ip, &il, &op, &left) == (size_t)-1) {
      if (errno == EILSEQ || errno == EINVAL) { *op++ = '?'; left--; ip++; il--; }
      else break;
    }
  }
  iconv_close(cd);
  *outlen = cap - left;
  return o;
}

static int cmd_text(int fd, int argc, char **argv)
{
  const char *font = "24", *enc = "GBK", *media = "cont";
  int size = 0, lineh = 0, x = 8, y = 8, c;
  optreset = 1;
  optind = 1;
  while ((c = getopt(argc, argv, "f:s:l:x:y:m:e:")) != -1) {
    switch (c) {
      case 'f': font = optarg; break;
      case 's': size = atoi(optarg); break;
      case 'l': lineh = atoi(optarg); break;
      case 'x': x = atoi(optarg); break;
      case 'y': y = atoi(optarg); break;
      case 'm': media = optarg; break;
      case 'e': enc = optarg; break;
      default: return 2;
    }
  }
  if (optind >= argc) { fprintf(stderr, "zicoxctl text: no text given\n"); return 2; }
  if (!lineh) lineh = size > 0 ? size + 8 : 30;

  /* Collect UTF-8 text, one line per argument / "\n". */
  size_t tlen = 0, tcap = 4096;
  char *text = malloc(tcap);
  for (int i = optind; i < argc; i++) {
    size_t n;
    char *src;
    if (!strcmp(argv[i], "-")) src = read_stream(stdin, &n);
    else { n = strlen(argv[i]); src = strdup(argv[i]); }
    while (tlen + n + 2 > tcap) { tcap *= 2; text = realloc(text, tcap); }
    for (size_t k = 0; k < n; k++) {
      if (src[k] == '\\' && k + 1 < n && src[k + 1] == 'n') { text[tlen++] = '\n'; k++; }
      else if (src[k] != '\r') text[tlen++] = src[k];
    }
    if (tlen && text[tlen - 1] != '\n') text[tlen++] = '\n';
    free(src);
  }

  int lines = 0;
  for (size_t k = 0; k < tlen; k++) if (text[k] == '\n') lines++;

  /* Build CPCL with CRLF line ends. */
  size_t blen;
  char *body = convert(text, tlen, enc, &blen);
  if (!body) return 1;

  char head[512];
  int height = y + lines * lineh + 16;
  int hl = snprintf(head, sizeof(head),
                    "! 0 200 200 %d 1\r\nPAGE-WIDTH %d\r\nML %d\r\nT ",
                    height, g_head, lineh);

  size_t fl;
  char *fontenc = convert(font, strlen(font), enc, &fl);
  char pos[64];
  int pl = snprintf(pos, sizeof(pos), " %d %d %d\r\n", size, x, y);

  const char *tail = !strcmp(media, "gap") ? "ENDML\r\nGAP-SENSE\r\nFORM\r\nPOSTFEED 24\r\nPRINT\r\n" : "ENDML\r\nPOSTFEED 24\r\nPRINT\r\n";

  int rc = 0;
  rc |= send_bytes(fd, head, (size_t)hl);
  rc |= send_bytes(fd, fontenc, fl);
  rc |= send_bytes(fd, pos, (size_t)pl);
  for (size_t k = 0; k < blen; k++) {
    if (body[k] == '\n') rc |= send_bytes(fd, "\r\n", 2);
    else rc |= send_bytes(fd, body + k, 1);
  }
  rc |= send_bytes(fd, tail, strlen(tail));
  free(text); free(body); free(fontenc);
  return rc ? 1 : 0;
}

static void usage(void)
{
  fputs("Usage: zicoxctl [-m model] [-d device] [-n host[:port]] status|sn|selftest|feed [gap|mark]|send FILE|text [opts] TEXT...\n"
        "See README.md for the options of each command.\n", stderr);
}

int main(int argc, char *argv[])
{
  const char *dev = NULL, *net = NULL;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
    else if (!strcmp(argv[i], "-n") && i + 1 < argc) net = argv[++i];
    else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
      const char *m = argv[++i];
      size_t k;
      for (k = 0; k < sizeof(models) / sizeof(models[0]); k++)
        if (!strcasecmp(m, models[k].name)) { g_ctl = models[k].ctl; g_head = models[k].head; break; }
      if (k == sizeof(models) / sizeof(models[0])) {
        fprintf(stderr, "zicoxctl: unknown model %s (CC4, CC2, CC3, M35, HDT334)\n", m);
        return 2;
      }
    }
    else { usage(); return 2; }
  }
  if (i >= argc) { usage(); return 2; }
  const char *cmd = argv[i];
  int cargc = argc - i;
  char **cargv = argv + i;

  if (!dev && !net) net = g_ctl;
  int fd = open_target(dev, net);
  if (fd < 0) return 1;

  int rc = 0;
  if (!strcmp(cmd, "status")) {
    unsigned char raw = 0;
    int st = zc_query_status(fd, 10000, &raw);  /* Bluetooth connect takes ~3 s */
    switch (st) {
      case ZC_STATUS_OK: printf("OK (0x%02x)\n", raw); break;
      case ZC_STATUS_PAPER_OUT: printf("PAPER OUT (0x%02x)\n", raw); rc = 3; break;
      case ZC_STATUS_COVER_OPEN: printf("COVER OPEN (0x%02x)\n", raw); rc = 3; break;
      default: printf("NO REPLY (printer off, out of range, or busy?)\n"); rc = 1; break;
    }
    close(fd);
    return rc;
  } else if (!strcmp(cmd, "sn")) {
    static const unsigned char q[4] = { 0x1D, 0x49, 0x44, 0x00 };
    unsigned char r[64];
    send_bytes(fd, q, sizeof(q));
    ssize_t n = zc_read_timeout(fd, r, sizeof(r), 10000, 300);
    if (n <= 0) { puts("NO REPLY"); close(fd); return 1; }
    printf("raw:");
    for (ssize_t k = 0; k < n; k++) printf(" %02x", r[k]);
    printf("\n");
    /* Vendor demo: drop everything from the first 00 byte, then read the last 4 bytes little-endian. */
    for (ssize_t k = 0; k < n; k++) if (r[k] == 0) { n = k; break; }
    if (n >= 4) {
      unsigned v = r[n - 4] | (r[n - 3] << 8) | (r[n - 2] << 16) | ((unsigned)r[n - 1] << 24);
      printf("SN (as in vendor demo): %u\n", v);
    }
    close(fd);
    return 0;
  } else if (!strcmp(cmd, "selftest")) {
    static const unsigned char q[] = { 0x1f, 0x52, 0x16, 0x00, 0x30, 0x02, 0x10, 0x00, 0x00, 0x00,
                                       'u', 'i', '_', 'p', 'r', 'n', '_', 's', 'e', 'l', 'f', 't', 'e', 's', 't', 0x00 };
    rc = send_bytes(fd, q, sizeof(q)) ? 1 : 0;
  } else if (!strcmp(cmd, "feed")) {
    const char *sense = (cargc > 1 && !strcmp(cargv[1], "mark")) ? "BAR-SENSE" : "GAP-SENSE";
    char job[128];
    int n = snprintf(job, sizeof(job), "! 0 200 200 8 1\r\n%s\r\nFORM\r\nPRINT\r\n", sense);
    rc = send_bytes(fd, job, (size_t)n) ? 1 : 0;
  } else if (!strcmp(cmd, "send")) {
    if (cargc < 2) { usage(); close(fd); return 2; }
    FILE *f = !strcmp(cargv[1], "-") ? stdin : fopen(cargv[1], "rb");
    if (!f) { fprintf(stderr, "zicoxctl: %s: %s\n", cargv[1], strerror(errno)); close(fd); return 1; }
    size_t n;
    char *b = read_stream(f, &n);
    if (f != stdin) fclose(f);
    rc = send_bytes(fd, b, n) ? 1 : 0;
    free(b);
  } else if (!strcmp(cmd, "text")) {
    rc = cmd_text(fd, cargc, cargv);
  } else {
    usage();
    close(fd);
    return 2;
  }
  finish(fd);
  return rc;
}
