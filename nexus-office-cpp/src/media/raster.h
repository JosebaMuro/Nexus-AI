// Edicion de imagen sin GIMP (lo que hace Pillow en media/gimp.py): leer y guardar PNG, JPEG, WebP, BMP, GIF, TIFF
// (solo escritura), EXIF y ppp, y las operaciones de imagen_editar con la misma semantica que las de Pillow.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx::raster {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Image {
  int w = 0, h = 0, c = 3;   // canales: 3 (RGB) o 4 (RGBA)
  std::vector<uint8_t> px;
  Image() = default;
  Image(int w_, int h_, int c_, uint8_t fill = 0) : w(w_), h(h_), c(c_), px((size_t)w_ * h_ * c_, fill) {}
  uint8_t* at(int x, int y) { return px.data() + ((size_t)y * w + x) * c; }
  const uint8_t* at(int x, int y) const { return px.data() + ((size_t)y * w + x) * c; }
  bool alpha() const { return c == 4; }
};

struct Info {
  std::string format;   // PNG, JPEG, WEBP, GIF, BMP, TIFF, PSD...
  std::string mode;     // RGB, RGBA, L, LA, P...
  int w = 0, h = 0;
  bool transparency = false;
  std::optional<std::pair<double, double>> dpi;
  int orientation = 1;
  std::map<int, std::string> exif;   // etiqueta -> texto (ASCII y enteros de la IFD0)
};

Info probe(const fs::path& p);          // sin decodificar los pixeles (lanza Error si no lo reconoce)
Image load(const fs::path& p, Info* info = nullptr, bool exif_transpose = true);   // girada segun su EXIF
// fmt: png, jpg, webp, bmp, tif (lanza Error con xcf u otros)
void save(const Image& img, const fs::path& p, const std::string& fmt, int quality = 90,
          std::optional<std::pair<double, double>> dpi = std::nullopt);

Image to_rgb(const Image& img);
Image to_rgba(const Image& img);

// primitivas (ya resueltas a pixeles por media::gimp::resolve)
Image apply(Image img, const json& prim, std::optional<std::pair<double, double>>& dpi);

// texto sobre un lienzo en escala de grises (1 byte por pixel), como ImageDraw.text de Pillow con el ancla en la
// esquina superior izquierda del ascendente; font son los bytes de una TrueType (load_font)
std::string load_font(const std::string& name = "");   // name o la primera fuente del sistema (lanza Error si no hay)
double text_length(const std::string& font, const std::string& text, int size);
void text_gray(std::vector<uint8_t>& canvas, int w, int h, int x, int y, const std::string& font, const std::string& text,
               int size, uint8_t ink);

}  // namespace nx::raster
