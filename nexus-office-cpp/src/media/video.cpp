// Video a partir de imagenes (ver video.h): fotogramas compuestos en C++ y guardados como Motion JPEG en un AVI.
#include "media/video.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "stb_image.h"
#include "stb_image_write.h"
#include "util/text.h"

namespace nx::video {

namespace {

struct Img {
  int w = 0, h = 0;
  std::vector<uint8_t> px;   // RGB
};

Img load(const fs::path& p) {
  Img im;
  auto raw = read_file(p);
  if (!raw || raw->empty()) return im;
  int w = 0, h = 0, n = 0;
  unsigned char* d = stbi_load_from_memory((const unsigned char*)raw->data(), (int)raw->size(), &w, &h, &n, 3);
  if (!d) return im;
  im.w = w;
  im.h = h;
  im.px.assign(d, d + (size_t)w * h * 3);
  stbi_image_free(d);
  return im;
}

// encuadre «cubrir» con zoom y desplazamiento: progreso p de 0 a 1 a lo largo de los segundos de la imagen
struct View {
  double scale, cx, cy;
};

View view_of(const Img& im, const Opts& o, int index, double p) {
  double cover = std::max((double)o.w / im.w, (double)o.h / im.h);
  double zoom = 1.0;
  double cx = im.w / 2.0, cy = im.h / 2.0;
  if (o.kenburns) {
    // alterna acercar y alejar, y el sentido del desplazamiento, para que no parezca mecanico
    bool in = index % 2 == 0;
    double q = in ? p : 1.0 - p;
    zoom = 1.0 + 0.10 * q;
    double room_x = std::max(0.0, im.w - o.w / (cover * zoom)) / 2.0;
    double room_y = std::max(0.0, im.h - o.h / (cover * zoom)) / 2.0;
    double dir = (index % 4 < 2) ? 1.0 : -1.0;
    cx += dir * room_x * 0.6 * (p - 0.5) * 2.0;
    cy += (index % 3 == 0 ? -1.0 : 1.0) * room_y * 0.4 * (p - 0.5) * 2.0;
  }
  return {cover * zoom, cx, cy};
}

inline void sample(const Img& im, double sx, double sy, float out[3]) {
  sx = std::clamp(sx, 0.0, (double)im.w - 1.001);
  sy = std::clamp(sy, 0.0, (double)im.h - 1.001);
  int x0 = (int)sx, y0 = (int)sy;
  int x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
  float fx = (float)(sx - x0), fy = (float)(sy - y0);
  const uint8_t* a = &im.px[((size_t)y0 * im.w + x0) * 3];
  const uint8_t* b = &im.px[((size_t)y0 * im.w + x1) * 3];
  const uint8_t* c = &im.px[((size_t)y1 * im.w + x0) * 3];
  const uint8_t* d = &im.px[((size_t)y1 * im.w + x1) * 3];
  for (int k = 0; k < 3; ++k) {
    float top = a[k] + (b[k] - a[k]) * fx, bot = c[k] + (d[k] - c[k]) * fx;
    out[k] = top + (bot - top) * fy;
  }
}

void draw(const Img& im, const View& v, const Opts& o, float alpha, std::vector<float>& acc) {
  double inv = 1.0 / v.scale;
  float px[3];
  for (int y = 0; y < o.h; ++y) {
    double sy = (y - o.h / 2.0) * inv + v.cy;
    float* row = &acc[(size_t)y * o.w * 3];
    for (int x = 0; x < o.w; ++x) {
      double sx = (x - o.w / 2.0) * inv + v.cx;
      sample(im, sx, sy, px);
      for (int k = 0; k < 3; ++k) row[x * 3 + k] += alpha * px[k];
    }
  }
}

void sink(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, size); }

void put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 0xff));
}
void put16(std::string& s, uint16_t v) {
  s.push_back((char)(v & 0xff));
  s.push_back((char)(v >> 8));
}
void fourcc(std::string& s, const char* c) { s.append(c, 4); }

constexpr long HEADER = 224;     // RIFF + hdrl + LIST movi: tamano fijo, se reescribe al cerrar
constexpr long MOVI_FOURCC = 220;

}  // namespace

void formato(Opts& o, const std::string& nombre) {
  std::string n = lower(trim(nombre));
  if (n.empty() || n == "16:9" || n == "720p" || n == "horizontal") o.w = 1280, o.h = 720;
  else if (n == "1080p" || n == "full hd" || n == "fullhd") o.w = 1920, o.h = 1080;
  else if (n == "9:16" || n == "vertical" || n == "reels" || n == "stories" || n == "tiktok") o.w = 720, o.h = 1280;
  else if (n == "1:1" || n == "cuadrado" || n == "square") o.w = 1080, o.h = 1080;
  else if (n == "4:3") o.w = 1024, o.h = 768;
  else if (n == "480p") o.w = 854, o.h = 480;
}

AviWriter::AviWriter(const fs::path& out, int w, int h, int fps) : w_(w), h_(h), fps_(std::max(1, fps)) {
  mkdirs(out.parent_path());
#ifdef _WIN32
  f_ = _wfopen(out.wstring().c_str(), L"wb");
#else
  f_ = std::fopen(S(out).c_str(), "wb");
#endif
  if (!f_) return;
  std::string zero(HEADER, '\0');
  std::fwrite(zero.data(), 1, zero.size(), f_);
  movi_pos_ = HEADER;
}

AviWriter::~AviWriter() {
  if (f_) std::fclose(f_);
}

void AviWriter::frame(const std::string& jpeg) {
  if (!f_) return;
  std::string h;
  fourcc(h, "00dc");
  put32(h, (uint32_t)jpeg.size());
  index_.push_back({(uint32_t)(movi_pos_ - MOVI_FOURCC), (uint32_t)jpeg.size()});
  std::fwrite(h.data(), 1, h.size(), f_);
  std::fwrite(jpeg.data(), 1, jpeg.size(), f_);
  movi_pos_ += 8 + (long)jpeg.size();
  if (jpeg.size() % 2) {
    std::fputc(0, f_);
    ++movi_pos_;
  }
  max_frame_ = std::max<uint32_t>(max_frame_, (uint32_t)jpeg.size());
}

json AviWriter::close() {
  if (!f_) return nullptr;
  // indice: cada fotograma es un fotograma clave
  std::string idx;
  fourcc(idx, "idx1");
  put32(idx, (uint32_t)(index_.size() * 16));
  for (auto& [off, size] : index_) {
    fourcc(idx, "00dc");
    put32(idx, 0x10);
    put32(idx, off);
    put32(idx, size);
  }
  std::fwrite(idx.data(), 1, idx.size(), f_);
  long total = movi_pos_ + (long)idx.size();
  uint32_t frames = (uint32_t)index_.size();
  std::string h;
  fourcc(h, "RIFF");
  put32(h, (uint32_t)(total - 8));
  fourcc(h, "AVI ");
  fourcc(h, "LIST");
  put32(h, 192);
  fourcc(h, "hdrl");
  fourcc(h, "avih");
  put32(h, 56);
  put32(h, (uint32_t)(1000000 / fps_));
  put32(h, max_frame_ * (uint32_t)fps_);
  put32(h, 0);
  put32(h, 0x10);   // AVIF_HASINDEX
  put32(h, frames);
  put32(h, 0);
  put32(h, 1);
  put32(h, max_frame_ + 8);
  put32(h, (uint32_t)w_);
  put32(h, (uint32_t)h_);
  for (int i = 0; i < 4; ++i) put32(h, 0);
  fourcc(h, "LIST");
  put32(h, 116);
  fourcc(h, "strl");
  fourcc(h, "strh");
  put32(h, 56);
  fourcc(h, "vids");
  fourcc(h, "MJPG");
  put32(h, 0);
  put16(h, 0);
  put16(h, 0);
  put32(h, 0);
  put32(h, 1);
  put32(h, (uint32_t)fps_);
  put32(h, 0);
  put32(h, frames);
  put32(h, max_frame_ + 8);
  put32(h, 0xFFFFFFFF);
  put32(h, 0);
  put16(h, 0);
  put16(h, 0);
  put16(h, (uint16_t)w_);
  put16(h, (uint16_t)h_);
  fourcc(h, "strf");
  put32(h, 40);
  put32(h, 40);
  put32(h, (uint32_t)w_);
  put32(h, (uint32_t)h_);
  put16(h, 1);
  put16(h, 24);
  fourcc(h, "MJPG");
  put32(h, (uint32_t)(w_ * h_ * 3));
  for (int i = 0; i < 4; ++i) put32(h, 0);
  fourcc(h, "LIST");
  put32(h, (uint32_t)(movi_pos_ - MOVI_FOURCC));
  fourcc(h, "movi");
  std::fseek(f_, 0, SEEK_SET);
  std::fwrite(h.data(), 1, h.size(), f_);
  std::fclose(f_);
  f_ = nullptr;
  return {{"frames", frames}, {"bytes", total}};
}

json render_avi(const std::vector<fs::path>& paths_in, const Opts& o_in, const fs::path& out) {
  Opts o = o_in;
  o.w = std::clamp(o.w / 2 * 2, 64, 3840);
  o.h = std::clamp(o.h / 2 * 2, 64, 3840);
  o.fps = std::clamp(o.fps, 1, 60);
  o.segundos = std::clamp(o.segundos, 0.2, 60.0);
  o.transicion = std::clamp(o.transicion, 0.0, o.segundos * 0.5);
  std::vector<Img> imgs;
  for (auto& p : paths_in) {
    Img im = load(p);
    if (im.w > 0) imgs.push_back(std::move(im));
  }
  if (imgs.empty()) throw std::runtime_error("ninguna imagen legible (PNG, JPEG, BMP, GIF o TGA)");
  AviWriter avi(out, o.w, o.h, o.fps);
  if (!avi.ok()) throw std::runtime_error("no se puede escribir " + S(out));
  const int per = std::max(1, (int)std::lround(o.segundos * o.fps));
  const int fade = (int)std::lround(o.transicion * o.fps);
  const int n = (int)imgs.size();
  std::vector<float> acc((size_t)o.w * o.h * 3);
  std::vector<uint8_t> rgb(acc.size());
  std::string jpg;
  for (int i = 0; i < n; ++i) {
    for (int f = 0; f < per; ++f) {
      std::fill(acc.begin(), acc.end(), 0.f);
      double p = per > 1 ? (double)f / (per - 1) : 0.0;
      int into_fade = f - (per - fade);
      if (fade > 0 && i + 1 < n && into_fade >= 0) {
        // fundido con el principio de la siguiente imagen
        float a = (float)(into_fade + 1) / (fade + 1);
        draw(imgs[i], view_of(imgs[i], o, i, p), o, 1.f - a, acc);
        draw(imgs[i + 1], view_of(imgs[i + 1], o, i + 1, 0.0), o, a, acc);
      } else {
        // la imagen siguiente arranca donde termino el fundido: el progreso del zoom continua
        draw(imgs[i], view_of(imgs[i], o, i, p), o, 1.f, acc);
      }
      for (size_t k = 0; k < acc.size(); ++k) rgb[k] = (uint8_t)std::clamp(acc[k] + 0.5f, 0.f, 255.f);
      jpg.clear();
      stbi_write_jpg_to_func(sink, &jpg, o.w, o.h, 3, rgb.data(), std::clamp(o.calidad, 30, 100));
      avi.frame(jpg);
    }
  }
  json r = avi.close();
  r["width"] = o.w;
  r["height"] = o.h;
  r["fps"] = o.fps;
  r["images"] = n;
  r["seconds"] = round_to((double)r["frames"].get<uint32_t>() / o.fps, 1);
  return r;
}

}  // namespace nx::video
