// Minimal single-frame GIF decoder for the kmoni map layers.
// Host-testable: no Arduino dependencies.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Pixels are 0xAARRGGBB. Alpha is 0 or 255 coming out of a GIF.
// `work` is caller-owned scratch of width*height bytes. `dst` is only touched
// once the whole frame has decoded, so a corrupt download leaves it intact.
inline bool gifDrawOver(const uint8_t *data, size_t len, uint32_t *dst, int dstW, int dstH,
                        uint8_t *work) {
  if (len < 13 || memcmp(data, "GIF8", 4) != 0) return false;

  const uint8_t *palette = nullptr;
  size_t pos = 13;
  const uint8_t flags = data[10];
  if (flags & 0x80) {
    palette = data + pos;
    pos += 3u << ((flags & 7) + 1);
  }

  int transparent = -1;
  while (pos < len) {
    const uint8_t tag = data[pos++];
    if (tag == 0x21) {  // extension
      if (pos + 1 > len) return false;
      const uint8_t label = data[pos++];
      if (label == 0xF9 && pos + 5 <= len && (data[pos + 1] & 1)) transparent = data[pos + 4];
      while (pos < len && data[pos]) pos += data[pos] + 1u;
      pos++;
    } else if (tag == 0x2C) {
      break;  // image descriptor
    } else {
      return false;  // trailer or garbage before any frame
    }
  }
  if (pos + 9 > len) return false;

  const int fx = data[pos] | data[pos + 1] << 8;
  const int fy = data[pos + 2] | data[pos + 3] << 8;
  const int fw = data[pos + 4] | data[pos + 5] << 8;
  const int fh = data[pos + 6] | data[pos + 7] << 8;
  const uint8_t imgFlags = data[pos + 8];
  pos += 9;
  if (fw <= 0 || fh <= 0 || fx + fw > dstW || fy + fh > dstH) return false;
  if (imgFlags & 0x80) {
    palette = data + pos;
    pos += 3u << ((imgFlags & 7) + 1);
  }
  if (!palette || pos >= len) return false;

  // LZW
  const int minCode = data[pos++];
  if (minCode < 2 || minCode > 8) return false;
  static uint16_t prefix[4096];
  static uint8_t suffix[4096];
  static uint8_t stack[4097];
  const int clear = 1 << minCode;
  const int eoi = clear + 1;
  int next = eoi + 1, codeSize = minCode + 1, prev = -1;
  uint8_t first = 0;
  uint32_t bits = 0;
  int bitCount = 0;
  const size_t total = (size_t)fw * fh;
  size_t out = 0;
  size_t block = 0;
  bool done = false;

  while (!done && out < total) {
    if (block == 0) {
      if (pos >= len) return false;
      block = data[pos++];
      if (block == 0) break;
    }
    if (pos >= len) return false;
    bits |= (uint32_t)data[pos++] << bitCount;
    bitCount += 8;
    block--;

    while (bitCount >= codeSize) {
      int code = bits & ((1u << codeSize) - 1);
      bits >>= codeSize;
      bitCount -= codeSize;
      if (code == clear) {
        next = eoi + 1;
        codeSize = minCode + 1;
        prev = -1;
        continue;
      }
      if (code == eoi) {
        done = true;
        break;
      }
      if (prev < 0) {
        if (code >= clear) return false;
        first = (uint8_t)code;
        work[out++] = first;
        prev = code;
        if (out >= total) break;
        continue;
      }
      if (code > next || (code == next && next >= 4096)) return false;
      int sp = 0;
      int cur = code;
      if (code == next) {
        stack[sp++] = first;
        cur = prev;
      }
      while (cur >= clear) {
        stack[sp++] = suffix[cur];
        cur = prefix[cur];
      }
      first = (uint8_t)cur;
      stack[sp++] = first;
      if (next < 4096) {
        prefix[next] = (uint16_t)prev;
        suffix[next] = first;
        next++;
        if (next == (1 << codeSize) && codeSize < 12) codeSize++;
      }
      while (sp && out < total) work[out++] = stack[--sp];
      prev = code;
      if (out >= total) break;
    }
  }
  if (out < total) return false;

  // Interlaced rows arrive in four passes; these layers are progressive today.
  const bool interlaced = imgFlags & 0x40;
  static const uint8_t passStart[4] = {0, 4, 2, 1};
  static const uint8_t passStep[4] = {8, 8, 4, 2};
  int pass = 0, y = 0;
  for (int row = 0; row < fh; row++) {
    const uint8_t *src = work + (size_t)row * fw;
    uint32_t *d = dst + (size_t)(fy + y) * dstW + fx;
    for (int x = 0; x < fw; x++) {
      const uint8_t idx = src[x];
      if (idx == transparent) continue;
      const uint8_t *c = palette + idx * 3u;
      d[x] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    }
    if (!interlaced) {
      y++;
    } else {
      y += passStep[pass];
      while (y >= fh && pass < 3) y = passStart[++pass];
    }
  }
  return true;
}
