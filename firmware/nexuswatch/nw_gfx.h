// NexusWatch - graficos por franjas para una pantalla de 240x240 (GC9A01 redonda).
// Se dibuja la escena franja a franja (240 x NW_STRIP lineas) para no necesitar 115 KB de RAM: cada primitiva
// se recorta a la franja actual. Mismo codigo en el ESP32 y en el emulador del PC.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "nw_assets.h"

#ifndef NW_STRIP
#define NW_STRIP 40
#endif
#define NW_W 240
#define NW_H 240

namespace nw {

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t hex(const char* s, uint16_t def = 0x07FF) {
  if (!s) return def;
  if (*s == '#') s++;
  unsigned v = 0;
  int n = 0;
  for (; *s && n < 6; s++, n++) {
    char c = *s;
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return def;
  }
  if (n != 6) return def;
  return rgb((v >> 16) & 255, (v >> 8) & 255, v & 255);
}
static inline uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {  // t=0 -> a, 255 -> b
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar * (255 - t) + br * t) / 255) << 11) | (((ag * (255 - t) + bg * t) / 255) << 5) |
                    ((ab * (255 - t) + bb * t) / 255));
}
static inline uint16_t dim(uint16_t c, uint8_t k) { return mix(0, c, k); }

static const uint16_t BLACK = 0x0000, WHITE = 0xFFFF;
static const uint16_t BG = 0x0841;      // casi negro azulado
static const uint16_t MUTED = 0x6C51;   // gris azulado

struct Canvas {
  uint16_t buf[NW_W * NW_STRIP];
  int y0 = 0, h = NW_STRIP;

  void begin(int top) {
    y0 = top;
    h = (top + NW_STRIP > NW_H) ? NW_H - top : NW_STRIP;
  }
  inline void px(int x, int y, uint16_t c) {
    if ((unsigned)x < NW_W && y >= y0 && y < y0 + h) buf[(y - y0) * NW_W + x] = c;
  }
  inline uint16_t get(int x, int y) const {
    if ((unsigned)x < NW_W && y >= y0 && y < y0 + h) return buf[(y - y0) * NW_W + x];
    return 0;
  }
  inline void pxa(int x, int y, uint16_t c, uint8_t a) {  // con transparencia
    if ((unsigned)x < NW_W && y >= y0 && y < y0 + h) {
      uint16_t& d = buf[(y - y0) * NW_W + x];
      d = mix(d, c, a);
    }
  }
  void clear(uint16_t c) {
    for (int i = 0; i < NW_W * h; i++) buf[i] = c;
  }
  void fillRect(int x, int y, int w, int hh, uint16_t c) {
    int ya = y < y0 ? y0 : y, yb = (y + hh > y0 + h) ? y0 + h : y + hh;
    int xa = x < 0 ? 0 : x, xb = (x + w > NW_W) ? NW_W : x + w;
    for (int yy = ya; yy < yb; yy++)
      for (int xx = xa; xx < xb; xx++) buf[(yy - y0) * NW_W + xx] = c;
  }
  void vgradient(uint16_t top, uint16_t bottom) {
    for (int yy = y0; yy < y0 + h; yy++) {
      uint16_t c = mix(top, bottom, (uint8_t)(yy * 255 / (NW_H - 1)));
      for (int xx = 0; xx < NW_W; xx++) buf[(yy - y0) * NW_W + xx] = c;
    }
  }
  // circulo relleno con borde suavizado
  void fillCircle(float cx, float cy, float r, uint16_t c, uint8_t alpha = 255) {
    int ya = (int)floorf(cy - r - 1), yb = (int)ceilf(cy + r + 1);
    if (ya < y0) ya = y0;
    if (yb > y0 + h) yb = y0 + h;
    for (int yy = ya; yy < yb; yy++) {
      float dy = yy + 0.5f - cy;
      float span = r * r - dy * dy;
      if (span < -2 * r) continue;
      float hw = span > 0 ? sqrtf(span) : 0;
      int xa = (int)floorf(cx - hw - 1), xb = (int)ceilf(cx + hw + 1);
      for (int xx = xa; xx <= xb; xx++) {
        float dx = xx + 0.5f - cx;
        float d = sqrtf(dx * dx + dy * dy);
        float cov = r + 0.5f - d;
        if (cov <= 0) continue;
        uint8_t a = cov >= 1 ? alpha : (uint8_t)(cov * alpha);
        pxa(xx, yy, c, a);
      }
    }
  }
  // anillo (r0..r1) entre los angulos a0..a1 (radianes, 0 = arriba, sentido horario)
  void ring(float cx, float cy, float r0, float r1, uint16_t c, float a0 = 0, float a1 = 6.2832f, uint8_t alpha = 255) {
    int ya = (int)(cy - r1 - 1), yb = (int)(cy + r1 + 2);
    if (ya < y0) ya = y0;
    if (yb > y0 + h) yb = y0 + h;
    bool full = a1 - a0 >= 6.28f;
    for (int yy = ya; yy < yb; yy++) {
      float dy = yy + 0.5f - cy;
      for (int xx = (int)(cx - r1 - 1); xx <= (int)(cx + r1 + 1); xx++) {
        float dx = xx + 0.5f - cx;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < r0 - 1 || d > r1 + 1) continue;
        if (!full) {
          float ang = atan2f(dx, -dy);
          if (ang < 0) ang += 6.2832f;
          float aa = fmodf(a0, 6.2832f);
          if (aa < 0) aa += 6.2832f;
          float rel = ang - aa;
          if (rel < 0) rel += 6.2832f;
          if (rel > a1 - a0) continue;
        }
        float cov = d < r0 ? 1 - (r0 - d) : d > r1 ? 1 - (d - r1) : 1;
        if (cov <= 0) continue;
        pxa(xx, yy, c, (uint8_t)(cov * alpha));
      }
    }
  }
  void line(float x0, float y0_, float x1, float y1, float w, uint16_t c, uint8_t alpha = 255) {
    float dx = x1 - x0, dy = y1 - y0_;
    float len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len / (w * 0.5f + 0.5f)) + 1;
    for (int i = 0; i <= n; i++) fillCircle(x0 + dx * i / n, y0_ + dy * i / n, w / 2, c, alpha);
  }
  void avatar(int x, int y, uint8_t bright = 255) {
    int ya = y < y0 ? y0 : y, yb = (y + NW_AVATAR > y0 + h) ? y0 + h : y + NW_AVATAR;
    for (int yy = ya; yy < yb; yy++) {
      const uint16_t* row = NW_AVATAR_PX + (yy - y) * NW_AVATAR;
      for (int xx = 0; xx < NW_AVATAR; xx++) {
#ifdef ARDUINO
        uint16_t c = pgm_read_word(row + xx);
#else
        uint16_t c = row[xx];
#endif
        if (c == 0x0001) continue;
        px(x + xx, yy, bright == 255 ? c : dim(c, bright));
      }
    }
  }
  // ---------------------------------------------------------------- texto (Latin-1, UTF-8 de entrada)
  static int nextCp(const char*& s) {
    unsigned char c = (unsigned char)*s;
    if (!c) return 0;
    if (c < 0x80) { s++; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
      int cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
      s += 2;
      return cp;
    }
    // caracteres fuera de Latin-1: se saltan los bytes de continuacion
    s++;
    while ((*s & 0xC0) == 0x80) s++;
    return '?';
  }
  static int glyphW(int cp) {
    if (cp < 0x20 || cp > 0xFF) cp = '?';
    return NW_FONT_IDX[cp - 0x20][1];
  }
  static int textW(const char* s, int scale = 1) {
    int w = 0;
    while (int cp = nextCp(s)) w += glyphW(cp) * scale;
    return w;
  }
  int glyph(int x, int y, int cp, uint16_t c, int scale) {
    if (cp < 0x20 || cp > 0xFF) cp = '?';
    const uint16_t* gi = NW_FONT_IDX[cp - 0x20];
    int off = gi[0], w = gi[1], bpr = (w + 7) / 8;
    if (y + NW_FONT_H * scale < y0 || y > y0 + h) return w * scale;
    for (int gy = 0; gy < NW_FONT_H; gy++)
      for (int gx = 0; gx < w; gx++) {
#ifdef ARDUINO
        uint8_t b = pgm_read_byte(NW_FONT + off + gy * bpr + gx / 8);
#else
        uint8_t b = NW_FONT[off + gy * bpr + gx / 8];
#endif
        if (b & (0x80 >> (gx & 7))) {
          if (scale == 1) px(x + gx, y + gy, c);
          else fillRect(x + gx * scale, y + gy * scale, scale, scale, c);
        }
      }
    return w * scale;
  }
  // align: 0 izquierda, 1 centro, 2 derecha. maxw > 0 recorta con "…"
  void text(int x, int y, const char* s, uint16_t c, int scale = 1, int align = 0, int maxw = 0) {
    char tmp[96];
    if (maxw > 0 && textW(s, scale) > maxw) {
      size_t n = 0;
      const char* p = s;
      int w = textW("..", scale);
      while (*p && n < sizeof(tmp) - 4) {
        const char* q = p;
        int cp = nextCp(q);
        if (w + glyphW(cp) * scale > maxw) break;
        w += glyphW(cp) * scale;
        while (p < q && n < sizeof(tmp) - 4) tmp[n++] = *p++;
      }
      tmp[n++] = '.';
      tmp[n++] = '.';
      tmp[n] = 0;
      s = tmp;
    }
    int w = textW(s, scale);
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    while (int cp = nextCp(s)) x += glyph(x, y, cp, c, scale);
  }
};

}  // namespace nw
