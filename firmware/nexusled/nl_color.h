// NexusLED - color: RGB, HSV, mezclas, paletas y correccion gamma.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <string>

namespace nl {

struct RGB {
  uint8_t r = 0, g = 0, b = 0;
  RGB() {}
  RGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
  bool operator==(const RGB& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const RGB& o) const { return !(*this == o); }
};

inline uint8_t qadd(int a, int b) { int v = a + b; return v > 255 ? 255 : (uint8_t)v; }
inline uint8_t scale8(uint8_t v, uint8_t s) { return (uint8_t)(((uint16_t)v * (uint16_t)(s + 1)) >> 8); }
inline RGB scale(RGB c, uint8_t s) { return RGB(scale8(c.r, s), scale8(c.g, s), scale8(c.b, s)); }
inline RGB addc(RGB a, RGB b) { return RGB(qadd(a.r, b.r), qadd(a.g, b.g), qadd(a.b, b.b)); }
// mezcla: t=0 -> a, t=255 -> b
inline RGB blend(RGB a, RGB b, uint8_t t) {
  return RGB((uint8_t)(a.r + (((int)b.r - a.r) * t) / 255), (uint8_t)(a.g + (((int)b.g - a.g) * t) / 255),
             (uint8_t)(a.b + (((int)b.b - a.b) * t) / 255));
}

// HSV con matiz 0-255 (rueda de color "arcoiris")
inline RGB hsv(uint8_t h, uint8_t s = 255, uint8_t v = 255) {
  uint8_t region = h / 43;
  uint8_t rem = (h - region * 43) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
  switch (region) {
    case 0: return RGB(v, t, p);
    case 1: return RGB(q, v, p);
    case 2: return RGB(p, v, t);
    case 3: return RGB(p, q, v);
    case 4: return RGB(t, p, v);
    default: return RGB(v, p, q);
  }
}

// seno 0-255 de un angulo 0-255
inline uint8_t sin8(uint8_t x) {
  static const uint8_t T[65] = {128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 162, 165, 167, 170, 173, 176,
                                179, 182, 185, 188, 190, 193, 196, 198, 201, 203, 206, 208, 211, 213, 215, 218, 220,
                                222, 224, 226, 228, 230, 232, 234, 235, 237, 238, 240, 241, 243, 244, 245, 246, 248,
                                249, 250, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255, 255, 255};
  uint8_t q = x >> 6, i = x & 63;
  switch (q) {
    case 0: return T[i];
    case 1: return T[64 - i];
    case 2: return (uint8_t)(256 - T[i]);
    default: return (uint8_t)(256 - T[64 - i]);
  }
}
inline uint8_t cos8(uint8_t x) { return sin8((uint8_t)(x + 64)); }
// triangulo 0-255-0
inline uint8_t tri8(uint8_t x) { return x < 128 ? (uint8_t)(x * 2) : (uint8_t)((255 - x) * 2); }

inline uint8_t gamma8(uint8_t v) {
  // gamma 2.2: tabla calculada una vez
  static uint8_t T[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; i++) {
      float x = powf(i / 255.0f, 2.2f) * 255.0f + 0.5f;
      T[i] = (uint8_t)(i && x < 1 ? 1 : x);
    }
    ready = true;
  }
  return T[v];
}

inline int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline bool parseHex(const std::string& s, RGB& out) {
  const char* p = s.c_str();
  if (*p == '#') p++;
  size_t n = strlen(p);
  if (n != 6 && n != 3) return false;
  int v[6];
  for (size_t i = 0; i < n; i++)
    if ((v[i] = hexv(p[i])) < 0) return false;
  if (n == 3)
    out = RGB(v[0] * 17, v[1] * 17, v[2] * 17);
  else
    out = RGB(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
  return true;
}
inline std::string toHex(RGB c) {
  static const char* H = "0123456789abcdef";
  std::string s = "#";
  uint8_t b[3] = {c.r, c.g, c.b};
  for (int i = 0; i < 3; i++) {
    s += H[b[i] >> 4];
    s += H[b[i] & 15];
  }
  return s;
}

// ---------------------------------------------------------------- paletas
// Cada paleta es una lista de paradas (posicion 0-255, color). La 0 usa los colores del segmento.
struct Stop { uint8_t pos, r, g, b; };
struct PaletteDef { const char* key; const char* name; const Stop* stops; uint8_t n; };

static const Stop P_RAINBOW[] = {{0, 255, 0, 0}, {42, 255, 255, 0}, {85, 0, 255, 0}, {128, 0, 255, 255},
                                 {170, 0, 0, 255}, {213, 255, 0, 255}, {255, 255, 0, 0}};
static const Stop P_FIRE[] = {{0, 0, 0, 0}, {80, 160, 0, 0}, {150, 255, 80, 0}, {210, 255, 190, 20}, {255, 255, 255, 160}};
static const Stop P_OCEAN[] = {{0, 0, 10, 40}, {80, 0, 60, 140}, {150, 0, 150, 200}, {210, 60, 220, 255}, {255, 0, 10, 40}};
static const Stop P_FOREST[] = {{0, 0, 40, 0}, {90, 30, 120, 10}, {160, 110, 170, 20}, {220, 20, 80, 10}, {255, 0, 40, 0}};
static const Stop P_PARTY[] = {{0, 90, 0, 255}, {64, 255, 0, 120}, {128, 255, 120, 0}, {192, 0, 200, 255}, {255, 90, 0, 255}};
static const Stop P_LAVA[] = {{0, 20, 0, 0}, {90, 180, 0, 0}, {160, 255, 60, 0}, {220, 255, 0, 40}, {255, 20, 0, 0}};
static const Stop P_NEON[] = {{0, 0, 240, 255}, {85, 255, 61, 242}, {170, 124, 255, 107}, {255, 0, 240, 255}};
static const Stop P_SUNSET[] = {{0, 255, 120, 0}, {90, 255, 30, 60}, {170, 120, 0, 140}, {230, 30, 0, 80}, {255, 255, 120, 0}};
static const Stop P_ICE[] = {{0, 0, 0, 40}, {100, 0, 80, 200}, {180, 120, 200, 255}, {255, 255, 255, 255}};
static const Stop P_AURORA[] = {{0, 0, 30, 20}, {70, 0, 200, 90}, {140, 0, 120, 200}, {200, 140, 0, 200}, {255, 0, 30, 20}};

static const PaletteDef PALETTES[] = {
    {"colors", "Colores del segmento", nullptr, 0},
    {"rainbow", "Arcoíris", P_RAINBOW, 7},
    {"fire", "Fuego", P_FIRE, 5},
    {"ocean", "Océano", P_OCEAN, 5},
    {"forest", "Bosque", P_FOREST, 5},
    {"party", "Fiesta", P_PARTY, 5},
    {"lava", "Lava", P_LAVA, 5},
    {"neon", "Neón NEXUS", P_NEON, 4},
    {"sunset", "Atardecer", P_SUNSET, 5},
    {"ice", "Hielo", P_ICE, 4},
    {"aurora", "Aurora", P_AURORA, 5},
};
static const int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);

inline RGB paletteAt(const Stop* st, int n, uint8_t pos) {
  if (n <= 0) return RGB();
  if (pos <= st[0].pos) return RGB(st[0].r, st[0].g, st[0].b);
  for (int i = 1; i < n; i++) {
    if (pos <= st[i].pos) {
      const Stop& a = st[i - 1];
      const Stop& b = st[i];
      uint8_t t = (uint8_t)(((int)(pos - a.pos) * 255) / ((b.pos - a.pos) ? (b.pos - a.pos) : 1));
      return blend(RGB(a.r, a.g, a.b), RGB(b.r, b.g, b.b), t);
    }
  }
  const Stop& l = st[n - 1];
  return RGB(l.r, l.g, l.b);
}

}  // namespace nl
