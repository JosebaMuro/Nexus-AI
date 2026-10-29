// Motor de imagen integrado (sin GIMP): decodificacion con stb_image y libwebp, escritura con stb_image_write,
// libwebp y un escritor TIFF propio, y las operaciones de Pillow que usa media/gimp.py (pil_apply / pil_edit).
#include "media/raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstring>
#include <functional>
#include <set>

#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include "stb_truetype.h"
#include "util/fs.h"
#include "util/text.h"
#include "webp/decode.h"
#include "webp/encode.h"

namespace nx::raster {

namespace {

uint8_t clamp8(double v) { return (uint8_t)std::clamp((int)std::lround(v), 0, 255); }

// ------------------------------------------------------------------ EXIF y metadatos
struct Reader {
  const uint8_t* d;
  size_t n;
  bool le;
  uint32_t u16(size_t o) const {
    if (o + 2 > n) return 0;
    return le ? d[o] | (d[o + 1] << 8) : (d[o] << 8) | d[o + 1];
  }
  uint32_t u32(size_t o) const {
    if (o + 4 > n) return 0;
    return le ? d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)
              : ((uint32_t)d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
  }
};

// IFD0 de una cabecera TIFF (EXIF o fichero TIFF). Devuelve las etiquetas como texto y, aparte, las numericas
void parse_tiff(const uint8_t* d, size_t n, Info& info, std::map<int, uint32_t>* nums = nullptr,
                std::map<int, double>* rats = nullptr) {
  if (n < 8) return;
  Reader r{d, n, d[0] == 'I'};
  if (!((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M'))) return;
  uint32_t ifd = r.u32(4);
  uint32_t count = r.u16(ifd);
  static const std::set<int> TEXT = {0x010F, 0x0110, 0x0132, 0x013B, 0x8298};
  for (uint32_t i = 0; i < count && i < 512; ++i) {
    size_t e = ifd + 2 + i * 12;
    if (e + 12 > n) break;
    int tag = (int)r.u16(e), type = (int)r.u16(e + 2);
    uint32_t cnt = r.u32(e + 4);
    if (type == 3 || type == 4) {
      uint32_t v = type == 3 ? r.u16(e + 8) : r.u32(e + 8);
      if (nums) (*nums)[tag] = v;
      if (tag == 0x0112) {
        info.orientation = (int)v;
        info.exif[tag] = std::to_string(v);
      }
    } else if (type == 5 && rats) {
      uint32_t off = r.u32(e + 8);
      if (off + 8 > n) continue;
      uint32_t num = r.u32(off), den = r.u32(off + 4);
      if (den) (*rats)[tag] = (double)num / den;
    } else if (type == 2 && TEXT.count(tag)) {
      size_t off = cnt <= 4 ? e + 8 : r.u32(e + 8);
      if (off + cnt > n) continue;
      std::string s((const char*)d + off, cnt);
      while (!s.empty() && s.back() == '\0') s.pop_back();
      info.exif[tag] = valid_utf8(s);
    }
  }
}

std::string read_bytes(const fs::path& p) {
  auto raw = read_file(p);
  if (!raw) throw Error("no se puede leer " + S(p.filename()));
  return *raw;
}

Info probe_bytes(const std::string& data) {
  Info info;
  auto* d = (const uint8_t*)data.data();
  size_t n = data.size();
  if (n >= 8 && !std::memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
    info.format = "PNG";
    size_t o = 8;
    while (o + 8 <= n) {
      uint32_t len = (d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
      std::string type((const char*)d + o + 4, 4);
      const uint8_t* c = d + o + 8;
      if (o + 12 + len > n) break;
      if (type == "IHDR" && len >= 13) {
        info.w = (int)((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]);
        info.h = (int)((c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7]);
        static const std::map<int, std::string> MODES = {{0, "L"}, {2, "RGB"}, {3, "P"}, {4, "LA"}, {6, "RGBA"}};
        auto it = MODES.find(c[9]);
        info.mode = it == MODES.end() ? "RGB" : it->second;
      } else if (type == "tRNS") {
        info.transparency = true;
      } else if (type == "pHYs" && len >= 9 && c[8] == 1) {
        double x = ((c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]) * 0.0254;
        double y = ((c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7]) * 0.0254;
        info.dpi = std::make_pair(x, y);
      } else if (type == "eXIf") {
        parse_tiff(c, len, info);
      } else if (type == "IEND") {
        break;
      }
      o += 12 + len;
    }
  } else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
    info.format = "JPEG";
    info.mode = "RGB";
    std::map<int, uint32_t> jn;
    std::map<int, double> jr;
    bool has_exif = false;
    size_t o = 2;
    while (o + 4 <= n && d[o] == 0xFF) {
      int m = d[o + 1];
      if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
        o += 2;
        continue;
      }
      size_t len = (d[o + 2] << 8) | d[o + 3];
      const uint8_t* c = d + o + 4;
      if (o + 2 + len > n) break;
      if (m == 0xE0 && len >= 14 && !std::memcmp(c, "JFIF", 5)) {
        int unit = c[7];
        double x = (c[8] << 8) | c[9], y = (c[10] << 8) | c[11];
        if (unit == 1) info.dpi = std::make_pair(x, y);
        if (unit == 2) info.dpi = std::make_pair(x * 2.54, y * 2.54);
      } else if (m == 0xE1 && len >= 8 && !std::memcmp(c, "Exif\0\0", 6)) {
        parse_tiff(c + 6, len - 8, info, &jn, &jr);
        has_exif = true;
      } else if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC && len >= 7) {
        info.h = (c[1] << 8) | c[2];
        info.w = (c[3] << 8) | c[4];
        if (c[5] == 1) info.mode = "L";
        if (c[5] == 4) info.mode = "CMYK";
      } else if (m == 0xDA) {
        break;
      }
      o += 2 + len;
    }
    if (!info.dpi && has_exif) {   // como Pillow: la resolucion del EXIF o, si no la hay, 72 ppp
      if (jr.count(0x011A)) {
        double x = jr[0x011A] * (jn.count(0x0128) && jn[0x0128] == 3 ? 2.54 : 1);
        info.dpi = std::make_pair(x, x);
      } else {
        info.dpi = std::make_pair(72.0, 72.0);
      }
    }
  } else if (n >= 12 && !std::memcmp(d, "RIFF", 4) && !std::memcmp(d + 8, "WEBP", 4)) {
    info.format = "WEBP";
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(d, n, &f) == VP8_STATUS_OK) {
      info.w = f.width;
      info.h = f.height;
      info.mode = f.has_alpha ? "RGBA" : "RGB";
      info.transparency = f.has_alpha;
    }
  } else if (n >= 8 && (!std::memcmp(d, "II*\0", 4) || !std::memcmp(d, "MM\0*", 4))) {
    info.format = "TIFF";
    std::map<int, uint32_t> nums;
    parse_tiff(d, n, info, &nums);
    info.w = (int)nums[256];
    info.h = (int)nums[257];
    int spp = nums.count(277) ? (int)nums[277] : 1;
    info.mode = spp >= 4 ? "RGBA" : spp == 3 ? "RGB" : "L";
    info.transparency = spp == 4 || spp == 2;
  } else {
    int w, h, comp;
    if (!stbi_info_from_memory(d, (int)n, &w, &h, &comp)) throw Error("formato de imagen no reconocido");
    info.w = w;
    info.h = h;
    if (n >= 6 && !std::memcmp(d, "GIF8", 4)) {
      info.format = "GIF";
      info.mode = "P";
      for (size_t i = 0; i + 3 < n && i < 4096; ++i)
        if (d[i] == 0x21 && d[i + 1] == 0xF9 && d[i + 2] == 0x04 && (d[i + 3] & 1)) info.transparency = true;
    } else {
      info.format = n >= 2 && d[0] == 'B' && d[1] == 'M' ? "BMP" : n >= 4 && !std::memcmp(d, "8BPS", 4) ? "PSD" : "TGA";
      info.mode = comp == 4 ? "RGBA" : comp == 2 ? "LA" : comp == 1 ? "L" : "RGB";
      if (info.format == "BMP" && n >= 46 && (d[14] | (d[15] << 8)) >= 40) {
        auto i32 = [&](size_t o) { return (int32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)); };
        info.dpi = std::make_pair(i32(38) / 39.3701, i32(42) / 39.3701);
      }
      info.transparency = comp == 4 || comp == 2;
    }
  }
  if (info.orientation >= 5 && info.orientation <= 8) {}   // el tamano devuelto es el del fichero (como PIL)
  return info;
}

Image transpose(const Image& s, int orientation) {
  if (orientation < 2 || orientation > 8) return s;
  bool swap = orientation >= 5;
  Image o(swap ? s.h : s.w, swap ? s.w : s.h, s.c);
  for (int y = 0; y < s.h; ++y)
    for (int x = 0; x < s.w; ++x) {
      int nx = x, ny = y;
      switch (orientation) {
        case 2: nx = s.w - 1 - x; break;
        case 3: nx = s.w - 1 - x; ny = s.h - 1 - y; break;
        case 4: ny = s.h - 1 - y; break;
        case 5: nx = y; ny = x; break;
        case 6: nx = s.h - 1 - y; ny = x; break;
        case 7: nx = s.h - 1 - y; ny = s.w - 1 - x; break;
        case 8: nx = y; ny = s.w - 1 - x; break;
      }
      std::memcpy(o.at(nx, ny), s.at(x, y), (size_t)s.c);
    }
  return o;
}

void write_cb(void* ctx, void* data, int size) { ((std::string*)ctx)->append((const char*)data, (size_t)size); }

uint32_t crc_png(const std::string& s) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void put32be(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s += (char)((v >> (8 * i)) & 0xFF);
}

std::string tiff_bytes(const Image& img, std::optional<std::pair<double, double>> dpi) {
  // TIFF de base sin comprimir, II, una sola tira
  std::string out = std::string("II*\0", 4);
  auto u16 = [&](uint32_t v) { out += (char)(v & 255); out += (char)((v >> 8) & 255); };
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out += (char)((v >> (8 * i)) & 255); };
  size_t data_len = img.px.size();
  uint32_t data_off = 8, bps_off = (uint32_t)(data_off + data_len), res_off = bps_off + 8;
  u32(bps_off + 8 + 16);   // la IFD va al final
  out.append((const char*)img.px.data(), data_len);
  for (int i = 0; i < 4; ++i) u16(8);
  double xd = dpi ? dpi->first : 72, yd = dpi ? dpi->second : 72;
  u32((uint32_t)std::lround(xd * 100)); u32(100);
  u32((uint32_t)std::lround(yd * 100)); u32(100);
  std::vector<std::array<uint32_t, 4>> e = {
      {256, 4, 1, (uint32_t)img.w}, {257, 4, 1, (uint32_t)img.h}, {258, 3, (uint32_t)img.c, img.c > 2 ? bps_off : 8},
      {259, 3, 1, 1}, {262, 3, 1, 2}, {273, 4, 1, data_off}, {277, 3, 1, (uint32_t)img.c}, {278, 4, 1, (uint32_t)img.h},
      {279, 4, 1, (uint32_t)data_len}, {282, 5, 1, res_off}, {283, 5, 1, res_off + 8}, {284, 3, 1, 1}, {296, 3, 1, 2}};
  if (img.c == 4) e.push_back({338, 3, 1, 2});   // alfa sin premultiplicar
  u16((uint32_t)e.size());
  for (auto& f : e) {
    u16(f[0]);
    u16(f[1]);
    u32(f[2]);
    if (f[1] == 3 && f[2] == 1) {
      u16(f[3]);
      u16(0);
    } else {
      u32(f[3]);
    }
  }
  u32(0);
  return out;
}

// ------------------------------------------------------------------ utilidades de pixel
inline int lum(const uint8_t* p) { return (p[0] * 19595 + p[1] * 38470 + p[2] * 7471 + 0x8000) >> 16; }   // PIL "L"

template <class F>
void each_rgb(Image& img, F f) {
  for (size_t i = 0; i < img.px.size(); i += img.c) f(&img.px[i]);
}

void point(Image& img, const std::function<int(int)>& fn) {
  uint8_t lut[256];
  for (int v = 0; v < 256; ++v) lut[v] = (uint8_t)std::clamp(fn(v), 0, 255);
  each_rgb(img, [&](uint8_t* p) {
    for (int k = 0; k < 3; ++k) p[k] = lut[p[k]];
  });
}

void point_ch(Image& img, const std::array<std::array<uint8_t, 256>, 3>& luts) {
  each_rgb(img, [&](uint8_t* p) {
    for (int k = 0; k < 3; ++k) p[k] = luts[k][p[k]];
  });
}

// desenfoque gaussiano aproximado con tres pasadas de caja (como ImageFilter.GaussianBlur de Pillow)
void box_pass(std::vector<float>& a, std::vector<float>& tmp, int w, int h, int ch, int r, bool horiz) {
  if (r < 1) return;
  int len = horiz ? w : h, lines = horiz ? h : w;
  float inv = 1.0f / (2 * r + 1);
  for (int l = 0; l < lines; ++l)
    for (int c = 0; c < ch; ++c) {
      auto idx = [&](int i) {
        i = std::clamp(i, 0, len - 1);
        return horiz ? (((size_t)l * w + i) * ch + c) : (((size_t)i * w + l) * ch + c);
      };
      float acc = 0;
      for (int i = -r; i <= r; ++i) acc += a[idx(i)];
      for (int i = 0; i < len; ++i) {
        tmp[idx(i)] = acc * inv;
        acc += a[idx(i + r + 1)] - a[idx(i - r)];
      }
    }
  a.swap(tmp);
}

void gaussian(std::vector<float>& a, int w, int h, int ch, double sigma) {
  if (sigma <= 0) return;
  // radios de las cajas para tres pasadas (Kovesi)
  double wi = std::sqrt(12 * sigma * sigma / 3 + 1);
  int wl = (int)std::floor(wi);
  if (wl % 2 == 0) --wl;
  int wu = wl + 2;
  double mi = (12 * sigma * sigma - 3.0 * wl * wl - 4.0 * 3 * wl - 3 * 3) / (-4.0 * wl - 4);
  int m = (int)std::lround(mi);
  std::vector<float> tmp(a.size());
  for (int i = 0; i < 3; ++i) {
    int r = ((i < m ? wl : wu) - 1) / 2;
    box_pass(a, tmp, w, h, ch, r, true);
    box_pass(a, tmp, w, h, ch, r, false);
  }
}

std::vector<float> to_float(const Image& img, int ch) {
  std::vector<float> f((size_t)img.w * img.h * ch);
  for (size_t i = 0, j = 0; i < img.px.size(); i += img.c)
    for (int k = 0; k < ch; ++k) f[j++] = img.px[i + k];
  return f;
}

Image blurred(const Image& img, double sigma) {
  auto f = to_float(img, 3);
  gaussian(f, img.w, img.h, 3, sigma);
  Image o = img;
  for (size_t i = 0, j = 0; i < o.px.size(); i += o.c)
    for (int k = 0; k < 3; ++k) o.px[i + k] = clamp8(f[j++]);
  return o;
}

Image resize(const Image& img, int w, int h) {
  Image o(w, h, img.c);
  stbir_resize_uint8_generic(img.px.data(), img.w, img.h, 0, o.px.data(), w, h, 0, img.c, img.c == 4 ? 3 : STBIR_ALPHA_CHANNEL_NONE,
                             0, STBIR_EDGE_CLAMP, STBIR_FILTER_CATMULLROM, STBIR_COLORSPACE_LINEAR, nullptr);
  return o;
}

std::array<uint8_t, 4> rgba_of(const json& c) {
  std::array<uint8_t, 4> o = {255, 255, 255, 255};
  if (c.is_array())
    for (size_t i = 0; i < 3 && i < c.size(); ++i) o[i] = (uint8_t)std::clamp(c[i].get<int>(), 0, 255);
  return o;
}

std::pair<int, int> place(const std::string& pos, int W, int H, int w, int h) {
  int m = (int)std::lround(0.03 * std::min(W, H));
  int x = contains(pos, "izquierda") ? m : contains(pos, "derecha") ? W - w - m : (W - w) / 2;
  int y = starts_with(pos, "arriba") ? m : starts_with(pos, "abajo") ? H - h - m : (H - h) / 2;
  return {x, y};
}

// composicion "over" de un pixel RGBA sobre otro
void blend(uint8_t* dst, int dc, const uint8_t* src, double alpha) {
  if (alpha <= 0) return;
  if (dc == 4) {
    double da = dst[3] / 255.0, oa = alpha + da * (1 - alpha);
    for (int k = 0; k < 3; ++k) dst[k] = oa > 0 ? clamp8((src[k] * alpha + dst[k] * da * (1 - alpha)) / oa) : 0;
    dst[3] = clamp8(oa * 255);
  } else {
    for (int k = 0; k < 3; ++k) dst[k] = clamp8(src[k] * alpha + dst[k] * (1 - alpha));
  }
}

// ------------------------------------------------------------------ fuentes (stb_truetype)
std::string find_font(const std::string& name) {
  std::vector<std::string> cands;
  std::string ln = lower(name);
  if (!name.empty() && ln != "sans-serif" && ln != "sans" && ln != "serif") cands.push_back(name);
  for (auto* c : {"DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                  "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                  "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                  "arial.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
                  "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc",
                  "/system/fonts/Roboto-Regular.ttf", "/data/data/com.termux/files/usr/share/fonts/TTF/DejaVuSans.ttf"})
    cands.push_back(c);
  for (auto& c : cands) {
    fs::path p = P(c);
    if (is_file(p)) return read_text(p);
    if (is_file(paths::package() / "fonts" / p.filename())) return read_text(paths::package() / "fonts" / p.filename());
  }
  throw Error("no hay ninguna fuente TrueType en este equipo para escribir texto");
}

void draw_text(Image& img, const json& p) {
  std::string font = find_font(jstr(p, "fuente"));
  stbtt_fontinfo fi;
  int off = stbtt_GetFontOffsetForIndex((const unsigned char*)font.data(), 0);
  if (off < 0 || !stbtt_InitFont(&fi, (const unsigned char*)font.data(), off)) throw Error("fuente no valida");
  int size = p.value("tamano", 24);
  float scale = stbtt_ScaleForMappingEmToPixels(&fi, (float)size);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
  std::u32string text = u32(jstr(p, "texto"));
  // primero la caja de tinta (textbbox de Pillow), luego se pinta en una capa de cobertura
  int baseline = (int)std::ceil(asc * scale);
  int lh = (int)std::ceil((asc - desc) * scale) + 2;
  float xpos = 0;
  std::vector<std::tuple<int, float, int>> glyphs;   // (glifo, x, indice)
  for (size_t i = 0; i < text.size(); ++i) {
    int g = stbtt_FindGlyphIndex(&fi, (int)text[i]);
    glyphs.emplace_back(g, xpos, (int)i);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&fi, g, &adv, &lsb);
    xpos += adv * scale;
    if (i + 1 < text.size()) xpos += scale * stbtt_GetGlyphKernAdvance(&fi, g, stbtt_FindGlyphIndex(&fi, (int)text[i + 1]));
  }
  int lw = (int)std::ceil(xpos) + 4;
  std::vector<uint8_t> cov((size_t)lw * lh, 0);
  for (auto& [g, gx, _] : glyphs) {
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&fi, g, scale, scale, gx - std::floor(gx), 0, &x0, &y0, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0 || bh <= 0) continue;
    std::vector<uint8_t> bm((size_t)bw * bh);
    stbtt_MakeGlyphBitmapSubpixel(&fi, bm.data(), bw, bh, bw, scale, scale, gx - std::floor(gx), 0, g);
    int ox = (int)std::floor(gx) + x0 + 1, oy = baseline + y0;
    for (int y = 0; y < bh; ++y)
      for (int x = 0; x < bw; ++x) {
        int X = ox + x, Y = oy + y;
        if (X < 0 || Y < 0 || X >= lw || Y >= lh) continue;
        auto& d = cov[(size_t)Y * lw + X];
        d = (uint8_t)std::max<int>(d, bm[(size_t)y * bw + x]);
      }
  }
  int ix0 = lw, iy0 = lh, ix1 = -1, iy1 = -1;
  for (int y = 0; y < lh; ++y)
    for (int x = 0; x < lw; ++x)
      if (cov[(size_t)y * lw + x]) {
        ix0 = std::min(ix0, x), iy0 = std::min(iy0, y), ix1 = std::max(ix1, x), iy1 = std::max(iy1, y);
      }
  if (ix1 < 0) return;
  auto [px, py] = place(jstr(p, "posicion", "abajo-derecha"), img.w, img.h, ix1 - ix0 + 1, iy1 - iy0 + 1);
  auto col = rgba_of(p.value("color", json()));
  double op = p.value("opacidad", 100.0) / 100;
  for (int y = iy0; y <= iy1; ++y)
    for (int x = ix0; x <= ix1; ++x) {
      int X = px + x - ix0, Y = py + y - iy0;
      if (X < 0 || Y < 0 || X >= img.w || Y >= img.h) continue;
      double a = cov[(size_t)y * lw + x] / 255.0 * op;
      blend(img.at(X, Y), img.c, col.data(), a);
    }
}

Image rotate(const Image& src, double deg) {
  int d = (int)std::lround(deg);
  if (std::fabs(deg - d) < 1e-9 && (d == 90 || d == 180 || d == 270)) {
    Image o(d == 180 ? src.w : src.h, d == 180 ? src.h : src.w, src.c);
    for (int y = 0; y < src.h; ++y)
      for (int x = 0; x < src.w; ++x) {
        int nx = d == 90 ? src.h - 1 - y : d == 180 ? src.w - 1 - x : y;
        int ny = d == 90 ? x : d == 180 ? src.h - 1 - y : src.w - 1 - x;
        std::memcpy(o.at(nx, ny), src.at(x, y), (size_t)src.c);
      }
    return o;
  }
  Image s = to_rgba(src);   // como en GIMP: las esquinas nuevas quedan transparentes
  double a = deg * M_PI / 180, ca = std::cos(a), sa = std::sin(a);
  // tamano como Image.rotate(expand=True) de Pillow: esquinas giradas sobre el centro, ceil(max) - floor(min)
  auto r15 = [](double v) { return std::round(v * 1e15) / 1e15; };
  double mc = r15(ca), ms = r15(sa);
  double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
  for (auto [px, py] : {std::pair<double, double>{0, 0}, {(double)s.w, 0}, {(double)s.w, (double)s.h}, {0, (double)s.h}}) {
    double dx = px - s.w / 2.0, dy = py - s.h / 2.0;
    double tx = mc * dx - ms * dy + s.w / 2.0, ty = ms * dx + mc * dy + s.h / 2.0;
    xmin = std::min(xmin, tx), xmax = std::max(xmax, tx), ymin = std::min(ymin, ty), ymax = std::max(ymax, ty);
  }
  int W = (int)(std::ceil(xmax) - std::floor(xmin)), H = (int)(std::ceil(ymax) - std::floor(ymin));
  Image o(W, H, 4, 0);
  for (int i = 0; i < W * H; ++i) {
    o.px[(size_t)i * 4] = o.px[(size_t)i * 4 + 1] = o.px[(size_t)i * 4 + 2] = 255;
  }
  double cx = s.w / 2.0, cy = s.h / 2.0, ocx = W / 2.0, ocy = H / 2.0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      // giro horario: muestreo inverso
      double dx = x + 0.5 - ocx, dy = y + 0.5 - ocy;
      double sx = dx * ca + dy * sa + cx - 0.5, sy = -dx * sa + dy * ca + cy - 0.5;
      if (sx < -0.5 || sy < -0.5 || sx > s.w - 0.5 || sy > s.h - 0.5) continue;
      int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
      double fx = sx - x0, fy = sy - y0;
      double acc[4] = {0, 0, 0, 0};
      for (int k = 0; k < 4; ++k) {
        int xx = std::clamp(x0 + (k & 1), 0, s.w - 1), yy = std::clamp(y0 + (k >> 1), 0, s.h - 1);
        double wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
        const uint8_t* q = s.at(xx, yy);
        for (int c = 0; c < 4; ++c) acc[c] += q[c] * wgt;
      }
      uint8_t* t = o.at(x, y);
      for (int c = 0; c < 4; ++c) t[c] = clamp8(acc[c]);
    }
  return o;
}

std::pair<int, int> percentiles(const Image& img, double frac) {
  std::array<long long, 256> hist{};
  each_rgb(const_cast<Image&>(img), [&](uint8_t* p) { ++hist[lum(p)]; });
  long long total = (long long)img.w * img.h;
  double cut = total * frac;
  int lo = 0, hi = 255;
  long long acc = 0;
  for (int i = 0; i < 256; ++i) {
    acc += hist[i];
    if (acc > cut) {
      lo = i;
      break;
    }
  }
  acc = 0;
  for (int i = 255; i >= 0; --i) {
    acc += hist[i];
    if (acc > cut) {
      hi = i;
      break;
    }
  }
  return {lo, hi};
}

void rgb_to_hsv(const uint8_t* p, double& h, double& s, double& v) {
  double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
  double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  v = mx;
  s = mx > 0 ? d / mx : 0;
  if (d <= 0) h = 0;
  else if (mx == r) h = std::fmod((g - b) / d, 6.0);
  else if (mx == g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;
  h /= 6;
  if (h < 0) h += 1;
}

void hsv_to_rgb(double h, double s, double v, uint8_t* p) {
  double r, g, b;
  int i = (int)std::floor(h * 6) % 6;
  double f = h * 6 - std::floor(h * 6), pp = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch (i < 0 ? i + 6 : i) {
    case 0: r = v, g = t, b = pp; break;
    case 1: r = q, g = v, b = pp; break;
    case 2: r = pp, g = v, b = t; break;
    case 3: r = pp, g = q, b = v; break;
    case 4: r = t, g = pp, b = v; break;
    default: r = v, g = pp, b = q;
  }
  p[0] = clamp8(r * 255), p[1] = clamp8(g * 255), p[2] = clamp8(b * 255);
}

}  // namespace

// ======================================================================== API
Info probe(const fs::path& p) {
  Info info = probe_bytes(read_bytes(p));
  return info;
}

Image to_rgb(const Image& img) {
  if (img.c == 3) return img;
  Image o(img.w, img.h, 3);
  for (size_t i = 0, j = 0; i < img.px.size(); i += img.c, j += 3) std::memcpy(&o.px[j], &img.px[i], 3);
  return o;
}

Image to_rgba(const Image& img) {
  if (img.c == 4) return img;
  Image o(img.w, img.h, 4, 255);
  for (size_t i = 0, j = 0; i < img.px.size(); i += img.c, j += 4) std::memcpy(&o.px[j], &img.px[i], 3);
  return o;
}

Image load(const fs::path& p, Info* info_out, bool exif_transpose) {
  std::string data = read_bytes(p);
  Info info = probe_bytes(data);
  Image img;
  auto* d = (const uint8_t*)data.data();
  if (info.format == "WEBP") {
    int w, h;
    uint8_t* rgba = WebPDecodeRGBA(d, data.size(), &w, &h);
    if (!rgba) throw Error("WebP no valido");
    img = Image(w, h, 4);
    std::memcpy(img.px.data(), rgba, img.px.size());
    WebPFree(rgba);
    if (!info.transparency) img = to_rgb(img);
  } else if (info.format == "TIFF") {
    throw Error("el formato TIFF solo se puede leer con GIMP");
  } else {
    int w, h, n;
    uint8_t* raw = stbi_load_from_memory(d, (int)data.size(), &w, &h, &n, 0);
    if (!raw) throw Error(std::string("no se puede leer la imagen: ") + stbi_failure_reason());
    int c = (n == 2 || n == 4) ? 4 : 3;
    img = Image(w, h, c);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
      const uint8_t* s = raw + i * n;
      uint8_t* t = &img.px[i * c];
      if (n <= 2) {
        t[0] = t[1] = t[2] = s[0];
        if (c == 4) t[3] = s[1];
      } else {
        std::memcpy(t, s, (size_t)std::min(n, c));
      }
    }
    stbi_image_free(raw);
  }
  if (exif_transpose) img = transpose(img, info.orientation);
  if (info_out) *info_out = info;
  return img;
}

void save(const Image& img_in, const fs::path& path, const std::string& fmt, int quality,
          std::optional<std::pair<double, double>> dpi) {
  std::string out;
  Image img = img_in;
  if (fmt == "png") {
    stbi_write_png_compression_level = 9;
    stbi_write_png_to_func(write_cb, &out, img.w, img.h, img.c, img.px.data(), img.w * img.c);
    if (dpi && out.size() > 33) {   // pHYs tras IHDR
      std::string chunk = "pHYs";
      put32be(chunk, (uint32_t)std::lround(dpi->first / 0.0254));
      put32be(chunk, (uint32_t)std::lround(dpi->second / 0.0254));
      chunk += '\x01';
      std::string full;
      put32be(full, 9);
      full += chunk;
      put32be(full, crc_png(chunk));
      out.insert(33, full);
    }
  } else if (fmt == "jpg") {
    Image rgb = to_rgb(img);
    stbi_write_jpg_to_func(write_cb, &out, rgb.w, rgb.h, 3, rgb.px.data(), std::clamp(quality, 10, 100));
    if (dpi && out.size() > 18 && !std::memcmp(out.data() + 6, "JFIF", 4)) {
      out[13] = 1;
      int x = (int)std::lround(dpi->first), y = (int)std::lround(dpi->second);
      out[14] = (char)(x >> 8), out[15] = (char)(x & 255), out[16] = (char)(y >> 8), out[17] = (char)(y & 255);
    }
  } else if (fmt == "bmp") {
    Image rgb = to_rgb(img);
    stbi_write_bmp_to_func(write_cb, &out, rgb.w, rgb.h, 3, rgb.px.data());
  } else if (fmt == "webp") {
    uint8_t* buf = nullptr;
    size_t n = img.c == 4 ? WebPEncodeRGBA(img.px.data(), img.w, img.h, img.w * 4, (float)quality, &buf)
                          : WebPEncodeRGB(img.px.data(), img.w, img.h, img.w * 3, (float)quality, &buf);
    if (!n) throw Error("no se pudo codificar WebP");
    out.assign((const char*)buf, n);
    WebPFree(buf);
  } else if (fmt == "tif") {
    out = tiff_bytes(img, dpi);
  } else if (fmt == "xcf") {
    throw Error("el formato XCF (proyecto de GIMP) necesita GIMP");
  } else {
    throw Error("formato de salida no admitido: " + fmt);
  }
  if (out.empty() || !write_file(path, out)) throw Error("no se pudo guardar " + S(path.filename()));
}

Image apply(Image img, const json& prims, std::optional<std::pair<double, double>>& dpi) {
  for (auto& p : prims) {
    std::string n = jstr(p, "op");
    if (n == "escalar") {
      img = resize(img, p["w"].get<int>(), p["h"].get<int>());
    } else if (n == "recortar") {
      int x = p["x"], y = p["y"], w = p["w"], h = p["h"];
      Image o(w, h, img.c);
      for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
          if (x + xx < img.w && y + yy < img.h) std::memcpy(o.at(xx, yy), img.at(x + xx, y + yy), (size_t)img.c);
      img = std::move(o);
    } else if (n == "lienzo") {
      auto col = rgba_of(p["color"]);
      Image bg(p["w"].get<int>(), p["h"].get<int>(), img.c);
      for (size_t i = 0; i < bg.px.size(); i += bg.c) std::memcpy(&bg.px[i], col.data(), (size_t)bg.c);
      int ox = p["x"], oy = p["y"];
      for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
          if (x + ox >= bg.w || y + oy >= bg.h) continue;
          const uint8_t* s = img.at(x, y);
          if (img.c == 4) blend(bg.at(x + ox, y + oy), 4, s, s[3] / 255.0);
          else std::memcpy(bg.at(x + ox, y + oy), s, 3);
        }
      img = std::move(bg);
    } else if (n == "rotar") {
      img = rotate(img, p["grados"].get<double>());
    } else if (n == "voltear") {
      bool hz = jstr(p, "direccion") == "horizontal";
      Image o = img;
      for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
          std::memcpy(o.at(hz ? img.w - 1 - x : x, hz ? y : img.h - 1 - y), img.at(x, y), (size_t)img.c);
      img = std::move(o);
    } else if (n == "brillo_contraste") {
      double b = 1 + p["brillo"].get<double>() / 100, c = 1 + p["contraste"].get<double>() / 100;
      point(img, [&](int v) { return (int)std::lround(v * b); });
      double sum = 0;
      each_rgb(img, [&](uint8_t* q) { sum += lum(q); });
      double mean = std::floor(sum / std::max(1, img.w * img.h) + 0.5);
      point(img, [&](int v) { return (int)std::lround(mean + (v - mean) * c); });
    } else if (n == "niveles_auto") {
      auto [lo, hi] = percentiles(img, 0.005);
      if (hi - lo >= 2) {
        double k = 255.0 / (hi - lo);
        point(img, [&](int v) { return (int)std::lround((v - lo) * k); });
      }
    } else if (n == "balance_blancos") {   // ImageOps.autocontrast(cutoff=0.5) por canal
      std::array<std::array<long long, 256>, 3> hist{};
      each_rgb(img, [&](uint8_t* q) {
        for (int k = 0; k < 3; ++k) ++hist[k][q[k]];
      });
      std::array<std::array<uint8_t, 256>, 3> luts{};
      long long total = (long long)img.w * img.h;
      for (int k = 0; k < 3; ++k) {
        long long cut = (long long)(total * 0.005), acc = 0;
        int lo = 0, hi = 255;
        for (lo = 0; lo < 256; ++lo) {
          acc += hist[k][lo];
          if (acc > cut) break;
        }
        acc = 0;
        for (hi = 255; hi >= 0; --hi) {
          acc += hist[k][hi];
          if (acc > cut) break;
        }
        for (int v = 0; v < 256; ++v)
          luts[k][v] = hi <= lo ? (uint8_t)v : (uint8_t)std::clamp((int)((v - lo) * 255.0 / (hi - lo)), 0, 255);
      }
      point_ch(img, luts);
    } else if (n == "saturacion") {
      double f = 1 + p["cantidad"].get<double>() / 100;
      each_rgb(img, [&](uint8_t* q) {
        int l = lum(q);
        for (int k = 0; k < 3; ++k) q[k] = clamp8(l + (q[k] - l) * f);
      });
    } else if (n == "tono") {
      double shift = std::lround(p["grados"].get<double>() / 360 * 256) / 256.0;
      each_rgb(img, [&](uint8_t* q) {
        double h, s, v;
        rgb_to_hsv(q, h, s, v);
        h = std::fmod(h + shift + 1, 1.0);
        hsv_to_rgb(h, s, v, q);
      });
    } else if (n == "gamma") {
      double g = 1.0 / p["valor"].get<double>();
      point(img, [&](int v) { return (int)std::lround(255 * std::pow(v / 255.0, g)); });
    } else if (n == "blanco_negro") {
      each_rgb(img, [](uint8_t* q) { q[0] = q[1] = q[2] = (uint8_t)lum(q); });
    } else if (n == "sepia") {   // ImageOps.colorize(gris, "#1e140a", "#fff2dc", mid="#a07850")
      const double B[3] = {0x1e, 0x14, 0x0a}, M[3] = {0xa0, 0x78, 0x50}, W[3] = {0xff, 0xf2, 0xdc};
      each_rgb(img, [&](uint8_t* q) {
        int l = lum(q);
        for (int k = 0; k < 3; ++k)
          q[k] = l <= 127 ? clamp8(B[k] + (M[k] - B[k]) * l / 127.0) : clamp8(M[k] + (W[k] - M[k]) * (l - 127) / 128.0);
      });
    } else if (n == "enfocar") {
      double radio = p["radio"], amount = p["cantidad"];
      int thr = p["umbral"];
      Image bl = blurred(img, radio);
      for (size_t i = 0; i < img.px.size(); i += img.c)
        for (int k = 0; k < 3; ++k) {
          int diff = img.px[i + k] - bl.px[i + k];
          if (std::abs(diff) >= thr) img.px[i + k] = clamp8(img.px[i + k] + diff * amount);
        }
    } else if (n == "desenfocar") {
      img = blurred(img, p["radio"].get<double>() / 3.33);
    } else if (n == "reducir_ruido") {
      int size = p["fuerza"].get<int>() <= 5 ? 3 : 5, r = size / 2;
      Image o = img;
      std::vector<uint8_t> win;
      for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
          for (int k = 0; k < 3; ++k) {
            win.clear();
            for (int dy = -r; dy <= r; ++dy)
              for (int dx = -r; dx <= r; ++dx)
                win.push_back(img.at(std::clamp(x + dx, 0, img.w - 1), std::clamp(y + dy, 0, img.h - 1))[k]);
            std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
            o.at(x, y)[k] = win[win.size() / 2];
          }
      img = std::move(o);
    } else if (n == "invertir") {
      point(img, [](int v) { return 255 - v; });
    } else if (n == "posterizar") {
      double step = 255.0 / (p["niveles"].get<int>() - 1);
      point(img, [&](int v) { return (int)std::lround(std::round(v / step) * step); });
    } else if (n == "vineta") {
      int W = img.w, H = img.h;
      std::vector<float> mask((size_t)W * H, 255);
      double cx = W / 2.0, cy = H / 2.0, rx = W * 0.46, ry = H * 0.46;
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
          if (dx * dx + dy * dy <= 1) mask[(size_t)y * W + x] = 0;
        }
      gaussian(mask, W, H, 1, 0.4 * std::min(W, H) / 2);
      double f = p["fuerza"].get<double>() / 100;
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          double m = std::round(mask[(size_t)y * W + x] * f) / 255.0;
          uint8_t* q = img.at(x, y);
          for (int k = 0; k < 3; ++k) q[k] = clamp8(q[k] * (1 - m));
          if (img.c == 4) q[3] = clamp8(q[3] * (1 - m) + 255 * m);
        }
    } else if (n == "texto") {
      draw_text(img, p);
    } else if (n == "superponer") {
      Image ov = to_rgba(resize(to_rgba(load(P(jstr(p, "ruta")))), p["w"].get<int>(), p["h"].get<int>()));
      double op = p["opacidad"].get<double>() / 100;
      int ox = std::max(0, p["x"].get<int>()), oy = std::max(0, p["y"].get<int>());
      for (int y = 0; y < ov.h; ++y)
        for (int x = 0; x < ov.w; ++x) {
          if (x + ox >= img.w || y + oy >= img.h) continue;
          const uint8_t* s = ov.at(x, y);
          blend(img.at(x + ox, y + oy), img.c, s, s[3] / 255.0 * op);
        }
    } else if (n == "ppp") {
      double v = p["valor"];
      dpi = std::make_pair(v, v);
    } else if (n == "aplanar") {
      if (img.c == 4) {
        auto col = rgba_of(p["color"]);
        Image o(img.w, img.h, 3);
        for (size_t i = 0, j = 0; i < img.px.size(); i += 4, j += 3) {
          double a = img.px[i + 3] / 255.0;
          for (int k = 0; k < 3; ++k) o.px[j + k] = clamp8(img.px[i + k] * a + col[k] * (1 - a));
        }
        img = std::move(o);
      }
    }
  }
  return img;
}

std::string load_font(const std::string& name) { return find_font(name); }

double text_length(const std::string& font, const std::string& text, int size) {
  stbtt_fontinfo fi;
  int off = stbtt_GetFontOffsetForIndex((const unsigned char*)font.data(), 0);
  if (off < 0 || !stbtt_InitFont(&fi, (const unsigned char*)font.data(), off)) return 0;
  float scale = stbtt_ScaleForMappingEmToPixels(&fi, (float)size);
  std::u32string t = u32(text);
  double x = 0;
  for (size_t i = 0; i < t.size(); ++i) {
    int g = stbtt_FindGlyphIndex(&fi, (int)t[i]), adv, lsb;
    stbtt_GetGlyphHMetrics(&fi, g, &adv, &lsb);
    x += adv * scale;
    if (i + 1 < t.size()) x += scale * stbtt_GetGlyphKernAdvance(&fi, g, stbtt_FindGlyphIndex(&fi, (int)t[i + 1]));
  }
  return x;
}

void text_gray(std::vector<uint8_t>& canvas, int w, int h, int x, int y, const std::string& font, const std::string& text,
               int size, uint8_t ink) {
  stbtt_fontinfo fi;
  int off = stbtt_GetFontOffsetForIndex((const unsigned char*)font.data(), 0);
  if (off < 0 || !stbtt_InitFont(&fi, (const unsigned char*)font.data(), off)) return;
  float scale = stbtt_ScaleForMappingEmToPixels(&fi, (float)size);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
  int baseline = y + (int)std::lround(asc * scale);
  std::u32string t = u32(text);
  float xpos = (float)x;
  for (size_t i = 0; i < t.size(); ++i) {
    int g = stbtt_FindGlyphIndex(&fi, (int)t[i]), adv, lsb;
    float sub = xpos - std::floor(xpos);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&fi, g, scale, scale, sub, 0, &x0, &y0, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    if (bw > 0 && bh > 0) {
      std::vector<uint8_t> bm((size_t)bw * bh);
      stbtt_MakeGlyphBitmapSubpixel(&fi, bm.data(), bw, bh, bw, scale, scale, sub, 0, g);
      for (int yy = 0; yy < bh; ++yy)
        for (int xx = 0; xx < bw; ++xx) {
          int X = (int)std::floor(xpos) + x0 + xx, Y = baseline + y0 + yy;
          if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
          double a = bm[(size_t)yy * bw + xx] / 255.0;
          auto& d = canvas[(size_t)Y * w + X];
          d = (uint8_t)std::lround(d * (1 - a) + ink * a);
        }
    }
    stbtt_GetGlyphHMetrics(&fi, g, &adv, &lsb);
    xpos += adv * scale;
    if (i + 1 < t.size()) xpos += scale * stbtt_GetGlyphKernAdvance(&fi, g, stbtt_FindGlyphIndex(&fi, (int)t[i + 1]));
  }
}

}  // namespace nx::raster
