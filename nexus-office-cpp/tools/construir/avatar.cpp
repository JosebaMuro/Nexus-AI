// Recursos del avatar 3D «geisha» a partir de la hoja de personaje (recursos/web/assets/geisha):
//
//   nexus-construir avatar [hoja.jpg] [--salida carpeta]
//
// 1. Recorta los planos de la hoja (frente, perfiles, nuca, cuerpo de frente y de espaldas) y los amplia al doble.
// 2. Segmenta cada plano separando la figura del fondo.
// 3. Mide las siluetas fila a fila y guarda un «rig» JSON: con la anchura (vista frontal) y la profundidad (vista de
//    perfil) de cada altura el navegador construye un casco visual 3D.
// 4. Exporta las texturas RGBA (fondo transparente) que el shader proyecta desde cada vista.
#include <algorithm>
#include <cmath>
#include <optional>

#include "comun.h"
#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include "util/text.h"

namespace nc {
namespace {

struct Box {
  int x0, y0, x1, y1;
};
const std::vector<std::pair<std::string, Box>> SHEET_BOXES = {
    {"face_front", {0, 0, 282, 350}},      {"background", {286, 0, 560, 350}},  {"profile_left", {0, 372, 184, 628}},
    {"profile_right", {189, 372, 350, 628}}, {"back_head", {378, 372, 560, 628}}, {"body_front", {2, 1393, 279, 1851}},
    {"body_back", {288, 1393, 558, 1851}},
};
const char* VIEWS[] = {"face_front", "profile_left", "profile_right", "back_head", "body_front", "body_back"};

struct Img {
  int w = 0, h = 0;
  std::vector<uint8_t> px;   // RGB
};

Img load(const fs::path& p) {
  auto data = nx::read_file(p);
  if (!data) throw Fallo("no puedo leer " + nx::S(p));
  Img im;
  int n = 0;
  uint8_t* d = stbi_load_from_memory((const uint8_t*)data->data(), (int)data->size(), &im.w, &im.h, &n, 3);
  if (!d) throw Fallo("no es una imagen: " + nx::S(p));
  im.px.assign(d, d + (size_t)im.w * im.h * 3);
  stbi_image_free(d);
  return im;
}

// Recorte ampliado al doble (Catmull-Rom, nitido como Lanczos)
Img crop2x(const Img& src, const Box& b) {
  int cw = std::min(b.x1, src.w) - b.x0, ch = std::min(b.y1, src.h) - b.y0;
  if (cw <= 0 || ch <= 0) throw Fallo("la hoja es mas pequena que los recortes esperados (560x1851)");
  std::vector<uint8_t> c((size_t)cw * ch * 3);
  for (int y = 0; y < ch; ++y)
    std::copy_n(&src.px[((size_t)(b.y0 + y) * src.w + b.x0) * 3], (size_t)cw * 3, &c[(size_t)y * cw * 3]);
  Img out;
  out.w = (b.x1 - b.x0) * 2;
  out.h = (b.y1 - b.y0) * 2;
  out.px.resize((size_t)out.w * out.h * 3);
  stbir_resize_uint8_generic(c.data(), cw, ch, 0, out.px.data(), out.w, out.h, 0, 3, STBIR_ALPHA_CHANNEL_NONE, 0,
                             STBIR_EDGE_CLAMP, STBIR_FILTER_CATMULLROM, STBIR_COLORSPACE_SRGB, nullptr);
  return out;
}

void write_jpg(const fs::path& p, const Img& im, int q) {
  fs::create_directories(p.parent_path());
  if (!stbi_write_jpg(nx::S(p).c_str(), im.w, im.h, 3, im.px.data(), q)) throw Fallo("no puedo escribir " + nx::S(p));
}

using Mask = std::vector<std::vector<bool>>;
using Row = std::optional<std::pair<int, int>>;

double percentile(std::vector<double> v, double q) {   // como numpy (interpolacion lineal)
  std::sort(v.begin(), v.end());
  double i = q / 100.0 * (v.size() - 1);
  size_t lo = (size_t)std::floor(i), hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (i - lo);
}
double median(std::vector<double> v) { return percentile(std::move(v), 50); }

// Silueta solida de la figura. El fondo es gris poco saturado con vineta vertical: se modela por fila con los pixeles
// de los bordes y se clasifica como figura lo muy oscuro (pelo, kimono), lo claro (piel, naipes) o lo saturado (rojos).
// Cada fila se rellena entre sus extremos y los extremos se suavizan.
Mask segment(const Img& im) {
  int w = im.w, h = im.h;
  std::vector<double> lum((size_t)w * h), sat((size_t)w * h);
  for (size_t i = 0; i < lum.size(); ++i) {
    double r = im.px[i * 3], g = im.px[i * 3 + 1], b = im.px[i * 3 + 2];
    lum[i] = (r + g + b) / 3;
    sat[i] = std::max({r, g, b}) - std::min({r, g, b});
  }
  int band = std::max(12, (int)(w * 0.16));   // bandas exteriores; percentil alto porque la vineta aclara al centro
  std::vector<double> bg(h);
  for (int y = 0; y < h; ++y) {
    std::vector<double> edge;
    for (int x = 4; x < band; ++x) edge.push_back(lum[(size_t)y * w + x]);
    for (int x = w - band; x < w - 4; ++x) edge.push_back(lum[(size_t)y * w + x]);
    bg[y] = percentile(edge, 80);
  }
  const int k = 31;   // suavizado vertical robusto del fondo
  std::vector<double> sm_bg(h);
  for (int y = 0; y < h; ++y) {
    std::vector<double> win;
    for (int j = y - k / 2; j <= y + k / 2; ++j) win.push_back(bg[std::clamp(j, 0, h - 1)]);
    sm_bg[y] = std::clamp(median(win), 12.0, 70.0);
  }
  Mask fg(h, std::vector<bool>(w));
  for (int y = 0; y < h; ++y)
    for (int x = 3; x < w - 3; ++x) {
      size_t i = (size_t)y * w + x;
      fg[y][x] = lum[i] < sm_bg[y] * 0.55 || lum[i] > sm_bg[y] + 24 || sat[i] > 34;
    }
  std::vector<Row> rows(h);
  double cx = w / 2.0;
  for (int y = 0; y < h; ++y) {
    std::vector<int> xs;
    for (int x = 0; x < w; ++x)
      if (fg[y][x]) xs.push_back(x);
    if (xs.size() < 6) continue;
    struct Seg {
      int s, e, n;
    };
    std::vector<Seg> segs;
    size_t start = 0;
    for (size_t i = 1; i <= xs.size(); ++i)
      if (i == xs.size() || xs[i] - xs[i - 1] > 14) {
        int n = 0;
        for (int x = xs[start]; x <= xs[i - 1]; ++x) n += fg[y][x];
        if (n >= 4) segs.push_back({xs[start], xs[i - 1], n});
        start = i;
      }
    if (segs.empty()) continue;
    // se une el segmento central con los vecinos importantes (brazos y mangas)
    auto dist = [&](const Seg& s) { return s.s <= cx && cx <= s.e ? 0.0 : std::min(std::abs(s.s - cx), std::abs(s.e - cx)); };
    const Seg* main = &segs[0];
    for (auto& s : segs)
      if (dist(s) < dist(*main)) main = &s;
    int x0 = main->s, x1 = main->e;
    for (auto& s : segs)
      if (s.n > 25 && (std::abs(s.e - x0) < 40 || std::abs(s.s - x1) < 40)) x0 = std::min(x0, s.s), x1 = std::max(x1, s.e);
    rows[y] = std::make_pair(x0, x1);
  }
  // relleno de huecos verticales cortos y suavizado de extremos (mediana)
  auto med_rows = [&](int i, int r) -> std::pair<int, std::vector<double>> {
    std::vector<double> a, b;
    for (int j = std::max(0, i - r); j < std::min(h, i + r + 1); ++j)
      if (rows[j]) a.push_back(rows[j]->first), b.push_back(rows[j]->second);
    return {(int)a.size(), a.empty() ? std::vector<double>{} : std::vector<double>{median(a), median(b)}};
  };
  for (int i = 0; i < h; ++i)
    if (!rows[i]) {
      auto [n, m] = med_rows(i, 6);
      if (n > 3) rows[i] = std::make_pair((int)m[0], (int)m[1]);
    }
  std::vector<Row> sm(h);
  for (int i = 0; i < h; ++i) {
    auto [n, m] = med_rows(i, 13);
    if (rows[i] && n) sm[i] = std::make_pair((int)m[0], (int)m[1]);
  }
  Mask mask(h, std::vector<bool>(w));
  for (int y = 0; y < h; ++y)
    if (sm[y])
      for (int x = std::max(0, sm[y]->first); x <= std::min(w - 1, sm[y]->second); ++x) mask[y][x] = true;
  return mask;
}

// Textura RGBA: la mascara como alfa con el borde difuminado (gaussiano, sigma 1.5)
void write_rgba(const fs::path& p, const Img& im, const Mask& mask, double sigma = 1.5) {
  int w = im.w, h = im.h, r = (int)std::ceil(sigma * 3);
  std::vector<double> kern(2 * r + 1);
  double sum = 0;
  for (int i = -r; i <= r; ++i) sum += kern[i + r] = std::exp(-i * i / (2 * sigma * sigma));
  for (auto& v : kern) v /= sum;
  std::vector<double> a((size_t)w * h), t((size_t)w * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) a[(size_t)y * w + x] = mask[y][x] ? 255 : 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double s = 0;
      for (int i = -r; i <= r; ++i) s += kern[i + r] * a[(size_t)y * w + std::clamp(x + i, 0, w - 1)];
      t[(size_t)y * w + x] = s;
    }
  std::vector<uint8_t> out((size_t)w * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double s = 0;
      for (int i = -r; i <= r; ++i) s += kern[i + r] * t[(size_t)std::clamp(y + i, 0, h - 1) * w + x];
      size_t o = ((size_t)y * w + x) * 4, q = ((size_t)y * w + x) * 3;
      out[o] = im.px[q], out[o + 1] = im.px[q + 1], out[o + 2] = im.px[q + 2];
      out[o + 3] = (uint8_t)std::clamp((int)std::lround(s), 0, 255);
    }
  if (!stbi_write_png(nx::S(p).c_str(), w, h, 4, out.data(), w * 4)) throw Fallo("no puedo escribir " + nx::S(p));
}

std::vector<Row> extents(const Mask& m) {   // extremos izquierdo y derecho de cada fila
  std::vector<Row> rows(m.size());
  for (size_t y = 0; y < m.size(); ++y) {
    auto b = std::find(m[y].begin(), m[y].end(), true);
    if (b == m[y].end()) continue;
    auto e = std::find(m[y].rbegin(), m[y].rend(), true);
    rows[y] = std::make_pair((int)(b - m[y].begin()), (int)(m[y].rend() - e) - 1);
  }
  return rows;
}

// x0: apertura (quita picos hacia dentro); x1: cierre. Rellena muescas de menos de k filas. Solo por encima de limit
// (zona del pelo): por debajo hay estrechamientos reales (cuello).
std::vector<Row> fill_notches(std::vector<Row> rows, int limit, int k = 61) {
  std::vector<int> idx;
  for (int i = 0; i < (int)rows.size(); ++i)
    if (rows[i] && i < limit) idx.push_back(i);
  if (idx.empty()) return rows;
  std::vector<double> x0, x1;
  for (int i : idx) x0.push_back(rows[i]->first), x1.push_back(rows[i]->second);
  auto filt = [&](const std::vector<double>& v, bool take_min) {
    std::vector<double> out(v.size());
    int n = (int)v.size();
    for (int i = 0; i < n; ++i) {
      double r = v[std::clamp(i - k / 2, 0, n - 1)];
      for (int j = i - k / 2; j <= i + k / 2; ++j) r = take_min ? std::min(r, v[std::clamp(j, 0, n - 1)]) : std::max(r, v[std::clamp(j, 0, n - 1)]);
      out[i] = r;
    }
    return out;
  };
  x0 = filt(filt(x0, true), false);
  x1 = filt(filt(x1, false), true);
  for (size_t j = 0; j < idx.size(); ++j) rows[idx[j]] = std::make_pair((int)x0[j], (int)x1[j]);
  return rows;
}

json sample(const std::vector<Row>& rows, int n, int y0, int y1) {
  json out = json::array();
  for (int i = 0; i < n; ++i) {
    int y = (int)(y0 + (double)(y1 - y0) * i / (n - 1));
    Row r = rows[y];
    for (int d = 1; !r && d < 40; ++d)
      if (y + d < (int)rows.size() && rows[y + d]) r = rows[y + d];
    out.push_back(r ? json{y, r->first, r->second} : json{y, nullptr, nullptr});
  }
  return out;
}

// ---------------------------------------------------------------- rig en coordenadas de mundo
// Referencias medidas sobre la hoja (pixeles de los recortes a 2x). Cabeza: coronilla (sin el adorno), barbilla y
// punto mas estrecho del cuello en cada vista. Cuerpo: cuello y suelo. Unidades de mundo: metros, suelo en y=0.
struct {
  double top = 62, chin = 510, neck = 530;
} H_FRONT;
struct {
  double top = 43, chin = 398, neck = 430;
} H_PROFILE;
struct {
  double top = 48, neck = 388;
} H_BACK;
const double B_NECK = 158, B_FLOOR = 910, B_NECK_WORLD = 1.40, B_HEAD_NECK_WORLD = 1.43;

std::optional<std::pair<double, double>> ext(const json& view, double py) {
  const json* best = nullptr;
  for (auto& r : view["rows"])
    if (!best || std::abs(r[0].get<double>() - py) < std::abs((*best)[0].get<double>() - py)) best = &r;
  if (!best || (*best)[1].is_null()) return std::nullopt;
  return std::make_pair((*best)[1].get<double>(), (*best)[2].get<double>());
}

double r4(double v) { return std::round(v * 1e4) / 1e4; }

json build_rig(const json& V) {
  const json &ff = V["face_front"], &pl = V["profile_left"], &bh = V["back_head"], &bf = V["body_front"], &bb = V["body_back"];
  // escala del cuerpo y de la cabeza (la anchura del pelo en el cuerpo fija la escala de la cabeza)
  double kb = B_NECK_WORLD / (B_FLOOR - B_NECK);
  auto max_width = [](const json& view, double below) {
    double m = 0;
    for (auto& r : view["rows"])
      if (!r[1].is_null() && r[0].get<double>() < below) m = std::max(m, r[2].get<double>() - r[1].get<double>());
    return m;
  };
  double kh = max_width(bf, B_NECK - 10) * kb / max_width(ff, H_FRONT.chin);
  double sp = (H_FRONT.chin - H_FRONT.top) / (H_PROFILE.chin - H_PROFILE.top);
  double sbk = (H_FRONT.neck - H_FRONT.top) / (H_BACK.neck - H_BACK.top);
  auto center = [&](const json& view, double py) {
    auto e = ext(view, py);
    if (!e) throw Fallo("no se encuentra el cuello en una vista de la hoja");
    return (e->first + e->second) / 2;
  };
  double xc = center(ff, H_FRONT.neck), zc = center(pl, H_PROFILE.neck), zc_r = center(V["profile_right"], H_PROFILE.neck);
  double xcb = center(bh, H_BACK.neck), yN = B_HEAD_NECK_WORLD;
  std::vector<std::vector<double>> head_rows;
  for (auto& r : ff["rows"]) {
    double py = r[0].get<double>();
    if (r[1].is_null() || py > H_FRONT.neck + 30) continue;
    auto pe = ext(pl, H_PROFILE.top + (py - H_FRONT.top) / sp);
    if (!pe) continue;
    double y = yN + (H_FRONT.neck - py) * kh;
    double x0 = (r[1].get<double>() - xc) * kh, x1 = (r[2].get<double>() - xc) * kh;
    double z1 = (zc - pe->first) * sp * kh, z0 = (zc - pe->second) * sp * kh;   // cara (izquierda en el perfil) hacia +z
    if (py < 75) {   // cresta del adorno superior: pieza plana en el frente del mono
      z0 = 0.0, z1 = 0.035;
    } else if (py < 330 && (z1 - z0) < 0.85 * (x1 - x0)) {   // pelo: el perfil recortado subestima el volumen
      double mid = (z0 + z1) / 2, half = 0.425 * (x1 - x0);
      z0 = std::min(z0, mid - half), z1 = std::max(z1, mid + half);
    }
    head_rows.push_back({r4(y), r4(x0), r4(x1), r4(z0), r4(z1)});
  }
  std::stable_sort(head_rows.begin(), head_rows.end(), [](auto& a, auto& b) { return a[0] > b[0]; });
  double xbf = center(bf, B_NECK), xbb = center(bb, B_NECK);
  std::vector<std::vector<double>> body_rows;
  for (auto& r : bf["rows"]) {
    double py = r[0].get<double>();
    if (r[1].is_null() || py < B_NECK - 6) continue;
    auto be = ext(bb, py);
    double x0 = (r[1].get<double>() - xbf) * kb, x1 = (r[2].get<double>() - xbf) * kb;
    if (be) x0 = std::min(x0, -(be->second - xbb) * kb), x1 = std::max(x1, -(be->first - xbb) * kb);   // espalda espejada
    double w = x1 - x0, y = (B_FLOOR - py) * kb;
    double half = std::min(0.17, std::max(0.05, 0.22 * w));
    if (y > 1.36) {   // el cuello del cuerpo queda dentro del cuello de la cabeza
      half = std::min(half, 0.034);
      x0 = std::max(x0, -0.042), x1 = std::min(x1, 0.042);
    }
    double back = -half;
    if (py >= 300 && py <= 430) back -= 0.07 * std::sin(3.14159265358979323846 * (py - 300) / 130);   // lazo del obi en la espalda
    body_rows.push_back({r4(y), r4(x0), r4(x1), r4(back), r4(half * 0.9)});
  }
  std::stable_sort(body_rows.begin(), body_rows.end(), [](auto& a, auto& b) { return a[0] > b[0]; });
  // u = h * a + b ;  v = y * c + d   (h: coordenada horizontal de la vista en metros)
  auto proj = [](const json& view, double scale_h, double center_px, double sv, double ref_py, double ref_y, bool flip) {
    double w = view["w"].get<double>(), h = view["h"].get<double>();
    double a = (1 / scale_h) / w * (flip ? -1 : 1), b = center_px / w, c = (1 / sv) / h, d = 1 - ref_py / h - ref_y * c;
    return json{a, b, c, d};
  };
  auto pt = [&](double px, double py) { return json{(px - xc) * kh, yN + (H_FRONT.neck - py) * kh}; };
  auto pin = [&](double ax, double ay, double bx, double by) {
    json a = pt(ax, ay), b = pt(bx, by);
    return json{a[0], a[1], b[0], b[1]};
  };
  return {
      {"k_head", kh},
      {"k_body", kb},
      {"head",
       {{"rows", head_rows},
        {"front", proj(ff, kh, xc, kh, H_FRONT.neck, yN, false)},
        {"left", proj(pl, sp * kh, zc, sp * kh, H_PROFILE.neck, yN, true)},   // h = z, la cara a la izquierda
        {"right", proj(V["profile_right"], sp * kh, zc_r, sp * kh, H_PROFILE.neck, yN, false)},
        {"back", proj(bh, sbk * kh, xcb, sbk * kh, H_BACK.neck, yN, true)},
        {"eyes", {pt(225, 352), pt(339, 352)}},
        {"mouth", pt(282, 470)},
        {"pins", {pin(45, 95, 165, 125), pin(520, 90, 400, 122), pin(45, 160, 170, 165), pin(520, 160, 395, 165)}}}},
      {"body",
       {{"rows", body_rows},
        {"front", proj(bf, kb, xbf, kb, B_FLOOR, 0.0, false)},
        {"back", proj(bb, kb, xbb, kb, B_FLOOR, 0.0, true)}}},
  };
}

}  // namespace

int avatar(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {}, {});
  fs::path out = a.tiene("salida") ? fs::path(a.val("salida")) : raiz() / "recursos" / "web" / "assets" / "geisha";
  fs::path sheet = a.pos.empty() ? out / "character_sheet.jpg" : fs::path(a.pos[0]);
  Img src = load(sheet);
  log("avatar", nx::cat("hoja ", nx::S(sheet), " (", src.w, "x", src.h, ")"));
  fs::create_directories(out);
  std::map<std::string, Img> crops;
  for (auto& [name, box] : SHEET_BOXES) crops[name] = crop2x(src, box);
  write_jpg(out / "background.jpg", crops["background"], 92);
  std::error_code ec;
  if (!fs::equivalent(sheet, out / "character_sheet.jpg", ec)) write_jpg(out / "character_sheet.jpg", src, 95);
  json views = json::object();
  for (const char* name : VIEWS) {
    const Img& im = crops[name];
    Mask mask = segment(im);
    write_rgba(out / (std::string(name) + ".png"), im, mask);
    int top = -1, bottom = -1;
    for (int y = 0; y < im.h; ++y)
      if (std::find(mask[y].begin(), mask[y].end(), true) != mask[y].end()) {
        if (top < 0) top = y;
        bottom = y;
      }
    if (top < 0) throw Fallo(std::string("no se encuentra la figura en ") + name);
    std::string n = name;
    bool head = n == "face_front" || n == "profile_left" || n == "profile_right" || n == "back_head";
    int limit = n == "face_front" ? 330 : n == "back_head" ? 250 : 230;
    auto rows = head ? fill_notches(extents(mask), limit) : extents(mask);
    views[n] = {{"w", im.w}, {"h", im.h}, {"top", top}, {"bottom", bottom}, {"rows", sample(rows, 96, top, bottom)}};
    log("avatar", nx::cat(n, " ", im.w, "x", im.h, " figura y=", top, "..", bottom));
  }
  json rig = build_rig(views);
  escribir(out / "rig.json", rig.dump());
  log("avatar", nx::cat("rig.json escrito: ", rig["head"]["rows"].size(), " anillos de cabeza, ", rig["body"]["rows"].size(),
                        " de cuerpo, altura ", nx::fnum(rig["head"]["rows"][0][0].get<double>(), 2), " m"));
  return 0;
}

}  // namespace nc
