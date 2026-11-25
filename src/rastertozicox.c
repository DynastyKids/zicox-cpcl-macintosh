/*
 * rastertozicox - CUPS raster filter for Zicox CC4 (CPCL) thermal printers.
 *
 * Converts CUPS raster pages into CPCL "CG" bitmap blocks, following the
 * protocol used by the vendor's Android SDK (zpSDK / _PrinterPageImpl):
 *
 *   ! 0 200 200 <height> 1
 *   PAGE-WIDTH <width>
 *   [CONTRAST n] [SPEED n]
 *   CG <bytesPerRow> <rows> <x> <y> <raw 1-bit data>   (one per band)
 *   [GAP-SENSE | BAR-SENSE LEFT | BAR-SENSE] FORM
 *   [POSTFEED n]
 *   PRINT
 *
 * Optionally each page is wrapped in the vendor's modified GZIP frame
 * (see GZIPFrame.codec in the SDK).
 *
 * Driver options arrive through the raster page header (set by the PPD):
 *   cupsMediaType   0 continuous, 1 gap/label, 2 black mark left, 3 black mark right
 *   cupsInteger[0]  darkness: 0 printer default, 1..4 -> CONTRAST 0..3
 *   cupsInteger[1]  speed:    0 printer default, 1..6 -> SPEED 0..5
 *   cupsInteger[2]  dither:   0 threshold, 1 error diffusion, 2 ordered
 *   cupsInteger[3]  threshold level (1..255), 0 -> 128
 *   cupsInteger[4]  1 = GZIP-compress each page
 *   cupsInteger[5]  1 = rotate page 180 degrees
 *   cupsInteger[6]  1 = trim trailing blank rows (continuous media only)
 *   cupsInteger[7]  feed after print, in dots
 *   cupsInteger[8]  horizontal position: 0 left, 1 centred on the print head
 *   cupsInteger[9]  extra horizontal offset, in dots
 *   cupsInteger[10] 1 = width-only roll page size (set by PageSize): trim the blank tail
 *   cupsInteger[11] print head width in dots (set by each model's PPD); 0 -> 832 (CC4).
 *                   Pages wider than the head are cropped equally on both sides, since
 *                   such paper is centred on the head (e.g. 80 mm paper on a 72 mm head).
 *
 * Build: cc -O2 -o rastertozicox rastertozicox.c -lcups -lz
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <zlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEAD_DOTS   832   /* default: CC4, 104 mm at 8 dots/mm */
#define BAND_ROWS   256   /* max rows per CG block */
#define BLANK_SPLIT 16    /* blank-row run that ends a band */

/* ------------------------------------------------------------------ */
/* Growable output buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned char *data;
  size_t len, cap;
} buf_t;

static void buf_put(buf_t *b, const void *p, size_t n)
{
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 65536;
    while (cap < b->len + n) cap *= 2;
    unsigned char *d = realloc(b->data, cap);
    if (!d) { fputs("ERROR: Out of memory\n", stderr); exit(1); }
    b->data = d; b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
}

static void buf_printf(buf_t *b, const char *fmt, ...)
{
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n > 0) buf_put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static void write_all(const unsigned char *p, size_t n)
{
  while (n > 0) {
    ssize_t w = write(STDOUT_FILENO, p, n);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      fprintf(stderr, "ERROR: Unable to write print data: %s\n", strerror(errno));
      exit(1);
    }
    p += w; n -= (size_t)w;
  }
}

/*
 * Vendor GZIP frame (GZIPFrame.codec): a normal gzip stream, then
 *   bytes 4..7          <- total frame length (little endian)
 *   last 4 bytes        <- CRC32 of bytes [8, len-4)
 */
static void put_le32(unsigned char *p, uint32_t v)
{
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static void write_gzip_frame(const buf_t *in)
{
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    fputs("ERROR: deflateInit2 failed\n", stderr);
    exit(1);
  }
  uLong bound = deflateBound(&zs, (uLong)in->len);
  size_t cap = 10 + bound + 8;
  unsigned char *out = malloc(cap);
  if (!out) { fputs("ERROR: Out of memory\n", stderr); exit(1); }

  static const unsigned char hdr[10] = { 0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0 };
  memcpy(out, hdr, 10);

  zs.next_in = in->data; zs.avail_in = (uInt)in->len;
  zs.next_out = out + 10; zs.avail_out = (uInt)bound;
  if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
    fputs("ERROR: deflate failed\n", stderr);
    exit(1);
  }
  size_t len = 10 + zs.total_out + 8;
  deflateEnd(&zs);

  put_le32(out + len - 8, (uint32_t)crc32(0L, in->data, (uInt)in->len));
  put_le32(out + len - 4, (uint32_t)in->len);   /* ISIZE, overwritten below */

  put_le32(out + 4, (uint32_t)len);
  put_le32(out + len - 4, (uint32_t)crc32(0L, out + 8, (uInt)(len - 8 - 4)));

  write_all(out, len);
  free(out);
}

/* ------------------------------------------------------------------ */
/* Raster -> 1 bit                                                     */
/* ------------------------------------------------------------------ */

static const unsigned char bayer8[8][8] = {
  {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 }
};

/* Convert a raster line to "ink" values 0 (white) .. 255 (black). */
static void line_to_ink(const cups_page_header2_t *h, const unsigned char *src, unsigned char *ink)
{
  unsigned w = h->cupsWidth;
  int inverted = (h->cupsColorSpace == CUPS_CSPACE_W || h->cupsColorSpace == CUPS_CSPACE_SW ||
                  h->cupsColorSpace == CUPS_CSPACE_RGB || h->cupsColorSpace == CUPS_CSPACE_SRGB);

  if (h->cupsBitsPerPixel == 1) {
    for (unsigned x = 0; x < w; x++) {
      int bit = (src[x >> 3] >> (7 - (x & 7))) & 1;
      ink[x] = (bit ^ inverted) ? 255 : 0;
    }
  } else if (h->cupsBitsPerPixel == 8) {
    for (unsigned x = 0; x < w; x++)
      ink[x] = inverted ? 255 - src[x] : src[x];
  } else if (h->cupsBitsPerPixel == 24) {
    for (unsigned x = 0; x < w; x++) {
      const unsigned char *p = src + 3 * x;
      unsigned lum = (p[0] * 30 + p[1] * 59 + p[2] * 11 + 50) / 100;
      ink[x] = 255 - lum;
    }
  } else {
    memset(ink, 0, w);
  }
}

/* ------------------------------------------------------------------ */
/* Page                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned width, height, bpl;   /* bitmap size in dots / bytes per line */
  unsigned char *bits;           /* 1 = black, MSB first */
} page_t;

static int row_blank(const page_t *pg, unsigned y)
{
  const unsigned char *r = pg->bits + (size_t)y * pg->bpl;
  for (unsigned i = 0; i < pg->bpl; i++) if (r[i]) return 0;
  return 1;
}

static void rotate180(page_t *pg)
{
  unsigned char *nb = calloc((size_t)pg->bpl * pg->height, 1);
  if (!nb) { fputs("ERROR: Out of memory\n", stderr); exit(1); }
  for (unsigned y = 0; y < pg->height; y++)
    for (unsigned x = 0; x < pg->width; x++)
      if (pg->bits[(size_t)y * pg->bpl + (x >> 3)] & (0x80 >> (x & 7))) {
        unsigned nx = pg->width - 1 - x, ny = pg->height - 1 - y;
        nb[(size_t)ny * pg->bpl + (nx >> 3)] |= 0x80 >> (nx & 7);
      }
  free(pg->bits);
  pg->bits = nb;
}

/* Emit one CG block for rows [y0, y1), trimmed to the non-blank byte columns. */
static void emit_band(buf_t *out, const page_t *pg, unsigned y0, unsigned y1, unsigned xoff, unsigned ybase)
{
  unsigned c0 = pg->bpl, c1 = 0;
  for (unsigned y = y0; y < y1; y++) {
    const unsigned char *r = pg->bits + (size_t)y * pg->bpl;
    for (unsigned i = 0; i < pg->bpl; i++)
      if (r[i]) { if (i < c0) c0 = i; if (i + 1 > c1) c1 = i + 1; }
  }
  if (c1 <= c0) return;

  unsigned bw = c1 - c0;
  buf_printf(out, "CG %u %u %u %u ", bw, y1 - y0, xoff + c0 * 8, y0 - ybase);
  for (unsigned y = y0; y < y1; y++)
    buf_put(out, pg->bits + (size_t)y * pg->bpl + c0, bw);
  buf_put(out, "\r\n", 2);
}

static unsigned head_dots(const cups_page_header2_t *h)
{
  unsigned d = h->cupsInteger[11];
  return (d >= 64 && d <= 2048) ? d : HEAD_DOTS;
}

static void emit_page(const cups_page_header2_t *h, page_t *pg)
{
  const unsigned head = head_dots(h);
  buf_t out = { 0 };
  unsigned media = h->cupsMediaType;
  unsigned darkness = h->cupsInteger[0];
  unsigned speed = h->cupsInteger[1];
  int gzip = h->cupsInteger[4] == 1;
  int rot = h->cupsInteger[5] == 1;
  int trim = h->cupsInteger[6] == 1 || h->cupsInteger[10] == 1;  /* option, or a width-only roll size */
  unsigned postfeed = h->cupsInteger[7];
  unsigned center = h->cupsInteger[8];
  unsigned xoff = h->cupsInteger[9];

  if (rot) rotate180(pg);

  unsigned height = pg->height;
  if (trim && media == 0) {
    while (height > 1 && row_blank(pg, height - 1)) height--;
    height += 8; /* 1 mm tail */
    if (height > pg->height) height = pg->height;
  }

  /* Width-only roll sizes also drop the blank top (apps may place content anywhere on the
   * 200 mm page), keeping 1 mm; the label is as long as its content. */
  unsigned top = 0;
  if (h->cupsInteger[10] == 1 && media == 0) {
    while (top < height && row_blank(pg, top)) top++;
    if (top >= height) top = 0;
    else top = top > 8 ? top - 8 : 0;
  }

  if (center == 1 && pg->width < head) xoff += (head - pg->width) / 2;
  if (xoff > head - 8) xoff = head - 8;
  unsigned pagew = xoff + pg->bpl * 8;   /* CG blocks are whole bytes wide */
  if (pagew > head) pagew = head;

  buf_printf(&out, "! 0 200 200 %u 1\r\n", height - top);
  buf_printf(&out, "PAGE-WIDTH %u\r\n", pagew);
  if (darkness >= 1 && darkness <= 4) buf_printf(&out, "CONTRAST %u\r\n", darkness - 1);
  if (speed >= 1 && speed <= 6) buf_printf(&out, "SPEED %u\r\n", speed - 1);

  /* Group non-blank rows into bands. */
  unsigned y = top;
  while (y < height) {
    while (y < height && row_blank(pg, y)) y++;
    if (y >= height) break;
    unsigned y0 = y, blank = 0;
    while (y < height && y - y0 < BAND_ROWS) {
      if (row_blank(pg, y)) { if (++blank >= BLANK_SPLIT) break; }
      else blank = 0;
      y++;
    }
    unsigned y1 = y - blank;
    emit_band(&out, pg, y0, y1, xoff, top);
  }

  switch (media) {
    case 1: buf_printf(&out, "GAP-SENSE\r\nFORM\r\n"); break;
    case 2: buf_printf(&out, "BAR-SENSE LEFT\r\nFORM\r\n"); break;
    case 3: buf_printf(&out, "BAR-SENSE\r\nFORM\r\n"); break;
    default: break;
  }
  if (postfeed > 0) buf_printf(&out, "POSTFEED %u\r\n", postfeed);
  buf_printf(&out, "PRINT\r\n");

  if (gzip) write_gzip_frame(&out);
  else write_all(out.data, out.len);

  free(out.data);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t canceled = 0;
static void on_term(int sig) { (void)sig; canceled = 1; }

int main(int argc, char *argv[])
{
  if (argc < 6 || argc > 7) {
    fputs("Usage: rastertozicox job-id user title copies options [file]\n", stderr);
    return 1;
  }

  int fd = 0;
  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
    fprintf(stderr, "ERROR: Unable to open raster file: %s\n", strerror(errno));
    return 1;
  }

  signal(SIGTERM, on_term);

  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) { fputs("ERROR: Unable to read raster data\n", stderr); return 1; }

  cups_page_header2_t h;
  int page = 0;

  while (!canceled && cupsRasterReadHeader2(ras, &h)) {
    page++;
    fprintf(stderr, "PAGE: %d 1\n", page);
    fprintf(stderr, "INFO: Printing page %d (%ux%u, %u bpp, cs %u)\n", page,
            h.cupsWidth, h.cupsHeight, h.cupsBitsPerPixel, h.cupsColorSpace);
    fprintf(stderr, "DEBUG: media=%u int=[%u %u %u %u %u %u %u %u %u %u %u]\n", h.cupsMediaType,
            h.cupsInteger[0], h.cupsInteger[1], h.cupsInteger[2], h.cupsInteger[3], h.cupsInteger[4],
            h.cupsInteger[5], h.cupsInteger[6], h.cupsInteger[7], h.cupsInteger[8], h.cupsInteger[9],
            h.cupsInteger[10]);

    if (h.cupsBitsPerPixel != 1 && h.cupsBitsPerPixel != 8 && h.cupsBitsPerPixel != 24) {
      fprintf(stderr, "ERROR: Unsupported raster format (%u bits per pixel)\n", h.cupsBitsPerPixel);
      cupsRasterClose(ras);
      return 1;
    }

    page_t pg;
    unsigned head = head_dots(&h);
    pg.width = h.cupsWidth > head ? head : h.cupsWidth;
    unsigned xskip = h.cupsWidth > head ? (h.cupsWidth - head) / 2 : 0;  /* centre crop */
    pg.height = h.cupsHeight;
    pg.bpl = (pg.width + 7) / 8;
    pg.bits = calloc((size_t)pg.bpl * pg.height, 1);

    unsigned char *line = malloc(h.cupsBytesPerLine);
    unsigned char *ink = malloc(h.cupsWidth + 2);
    int *err_cur = calloc(h.cupsWidth + 2, sizeof(int));
    int *err_nxt = calloc(h.cupsWidth + 2, sizeof(int));
    if (!pg.bits || !line || !ink || !err_cur || !err_nxt) {
      fputs("ERROR: Out of memory\n", stderr);
      return 1;
    }

    unsigned dither = h.cupsInteger[2];
    int threshold = h.cupsInteger[3] ? (int)h.cupsInteger[3] : 128;
    if (threshold < 1 || threshold > 255) threshold = 128;

    for (unsigned y = 0; y < h.cupsHeight && !canceled; y++) {
      if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < 1) break;
      line_to_ink(&h, line, ink);
      if (xskip) memmove(ink, ink + xskip, pg.width);
      unsigned char *row = pg.bits + (size_t)y * pg.bpl;

      if (dither == 1) {
        /* Floyd-Steinberg, serpentine. err arrays are offset by 1. */
        int ltr = !(y & 1);
        memset(err_nxt, 0, (h.cupsWidth + 2) * sizeof(int));
        for (unsigned i = 0; i < pg.width; i++) {
          unsigned x = ltr ? i : pg.width - 1 - i;
          int v = ink[x] + err_cur[x + 1] / 16;
          int out = v >= threshold ? 255 : 0;
          int e = v - out;
          if (out) row[x >> 3] |= 0x80 >> (x & 7);
          int d = ltr ? 1 : -1;
          err_cur[x + 1 + d] += e * 7;
          err_nxt[x + 1 - d] += e * 3;
          err_nxt[x + 1] += e * 5;
          err_nxt[x + 1 + d] += e * 1;
        }
        int *t = err_cur; err_cur = err_nxt; err_nxt = t;
      } else if (dither == 2) {
        for (unsigned x = 0; x < pg.width; x++) {
          int t = (bayer8[y & 7][x & 7] * 4 + 2) + (threshold - 128);
          if (ink[x] > t) row[x >> 3] |= 0x80 >> (x & 7);
        }
      } else {
        for (unsigned x = 0; x < pg.width; x++)
          if (ink[x] >= threshold) row[x >> 3] |= 0x80 >> (x & 7);
      }
    }

    if (!canceled) emit_page(&h, &pg);

    free(pg.bits); free(line); free(ink); free(err_cur); free(err_nxt);
  }

  cupsRasterClose(ras);
  if (fd) close(fd);

  if (page == 0) {
    fputs("ERROR: No pages found\n", stderr);
    return 1;
  }
  fputs("INFO: Ready to print.\n", stderr);
  return canceled ? 1 : 0;
}
