// NexusLED - efectos de los segmentos y animaciones de estado de NEXUS.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "nl_color.h"

namespace nl {

struct Rng {
  uint32_t s = 0x9E3779B9u;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  uint32_t below(uint32_t n) { return n ? next() % n : 0; }
  uint8_t u8() { return (uint8_t)(next() >> 24); }
};

struct Segment {
  int start = 0, stop = 0;  // [start, stop) sobre la tira logica
  bool on = true;
  uint8_t bri = 255;
  uint8_t fx = 0, speed = 128, intensity = 128, pal = 0;
  RGB col[3] = {RGB(255, 160, 60), RGB(0, 0, 0), RGB(0, 0, 0)};
  bool reverse = false, mirror = false;
  std::string name;
  // estado de ejecucion del efecto (no se guarda)
  std::vector<uint8_t> data;
  uint32_t aux = 0, aux2 = 0, last = 0;
  uint8_t lastFx = 255;
  int len() const { return stop > start ? stop - start : 0; }
  void reset() {
    data.clear();
    aux = aux2 = last = 0;
  }
};

struct FxCtx {
  Segment& s;
  RGB* px;      // pixeles del segmento (longitud virtual: la mitad si esta en espejo)
  int L;
  uint32_t now;
  Rng& rng;
};

typedef void (*FxFn)(FxCtx&);

// periodo en ms segun la velocidad (0 = lento, 255 = rapido)
inline uint32_t periodMs(const Segment& s, uint32_t slow, uint32_t fast) {
  return slow - (uint32_t)((uint64_t)(slow - fast) * s.speed / 255);
}
// fase 0-65535 de un ciclo
inline uint16_t phase16(const FxCtx& c, uint32_t slow, uint32_t fast) {
  uint32_t p = periodMs(c.s, slow, fast);
  return (uint16_t)(((uint64_t)(c.now % p) * 65536) / p);
}
inline bool isBlack(RGB c) { return !c.r && !c.g && !c.b; }

// color de la paleta del segmento; la 0 recorre sus colores (o el arcoiris si se pide)
inline RGB palColor(const Segment& s, uint8_t pos, bool rainbowDefault = false) {
  if (s.pal > 0 && s.pal < PALETTE_COUNT) return paletteAt(PALETTES[s.pal].stops, PALETTES[s.pal].n, pos);
  if (rainbowDefault) return hsv(pos);
  RGB cs[3];
  int n = 0;
  for (int i = 0; i < 3; i++)
    if (i == 0 || !isBlack(s.col[i])) cs[n++] = s.col[i];
  if (n == 1) return cs[0];
  // ciclo cerrado c0 -> c1 (-> c2) -> c0
  uint32_t seg = 256 / n;
  int i = pos / seg;
  if (i >= n) i = n - 1;
  uint8_t t = (uint8_t)(((pos - i * seg) * 255) / seg);
  return blend(cs[i], cs[(i + 1) % n], t);
}

inline void fill(FxCtx& c, RGB col) {
  for (int i = 0; i < c.L; i++) c.px[i] = col;
}
// buffer persistente de pixeles (para estelas) dentro de s.data
inline RGB* persist(FxCtx& c) {
  size_t need = (size_t)c.L * 3;
  if (c.s.data.size() != need) c.s.data.assign(need, 0);
  return reinterpret_cast<RGB*>(c.s.data.data());
}
inline void fadeAll(RGB* b, int L, uint8_t keep) {
  for (int i = 0; i < L; i++) b[i] = scale(b[i], keep);
}

// ---------------------------------------------------------------- efectos
inline void fxSolid(FxCtx& c) { fill(c, c.s.col[0]); }

inline void fxBlink(FxCtx& c) {
  uint16_t ph = phase16(c, 3000, 100);
  bool onp = ph < (uint32_t)(c.s.intensity + 1) * 256;
  fill(c, onp ? c.s.col[0] : c.s.col[1]);
}

inline void fxBreathe(FxCtx& c) {
  uint8_t v = sin8((uint8_t)(phase16(c, 8000, 800) >> 8));
  v = (uint8_t)(20 + (v * 235) / 255);
  fill(c, blend(c.s.col[1], c.s.col[0], v));
}

inline void fxWipe(FxCtx& c) {
  uint16_t ph = phase16(c, 8000, 400);
  bool second = ph >= 32768;
  int lit = (int)(((uint32_t)(ph & 32767) * c.L) / 32768);
  RGB a = second ? c.s.col[1] : c.s.col[0];
  RGB b = second ? c.s.col[0] : c.s.col[1];
  for (int i = 0; i < c.L; i++) c.px[i] = i <= lit ? a : b;
}

inline void fxRainbow(FxCtx& c) { fill(c, palColor(c.s, (uint8_t)(phase16(c, 20000, 1000) >> 8), true)); }

inline void fxRainbowCycle(FxCtx& c) {
  uint8_t off = (uint8_t)(phase16(c, 20000, 800) >> 8);
  uint32_t spread = 256u * (1 + c.s.intensity / 64);
  for (int i = 0; i < c.L; i++) c.px[i] = palColor(c.s, (uint8_t)(off + (i * spread) / (c.L ? c.L : 1)), true);
}

inline void fxTheater(FxCtx& c) {
  uint32_t p = periodMs(c.s, 600, 40);
  uint32_t step = c.now / p;
  int gap = 2 + c.s.intensity / 64;
  for (int i = 0; i < c.L; i++)
    c.px[i] = ((i + step) % gap == 0) ? palColor(c.s, (uint8_t)(i * 256 / (c.L ? c.L : 1))) : c.s.col[1];
}

inline void fxComet(FxCtx& c) {
  RGB* b = persist(c);
  fadeAll(b, c.L, (uint8_t)(160 + c.s.intensity / 3));
  uint16_t ph = phase16(c, 6000, 400);
  int head = (int)(((uint32_t)ph * c.L) / 65536);
  b[head] = palColor(c.s, (uint8_t)(ph >> 8));
  for (int i = 0; i < c.L; i++) c.px[i] = addc(b[i], c.s.col[1]);
}

inline void fxScanner(FxCtx& c) {
  RGB* b = persist(c);
  fadeAll(b, c.L, (uint8_t)(140 + c.s.intensity / 3));
  uint16_t ph = phase16(c, 5000, 500);
  uint8_t t = tri8((uint8_t)(ph >> 8));
  int head = (int)(((uint32_t)t * (c.L - 1)) / 255);
  b[head] = c.s.col[0];
  for (int i = 0; i < c.L; i++) c.px[i] = addc(b[i], c.s.col[1]);
}

inline void fxTwinkle(FxCtx& c) {
  if (c.s.data.size() != (size_t)c.L * 2) c.s.data.assign((size_t)c.L * 2, 0);
  uint8_t* st = c.s.data.data();  // [i*2] = fase, [i*2+1] = color
  uint8_t inc = (uint8_t)(1 + c.s.speed / 24);
  for (int i = 0; i < c.L; i++) {
    uint8_t& ph = st[i * 2];
    if (ph) {
      int nv = ph + inc;
      ph = nv > 255 ? 0 : (uint8_t)nv;
    } else if (c.rng.below(4000) < (uint32_t)(c.s.intensity + 4)) {
      ph = 1;
      st[i * 2 + 1] = c.rng.u8();
    }
    c.px[i] = ph ? blend(c.s.col[1], palColor(c.s, st[i * 2 + 1]), tri8(ph)) : c.s.col[1];
  }
}

inline void fxSparkle(FxCtx& c) {
  for (int i = 0; i < c.L; i++) c.px[i] = c.s.col[1];
  int n = 1 + (c.L * c.s.intensity) / 2048;
  for (int k = 0; k < n; k++)
    if (c.rng.below(255) < (uint32_t)(c.s.speed / 2 + 30)) c.px[c.rng.below(c.L)] = c.s.col[0];
}

inline void fxFire(FxCtx& c) {
  if (c.s.data.size() != (size_t)c.L) c.s.data.assign(c.L, 0);
  uint8_t* heat = c.s.data.data();
  if (c.now - c.s.last < 16) {
    // misma imagen: sin cambio de estado entre frames muy seguidos
  } else {
    c.s.last = c.now;
    uint8_t cooling = (uint8_t)(20 + (255 - c.s.intensity) / 4);
    for (int i = 0; i < c.L; i++) {
      int cool = (int)c.rng.below(((cooling * 10) / (c.L ? c.L : 1)) + 2);
      heat[i] = heat[i] > cool ? heat[i] - cool : 0;
    }
    for (int k = c.L - 1; k >= 2; k--) heat[k] = (uint8_t)((heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3);
    if (c.rng.below(255) < (uint32_t)(60 + c.s.speed / 2)) {
      int y = (int)c.rng.below(c.L < 7 ? c.L : 7);
      heat[y] = qadd(heat[y], 160 + c.rng.below(95));
    }
  }
  for (int i = 0; i < c.L; i++) {
    uint8_t h = heat[i];
    c.px[i] = c.s.pal ? palColor(c.s, h) : paletteAt(P_FIRE, 5, h);
  }
}

inline void fxGradient(FxCtx& c) {
  bool three = !isBlack(c.s.col[2]);
  for (int i = 0; i < c.L; i++) {
    uint8_t t = (uint8_t)((i * 255) / (c.L > 1 ? c.L - 1 : 1));
    if (!three)
      c.px[i] = blend(c.s.col[0], c.s.col[1], t);
    else
      c.px[i] = t < 128 ? blend(c.s.col[0], c.s.col[1], (uint8_t)(t * 2)) : blend(c.s.col[1], c.s.col[2], (uint8_t)((t - 128) * 2));
  }
}

inline void fxPalette(FxCtx& c) {
  uint8_t off = (uint8_t)(phase16(c, 30000, 1500) >> 8);
  for (int i = 0; i < c.L; i++) c.px[i] = palColor(c.s, (uint8_t)(off + (i * 255) / (c.L ? c.L : 1)));
}

inline void fxPolice(FxCtx& c) {
  uint32_t p = periodMs(c.s, 1200, 200);
  uint32_t k = (c.now / (p / 8 ? p / 8 : 1)) % 8;
  bool leftOn = (k == 0 || k == 2);
  bool rightOn = (k == 4 || k == 6);
  int half = c.L / 2;
  for (int i = 0; i < c.L; i++) {
    bool left = i < half;
    c.px[i] = left ? (leftOn ? RGB(255, 0, 0) : RGB()) : (rightOn ? RGB(0, 0, 255) : RGB());
  }
}

inline void fxMeteor(FxCtx& c) {
  RGB* b = persist(c);
  for (int i = 0; i < c.L; i++)
    if (c.rng.below(10) > 4) b[i] = scale(b[i], (uint8_t)(150 + c.s.intensity / 3));
  uint16_t ph = phase16(c, 6000, 500);
  int head = (int)(((uint32_t)ph * (c.L + 8)) / 65536) - 4;
  for (int k = 0; k < 4; k++) {
    int j = head + k;
    if (j >= 0 && j < c.L) b[j] = palColor(c.s, (uint8_t)(j * 255 / (c.L ? c.L : 1)));
  }
  for (int i = 0; i < c.L; i++) c.px[i] = addc(b[i], c.s.col[1]);
}

inline void fxConfetti(FxCtx& c) {
  RGB* b = persist(c);
  fadeAll(b, c.L, (uint8_t)(200 + c.s.speed / 6 > 250 ? 250 : 200 + c.s.speed / 6));
  int n = 1 + (c.L * c.s.intensity) / 4096;
  for (int k = 0; k < n; k++)
    if (c.rng.below(100) < 35) b[c.rng.below(c.L)] = palColor(c.s, c.rng.u8(), true);
  for (int i = 0; i < c.L; i++) c.px[i] = b[i];
}

inline void fxWave(FxCtx& c) {
  uint8_t off = (uint8_t)(phase16(c, 8000, 500) >> 8);
  int waves = 1 + c.s.intensity / 32;
  for (int i = 0; i < c.L; i++) {
    uint8_t v = sin8((uint8_t)(off + (i * 256 * waves) / (c.L ? c.L : 1)));
    c.px[i] = blend(c.s.col[1], c.s.col[0], v);
  }
}

inline void fxStrobe(FxCtx& c) {
  uint32_t p = periodMs(c.s, 2000, 60);
  bool flash = (c.now % p) < 30;
  fill(c, flash ? c.s.col[0] : c.s.col[1]);
}

inline void fxDissolve(FxCtx& c) {
  if (c.s.data.size() != (size_t)c.L) c.s.data.assign(c.L, 0);
  uint8_t* st = c.s.data.data();
  int changes = 1 + (c.L * (c.s.speed + 1)) / 2048;
  bool toB = c.s.aux & 1;
  for (int k = 0; k < changes; k++) st[c.rng.below(c.L)] = toB ? 0 : 1;
  int count = 0;
  for (int i = 0; i < c.L; i++) count += st[i];
  if (!toB && count == c.L) c.s.aux ^= 1;
  if (toB && count == 0) c.s.aux ^= 1;
  for (int i = 0; i < c.L; i++) c.px[i] = st[i] ? c.s.col[0] : c.s.col[1];
}

inline void fxHeartbeat(FxCtx& c) {
  uint32_t p = periodMs(c.s, 2400, 600);
  uint32_t t = c.now % p;
  uint32_t a = p / 8;
  uint8_t v = 0;
  if (t < a) v = tri8((uint8_t)(t * 255 / a));
  else if (t > a + a / 2 && t < 2 * a + a / 2) v = tri8((uint8_t)((t - a - a / 2) * 255 / a));
  fill(c, blend(c.s.col[1], c.s.col[0], (uint8_t)(30 + v * 225 / 255)));
}

inline void fxPlasma(FxCtx& c) {
  uint16_t ph = phase16(c, 20000, 1500);
  uint8_t t1 = (uint8_t)(ph >> 8), t2 = (uint8_t)((ph >> 7) & 255);
  int scaleK = 1 + c.s.intensity / 48;
  for (int i = 0; i < c.L; i++) {
    uint8_t x = (uint8_t)((i * 256 * scaleK) / (c.L ? c.L : 1));
    uint8_t v = (uint8_t)((sin8((uint8_t)(x + t1)) + sin8((uint8_t)(x * 2 - t2))) / 2);
    c.px[i] = palColor(c.s, v, true);
  }
}

inline void fxLightning(FxCtx& c) {
  // aux = siguiente destello (ms), aux2 = fin del destello
  if (c.now >= c.s.aux) {
    c.s.aux2 = c.now + 40 + c.rng.below(80);
    c.s.aux = c.now + 200 + c.rng.below(periodMs(c.s, 6000, 400));
    c.s.last = c.rng.below(c.L);
  }
  fill(c, c.s.col[1]);
  if (c.now < c.s.aux2) {
    int w = 2 + (c.L * c.s.intensity) / 512;
    for (int i = -w; i <= w; i++) {
      int j = (int)c.s.last + i;
      if (j >= 0 && j < c.L) c.px[j] = c.s.col[0];
    }
  }
}

struct FxDef { const char* key; const char* name; FxFn fn; };
static const FxDef EFFECTS[] = {
    {"solid", "Fijo", fxSolid},
    {"blink", "Parpadeo", fxBlink},
    {"breathe", "Respiración", fxBreathe},
    {"wipe", "Barrido", fxWipe},
    {"rainbow", "Arcoíris", fxRainbow},
    {"rainbow_cycle", "Arcoíris en movimiento", fxRainbowCycle},
    {"theater", "Teatro", fxTheater},
    {"comet", "Cometa", fxComet},
    {"scanner", "Escáner", fxScanner},
    {"twinkle", "Centelleo", fxTwinkle},
    {"sparkle", "Destellos", fxSparkle},
    {"fire", "Fuego", fxFire},
    {"gradient", "Degradado", fxGradient},
    {"palette", "Paleta en movimiento", fxPalette},
    {"police", "Policía", fxPolice},
    {"meteor", "Meteorito", fxMeteor},
    {"confetti", "Confeti", fxConfetti},
    {"wave", "Ola", fxWave},
    {"strobe", "Estroboscopio", fxStrobe},
    {"dissolve", "Disolver", fxDissolve},
    {"heartbeat", "Latido", fxHeartbeat},
    {"plasma", "Plasma", fxPlasma},
    {"lightning", "Relámpagos", fxLightning},
};
static const int EFFECT_COUNT = sizeof(EFFECTS) / sizeof(EFFECTS[0]);

// ---------------------------------------------------------------- estados de NEXUS
// Animaciones que NEXUS pone encima de la escena mientras piensa, habla, escucha...
struct NexusStateDef { const char* key; const char* name; RGB color; };
static const NexusStateDef NEXUS_STATES[] = {
    {"none", "Sin estado (escena normal)", RGB(0, 0, 0)},
    {"idle", "En línea", RGB(0, 240, 255)},
    {"listening", "Escuchando", RGB(124, 255, 107)},
    {"thinking", "Pensando", RGB(166, 107, 255)},
    {"tool", "Usando herramienta", RGB(255, 176, 32)},
    {"speaking", "Hablando", RGB(255, 61, 242)},
    {"success", "Hecho", RGB(40, 255, 90)},
    {"error", "Error", RGB(255, 20, 20)},
    {"alert", "Aviso", RGB(255, 176, 32)},
};
static const int NEXUS_STATE_COUNT = sizeof(NEXUS_STATES) / sizeof(NEXUS_STATES[0]);

struct NexusStatus {
  uint8_t state = 0;     // indice en NEXUS_STATES (0 = ninguno)
  RGB color;             // color del estado (el de NEXUS por defecto)
  float level = -1;      // nivel de voz 0-1 (-1 = simulado)
  uint32_t since = 0;    // ms en que empezo
  uint32_t until = 0;    // ms en que caduca (0 = no caduca)
  uint32_t levelAt = 0;  // ms del ultimo nivel recibido
  float levelS = 0;      // nivel suavizado
};

inline void renderNexus(NexusStatus& n, RGB* px, int L, uint32_t now, Rng& rng) {
  RGB c = n.color;
  uint32_t t = now - n.since;
  const char* k = NEXUS_STATES[n.state].key;
  auto fillc = [&](RGB col) { for (int i = 0; i < L; i++) px[i] = col; };
  if (!strcmp(k, "idle")) {
    uint8_t v = sin8((uint8_t)((t % 6000) * 256 / 6000));
    fillc(scale(c, (uint8_t)(50 + v / 3)));
  } else if (!strcmp(k, "listening")) {
    // onda que sale del centro
    uint8_t off = (uint8_t)((t % 1500) * 256 / 1500);
    for (int i = 0; i < L; i++) {
      int d = i - L / 2;
      if (d < 0) d = -d;
      uint8_t v = sin8((uint8_t)(d * 512 / (L ? L : 1) - off));
      px[i] = scale(c, (uint8_t)(40 + v * 215 / 255));
    }
  } else if (!strcmp(k, "thinking")) {
    // dos cometas que giran
    for (int i = 0; i < L; i++) px[i] = scale(c, 18);
    uint32_t per = 1400;
    for (int h = 0; h < 2; h++) {
      int head = (int)(((t % per) * L) / per + h * L / 2) % (L ? L : 1);
      int tail = L / 5 + 2;
      for (int k2 = 0; k2 < tail; k2++) {
        int j = ((head - k2) % L + L) % L;
        px[j] = addc(px[j], scale(c, (uint8_t)(255 - k2 * 255 / tail)));
      }
    }
  } else if (!strcmp(k, "tool")) {
    uint32_t step = t / 60;
    for (int i = 0; i < L; i++) px[i] = ((i + step) % 6) < 2 ? c : scale(c, 25);
  } else if (!strcmp(k, "speaking")) {
    // vumetro desde el centro con el nivel de voz (o simulado)
    float target;
    if (n.level >= 0 && now - n.levelAt < 1500) {
      target = n.level;
    } else {
      target = 0.25f + (rng.below(1000) / 1000.0f) * 0.6f;
    }
    n.levelS += (target - n.levelS) * 0.35f;
    int half = L / 2;
    int lit = (int)(n.levelS * (half + 1));
    for (int i = 0; i < L; i++) {
      int d = i - half;
      if (d < 0) d = -d - (L % 2 == 0 ? 1 : 0);
      if (d < 0) d = 0;
      px[i] = d <= lit ? scale(c, (uint8_t)(255 - (lit ? d * 160 / (lit + 1) : 0))) : scale(c, 12);
    }
  } else if (!strcmp(k, "success")) {
    int lit = (int)((t > 600 ? 600 : t) * L / 600);
    uint8_t fade = t < 1200 ? 255 : (t > 2500 ? 60 : (uint8_t)(255 - (t - 1200) * 195 / 1300));
    for (int i = 0; i < L; i++) px[i] = i < lit ? scale(c, fade) : RGB();
  } else if (!strcmp(k, "error")) {
    bool flash = t < 1200 && ((t / 200) % 2 == 0);
    fillc(flash ? c : scale(c, 50));
  } else if (!strcmp(k, "alert")) {
    fillc(((t / 400) % 2 == 0) ? c : scale(c, 20));
  } else {
    fillc(RGB());
  }
}

}  // namespace nl
