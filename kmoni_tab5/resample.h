// Area-weighted upscaling and alpha blitting. Host-testable: no Arduino dependencies.
// The map scales by a non-integer 2.045x. Plain nearest-neighbour would make some
// station squares and border lines visibly wider than others; weighting each output
// pixel by its coverage keeps them even while staying sharp.
#pragma once
#include <stdint.h>

struct ResampleAxis {
  uint16_t i0;
  uint16_t i1;
  uint16_t w1;  // weight of i1, 0..256
};

// Requires dstLen >= srcLen (upscale only): each output pixel straddles at most one edge.
inline void resampleAxis(ResampleAxis *axis, int srcLen, int dstLen) {
  for (int d = 0; d < dstLen; d++) {
    const int i0 = (int)((int64_t)d * srcLen / dstLen);
    const int64_t edge = (int64_t)(i0 + 1) * dstLen;  // in units of 1/srcLen output pixels
    const int64_t end = (int64_t)(d + 1) * srcLen;
    axis[d].i0 = i0;
    if (end <= edge || i0 + 1 >= srcLen) {
      axis[d].i1 = i0;
      axis[d].w1 = 0;
    } else {
      axis[d].i1 = i0 + 1;
      axis[d].w1 = (uint16_t)((end - edge) * 256 / srcLen);
    }
  }
}

// Blends in premultiplied space so transparent neighbours don't darken edges.
inline uint32_t resampleMix(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, uint32_t wx,
                            uint32_t wy) {
  const uint32_t w[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy};
  const uint32_t p[4] = {p00, p10, p01, p11};
  uint64_t a = 0, r = 0, g = 0, b = 0;
  for (int i = 0; i < 4; i++) {
    const uint64_t wa = (uint64_t)w[i] * (p[i] >> 24);
    a += wa;
    r += wa * ((p[i] >> 16) & 0xFF);
    g += wa * ((p[i] >> 8) & 0xFF);
    b += wa * (p[i] & 0xFF);
  }
  if (a == 0) return 0;
  const uint32_t outA = (uint32_t)((a + 32768) >> 16);
  return outA << 24 | (uint32_t)((r + a / 2) / a) << 16 | (uint32_t)((g + a / 2) / a) << 8 |
         (uint32_t)((b + a / 2) / a);
}

inline void resampleRgba(const uint32_t *src, int srcW, const ResampleAxis *ax, int dstW,
                         const ResampleAxis *ay, int dstH, uint32_t *dst) {
  for (int y = 0; y < dstH; y++) {
    const uint32_t *r0 = src + (size_t)ay[y].i0 * srcW;
    const uint32_t *r1 = src + (size_t)ay[y].i1 * srcW;
    const uint32_t wy = ay[y].w1;
    for (int x = 0; x < dstW; x++) {
      const ResampleAxis &a = ax[x];
      const uint32_t p00 = r0[a.i0], p10 = r0[a.i1], p01 = r1[a.i0], p11 = r1[a.i1];
      if (p00 == p10 && p00 == p01 && p00 == p11) {
        *dst++ = p00;
      } else {
        *dst++ = resampleMix(p00, p10, p01, p11, a.w1, wy);
      }
    }
  }
}

// Draws ARGB over a byte-swapped RGB565 framebuffer (M5Canvas's native format).
inline void blitOverSwap565(uint16_t *fb, int fbW, int fbH, int dx, int dy, const uint32_t *src,
                            int w, int h) {
  for (int y = 0; y < h; y++) {
    const int py = dy + y;
    if (py < 0 || py >= fbH) {
      src += w;
      continue;
    }
    uint16_t *d = fb + (size_t)py * fbW + dx;
    for (int x = 0; x < w; x++) {
      const uint32_t p = *src++;
      const uint32_t a = p >> 24;
      if (a == 0) continue;
      uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
      if (a != 255) {
        const uint16_t cur = (uint16_t)(d[x] << 8 | d[x] >> 8);
        const uint32_t cr = (cur >> 8) & 0xF8, cg = (cur >> 3) & 0xFC, cb = (cur << 3) & 0xF8;
        r = (r * a + cr * (255 - a)) / 255;
        g = (g * a + cg * (255 - a)) / 255;
        b = (b * a + cb * (255 - a)) / 255;
      }
      const uint16_t c = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
      d[x] = (uint16_t)(c << 8 | c >> 8);
    }
  }
}
