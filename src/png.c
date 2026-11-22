/* Minimal PNG writer for screenshots: 24-bit RGB, zlib stream made with LZ77 + the fixed
 * Huffman codes (pixel art compresses very well that way). No external libraries. */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Out {
  uint8_t *p;
  size_t n, cap;
  uint32_t bits;
  int nbits;
} Out;

static int out_grow(Out *o, size_t more) {
  if (o->n + more <= o->cap) return 1;
  size_t c = o->cap ? o->cap * 2 : 65536;
  while (c < o->n + more) c *= 2;
  uint8_t *q = (uint8_t *)realloc(o->p, c);
  if (!q) return 0;
  o->p = q;
  o->cap = c;
  return 1;
}

static void out_byte(Out *o, uint8_t b) {
  if (out_grow(o, 1)) o->p[o->n++] = b;
}

/* deflate bit order: values LSB first */
static void put_bits(Out *o, uint32_t v, int n) {
  o->bits |= v << o->nbits;
  o->nbits += n;
  while (o->nbits >= 8) {
    out_byte(o, (uint8_t)o->bits);
    o->bits >>= 8;
    o->nbits -= 8;
  }
}

/* Huffman codes go MSB first */
static void put_code(Out *o, uint32_t code, int n) {
  uint32_t r = 0;
  for (int i = 0; i < n; i++) r |= ((code >> i) & 1u) << (n - 1 - i);
  put_bits(o, r, n);
}

static void put_lit(Out *o, int c) {
  if (c < 144) put_code(o, 0x30u + (uint32_t)c, 8);
  else if (c < 256) put_code(o, 0x190u + (uint32_t)(c - 144), 9);
  else if (c < 280) put_code(o, (uint32_t)(c - 256), 7);
  else put_code(o, 0xC0u + (uint32_t)(c - 280), 8);
}

static const uint16_t k_lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t k_dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(Out *o, int len, int dist) {
  int li = 28;
  while (k_lbase[li] > len) li--;
  put_lit(o, 257 + li);
  if (k_lext[li]) put_bits(o, (uint32_t)(len - k_lbase[li]), k_lext[li]);
  int di = 29;
  while (k_dbase[di] > dist) di--;
  put_code(o, (uint32_t)di, 5);
  if (k_dext[di]) put_bits(o, (uint32_t)(dist - k_dbase[di]), k_dext[di]);
}

#define HBITS 15
#define WIN 32768
#define MAXCHAIN 48

/* one fixed-Huffman block holding the whole input */
static void deflate_fixed(Out *o, const uint8_t *d, size_t n) {
  int32_t *head = (int32_t *)malloc(sizeof(int32_t) << HBITS);
  int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * WIN);
  if (!head || !prev) {
    free(head);
    free(prev);
    /* stored blocks as a fallback */
    put_bits(o, 0, 0);
    size_t at = 0;
    do {
      size_t len = n - at > 65535 ? 65535 : n - at;
      put_bits(o, at + len >= n ? 1 : 0, 1);
      put_bits(o, 0, 2);
      if (o->nbits) put_bits(o, 0, 8 - o->nbits);
      out_byte(o, (uint8_t)len);
      out_byte(o, (uint8_t)(len >> 8));
      out_byte(o, (uint8_t)~len);
      out_byte(o, (uint8_t)(~len >> 8));
      for (size_t i = 0; i < len; i++) out_byte(o, d[at + i]);
      at += len;
    } while (at < n);
    return;
  }
  for (int i = 0; i < (1 << HBITS); i++) head[i] = -1;
  put_bits(o, 1, 1); /* final */
  put_bits(o, 1, 2); /* fixed Huffman */
  size_t i = 0;
  while (i < n) {
    int best = 0, bdist = 0;
    if (i + 3 <= n) {
      uint32_t h = ((uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2]) * 2654435761u >> (32 - HBITS);
      int32_t cand = head[h];
      int chain = MAXCHAIN;
      size_t maxlen = n - i < 258 ? n - i : 258;
      while (cand >= 0 && i - (size_t)cand <= WIN - 1 && chain--) {
        const uint8_t *a = d + cand, *b = d + i;
        size_t l = 0;
        while (l < maxlen && a[l] == b[l]) l++;
        if ((int)l > best) {
          best = (int)l;
          bdist = (int)(i - (size_t)cand);
          if (l == maxlen) break;
        }
        cand = prev[cand & (WIN - 1)];
      }
      /* insert the positions this step covers */
      size_t adv = best >= 3 ? (size_t)best : 1;
      for (size_t k = 0; k < adv && i + k + 3 <= n; k++) {
        uint32_t hk =
            ((uint32_t)d[i + k] << 16 | (uint32_t)d[i + k + 1] << 8 | d[i + k + 2]) * 2654435761u >> (32 - HBITS);
        prev[(i + k) & (WIN - 1)] = head[hk];
        head[hk] = (int32_t)(i + k);
      }
    }
    if (best >= 3) {
      put_match(o, best, bdist);
      i += (size_t)best;
    } else {
      put_lit(o, d[i]);
      i++;
    }
  }
  put_lit(o, 256);
  if (o->nbits) put_bits(o, 0, 8 - o->nbits);
  free(head);
  free(prev);
}

static uint32_t crc_table[256];
static void crc_init(void) {
  if (crc_table[1]) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t c) {
  c = ~c;
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}

static void be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
  uint8_t h[8];
  be32(h, len);
  memcpy(h + 4, type, 4);
  uint32_t c = crc32_of(h + 4, 4, 0);
  c = crc32_of(data, len, c);
  uint8_t t[4];
  be32(t, c);
  return fwrite(h, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(t, 1, 4, f) == 4;
}

int png_write_bgra(const char *path, const uint8_t *bgra, int w, int h, int stride, int bottom_up) {
  crc_init();
  size_t rowb = (size_t)w * 3 + 1;
  uint8_t *raw = (uint8_t *)malloc(rowb * (size_t)h);
  if (!raw) return 0;
  for (int y = 0; y < h; y++) {
    const uint8_t *src = bgra + (size_t)(bottom_up ? h - 1 - y : y) * (size_t)stride;
    uint8_t *dst = raw + rowb * (size_t)y;
    dst[0] = 0; /* filter: none */
    for (int x = 0; x < w; x++) {
      dst[1 + x * 3] = src[x * 4 + 2];
      dst[2 + x * 3] = src[x * 4 + 1];
      dst[3 + x * 3] = src[x * 4 + 0];
    }
  }
  Out z = {0};
  out_byte(&z, 0x78);
  out_byte(&z, 0x01);
  deflate_fixed(&z, raw, rowb * (size_t)h);
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < rowb * (size_t)h; i++) {
    a = (a + raw[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  free(raw);
  uint8_t ad[4];
  be32(ad, (b << 16) | a);
  for (int i = 0; i < 4; i++) out_byte(&z, ad[i]);
  int ok = 0;
  FILE *f = fopen(path, "wb");
  if (f && z.p) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z.p, (uint32_t)z.n) &&
         chunk(f, "IEND", NULL, 0);
  }
  if (f) fclose(f);
  free(z.p);
  return ok;
}
