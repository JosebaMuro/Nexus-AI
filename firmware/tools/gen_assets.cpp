// Genera los recursos del reloj NexusWatch (firmware/nexuswatch/nw_assets.h), que el Pico W tambien usa:
//
// * FONT: fuente proporcional de 16 px de alto para Latin-1 (0x20-0xFF), 1 bit por pixel, a partir de DejaVu Sans.
// * AVATAR: cara del avatar de Nexus (la geisha de los recursos de Nexus-Office) en RGB565, 150x150 y con mascara.
//
//   g++ -std=c++17 -O2 -I nexus-office-cpp/third_party/stb -o gen_assets firmware/tools/gen_assets.cpp
//   ./gen_assets [ruta/a/fuente.ttf] [ruta/a/face_front.png]
//
// Sin dependencias fuera de stb (ya en nexus-office-cpp/third_party/stb). Se ejecuta desde la raiz del repositorio.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

static const char* const OUT = "firmware/nexuswatch/nw_assets.h";
static const char* const FUENTES[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/arialbd.ttf",
                                      "/System/Library/Fonts/Supplemental/Arial Bold.ttf"};
static const char* const GEISHAS[] = {"nexus-office-cpp/recursos/web/assets/geisha/face_front.png",
                                      "nexus-office-cpp/recursos/assets/geisha/face_front.png",
                                      "nexus-office/nexus/web/assets/geisha/face_front.png"};
static const int H = 16;     // alto de la fuente
static const int AV = 150;   // lado del avatar

static bool existe(const std::string& p) { return std::ifstream(p).good(); }

static std::vector<unsigned char> leer(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

// Tabla de la fuente: datos 1 bpp por filas (MSB a la izquierda) e indice (desplazamiento, ancho) por caracter.
static bool tabla_fuente(const std::string& ruta, std::vector<int>& datos, std::vector<std::pair<int, int>>& indice) {
  std::vector<unsigned char> ttf = leer(ruta);
  stbtt_fontinfo f;
  if (ttf.empty() || !stbtt_InitFont(&f, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) return false;
  float esc = stbtt_ScaleForMappingEmToPixels(&f, 14);   // tamano 14 como en las versiones anteriores
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&f, &asc, &desc, &gap);
  int base = (int)std::lround(asc * esc) - 1;   // linea base: el texto se dibuja una fila mas arriba
  for (int cp = 0x20; cp < 0x100; cp++) {
    int ch = (cp >= 0x7F && cp < 0xA0) ? ' ' : cp;
    int avance, lsb;
    stbtt_GetCodepointHMetrics(&f, ch, &avance, &lsb);
    if (cp == 0xAD) avance = 0;   // el guion opcional no se dibuja
    int w = std::max(3, (int)std::lround(avance * esc) + 1);
    std::vector<unsigned char> px(w * H, 0);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&f, ch, esc, esc, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0 && avance > 0) {
      std::vector<unsigned char> g(gw * gh);
      stbtt_MakeCodepointBitmap(&f, g.data(), gw, gh, gw, esc, esc, ch);
      for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
          int dx = x0 + x, dy = base + y0 + y;
          if (dx >= 0 && dx < w && dy >= 0 && dy < H && g[y * gw + x] >= 128) px[dy * w + dx] = 1;
        }
    }
    indice.push_back({(int)datos.size(), w});
    int bpr = (w + 7) / 8;
    for (int y = 0; y < H; y++)
      for (int b = 0; b < bpr; b++) {
        int v = 0;
        for (int bit = 0; bit < 8; bit++) {
          int x = b * 8 + bit;
          if (x < w && px[y * w + x]) v |= 0x80 >> bit;
        }
        datos.push_back(v);
      }
  }
  return true;
}

// Autocontraste por canal quitando el 1 % de cada extremo del histograma (como ImageOps.autocontrast(cutoff=1)).
static void autocontraste(std::vector<unsigned char>& rgba, int n) {
  for (int c = 0; c < 3; c++) {
    long hist[256] = {0};
    for (int i = 0; i < n; i++) hist[rgba[i * 4 + c]]++;
    long corte = n / 100, acum = 0;
    int lo = 0, hi = 255;
    for (lo = 0; lo < 256 && (acum += hist[lo]) <= corte; lo++) {}
    acum = 0;
    for (hi = 255; hi >= 0 && (acum += hist[hi]) <= corte; hi--) {}
    if (hi <= lo) continue;
    double k = 255.0 / (hi - lo);
    for (int i = 0; i < n; i++) {
      int v = (int)std::lround((rgba[i * 4 + c] - lo) * k);
      rgba[i * 4 + c] = (unsigned char)std::clamp(v, 0, 255);
    }
  }
}

static bool avatar(const std::string& ruta, std::vector<int>& out) {
  int w, h, n;
  unsigned char* img = stbi_load(ruta.c_str(), &w, &h, &n, 4);
  if (!img) return false;
  // recorte cuadrado centrado en la cara (el tercio superior de la imagen es el tocado)
  int lado = std::min(w, h), top = (int)(h * 0.12), izq = (w - lado) / 2;
  std::vector<unsigned char> rec(lado * lado * 4, 0);
  for (int y = 0; y < lado; y++)
    for (int x = 0; x < lado; x++) {
      int sy = top + y;
      if (sy < h) std::copy_n(img + (sy * w + izq + x) * 4, 4, &rec[(y * lado + x) * 4]);
    }
  stbi_image_free(img);
  std::vector<unsigned char> cara(AV * AV * 4);
  stbir_resize_uint8_generic(rec.data(), lado, lado, 0, cara.data(), AV, AV, 0, 4, 3, 0, STBIR_EDGE_CLAMP,
                             STBIR_FILTER_CATMULLROM, STBIR_COLORSPACE_LINEAR, nullptr);
  autocontraste(cara, AV * AV);
  double r = AV / 2.0;
  for (int y = 0; y < AV; y++)
    for (int x = 0; x < AV; x++) {
      const unsigned char* p = &cara[(y * AV + x) * 4];
      double dx = x + 0.5 - r, dy = y + 0.5 - r;
      bool dentro = dx * dx + dy * dy <= r * r;
      if (!dentro || p[3] < 110) {
        out.push_back(0x0001);   // transparente
        continue;
      }
      int c = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
      out.push_back(c == 0x0001 ? 0x0000 : c);
    }
  return true;
}

int main(int argc, char** argv) {
  std::string fuente = argc > 1 ? argv[1] : "", geisha = argc > 2 ? argv[2] : "";
  for (const char* p : FUENTES)
    if (fuente.empty() && existe(p)) fuente = p;
  for (const char* p : GEISHAS)
    if (geisha.empty() && existe(p)) geisha = p;
  if (fuente.empty()) return std::fprintf(stderr, "no encuentro una fuente TTF: pasala como argumento\n"), 1;
  if (geisha.empty()) return std::fprintf(stderr, "no encuentro face_front.png de la geisha: pasala como segundo argumento\n"), 1;
  std::vector<int> datos, av;
  std::vector<std::pair<int, int>> indice;
  if (!tabla_fuente(fuente, datos, indice)) return std::fprintf(stderr, "no puedo leer la fuente %s\n", fuente.c_str()), 1;
  if (!avatar(geisha, av)) return std::fprintf(stderr, "no puedo leer %s\n", geisha.c_str()), 1;

  std::string s = "// Generado por firmware/tools/gen_assets.cpp: no editar a mano.\n#pragma once\n#include <stdint.h>\n";
  s += "#define NW_FONT_H " + std::to_string(H) + "\n#define NW_AVATAR " + std::to_string(AV) + "\n";
  s += "static const uint16_t NW_FONT_IDX[][2] = {";
  for (size_t i = 0; i < indice.size(); i++)
    s += (i ? ",{" : "{") + std::to_string(indice[i].first) + "," + std::to_string(indice[i].second) + "}";
  s += "};\nstatic const uint8_t NW_FONT[] = {\n";
  for (size_t i = 0; i < datos.size(); i += 32) {
    s += "  ";
    for (size_t j = i; j < std::min(datos.size(), i + 32); j++) s += (j > i ? "," : "") + std::to_string(datos[j]);
    s += ",\n";
  }
  s += "};\n// cara del avatar, RGB565; 0x0001 = transparente\nstatic const uint16_t NW_AVATAR_PX[] = {\n";
  char hex[8];
  for (size_t i = 0; i < av.size(); i += 24) {
    s += "  ";
    for (size_t j = i; j < std::min(av.size(), i + 24); j++) {
      std::snprintf(hex, sizeof hex, "0x%04X", av[j]);
      s += (j > i ? "," : "") + std::string(hex);
    }
    s += ",\n";
  }
  s += "};\n";
  std::ofstream(OUT, std::ios::binary) << s;
  std::printf("%s: fuente %zu bytes, avatar %zu bytes (%s, %s)\n", OUT, datos.size(), av.size() * 2, fuente.c_str(), geisha.c_str());
  return 0;
}
