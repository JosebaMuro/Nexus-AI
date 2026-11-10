// Compositor de escenas (media/scene.py): mismas palabras, colores, tamanos y composicion que la edicion Python.
// Se dibuja al doble de tamano y se reduce (bordes suaves); el brillo de soles, ventanas y faros va en una capa
// aparte que se desenfoca y se suma.
#include "media/scene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

#include "stb_image_resize.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::media::scene {

namespace {

using Pt = std::pair<double, double>;

const std::map<std::string, Rgb> COLORS = {
    {"rojo", {210, 40, 45}},       {"roja", {210, 40, 45}},       {"red", {210, 40, 45}},
    {"azul", {40, 90, 210}},       {"blue", {40, 90, 210}},       {"verde", {40, 160, 70}},
    {"green", {40, 160, 70}},      {"amarillo", {245, 205, 40}},  {"amarilla", {245, 205, 40}},
    {"yellow", {245, 205, 40}},    {"naranja", {245, 130, 30}},   {"orange", {245, 130, 30}},
    {"morado", {130, 60, 190}},    {"morada", {130, 60, 190}},    {"purple", {130, 60, 190}},
    {"violeta", {150, 80, 210}},   {"violet", {150, 80, 210}},    {"rosa", {240, 110, 170}},
    {"pink", {240, 110, 170}},     {"negro", {28, 28, 32}},       {"negra", {28, 28, 32}},
    {"black", {28, 28, 32}},       {"blanco", {240, 240, 235}},   {"blanca", {240, 240, 235}},
    {"white", {240, 240, 235}},    {"gris", {130, 130, 135}},     {"gray", {130, 130, 135}},
    {"grey", {130, 130, 135}},     {"marron", {120, 75, 40}},     {"brown", {120, 75, 40}},
    {"dorado", {230, 185, 60}},    {"dorada", {230, 185, 60}},    {"gold", {230, 185, 60}},
    {"golden", {230, 185, 60}},    {"plateado", {190, 195, 205}}, {"plateada", {190, 195, 205}},
    {"silver", {190, 195, 205}},   {"cian", {0, 200, 220}},       {"cyan", {0, 200, 220}},
    {"turquesa", {30, 200, 190}},  {"turquoise", {30, 200, 190}},
};

struct Hit {
  std::string cat, key;
};

// palabra -> (categoria, clave). Plurales: se prueba tambien sin la «s»/«es» final.
const std::map<std::string, Hit> WORDS = [] {
  std::map<std::string, Hit> m;
  auto add = [&](const std::string& cat, const std::string& key, std::initializer_list<const char*> ws) {
    for (auto* w : ws) m[w] = {cat, key};
  };
  // hora del dia
  add("time", "night", {"noche", "night", "nocturno", "nocturna", "medianoche", "midnight"});
  add("sky", "moon", {"luna", "moon"});
  add("time", "sunset", {"atardecer", "sunset", "ocaso", "crepusculo", "dusk"});
  add("time", "sunrise", {"amanecer", "sunrise", "alba", "dawn"});
  add("time", "day", {"dia", "day", "mediodia", "soleado", "sunny"});
  // clima
  add("weather", "rain", {"lluvia", "rain", "lloviendo", "rainy"});
  add("weather", "snow", {"nieve", "snow", "nevando", "snowy", "invierno", "winter"});
  add("weather", "fog", {"niebla", "fog", "bruma", "mist", "foggy"});
  add("weather", "storm", {"tormenta", "storm", "rayo", "lightning"});
  add("sky", "cloud", {"nube", "cloud", "nublado", "cloudy"});
  // lugar
  add("place", "sea", {"mar", "sea", "oceano", "ocean", "ola", "wave"});
  add("place", "beach", {"playa", "beach", "costa", "coast", "arena"});
  add("place", "mountain", {"montana", "mountain", "monte", "sierra", "pico", "peak", "volcan"});
  add("place", "forest", {"bosque", "forest", "selva", "jungle", "woods"});
  add("place", "city", {"ciudad", "city", "edificio", "building", "rascacielo", "skyscraper", "calle", "street",
                        "urbano", "urban", "town"});
  add("place", "space", {"espacio", "space", "galaxia", "galaxy", "universo", "universe", "nebulosa", "nebula", "cosmos"});
  add("place", "desert", {"desierto", "desert", "duna", "dune"});
  add("place", "lake", {"lago", "lake", "rio", "river", "estanque", "pond"});
  add("place", "garden", {"jardin", "garden", "parque", "park"});
  add("place", "field", {"campo", "field", "prado", "meadow", "pradera", "colina", "hill"});
  // cielo
  add("sky", "sun", {"sol", "sun"});
  add("sky", "star", {"estrella", "star", "estrellado", "starry"});
  add("sky", "planet", {"planeta", "planet"});
  add("subj", "bird", {"pajaro", "bird", "ave", "gaviota", "seagull"});
  add("subj", "rocket", {"cohete", "rocket", "nave", "spaceship"});
  add("subj", "ufo", {"ovni", "ufo"});
  // sujetos
  add("subj", "cat", {"gato", "gata", "gatito", "cat", "kitten"});
  add("subj", "dog", {"perro", "perra", "perrito", "cachorro", "dog", "puppy"});
  add("subj", "house", {"casa", "house", "cabana", "cabin", "hogar", "home"});
  add("subj", "castle", {"castillo", "castle", "palacio", "palace"});
  add("subj", "tree", {"arbol", "tree"});
  add("subj", "pine", {"pino", "pine"});
  add("subj", "flower", {"flor", "flower", "rosa_flor", "tulipan", "tulip", "girasol", "sunflower"});
  add("subj", "car", {"coche", "car", "auto", "carro", "automovil"});
  add("subj", "boat", {"barco", "boat", "velero", "sailboat", "ship", "barca"});
  add("subj", "fish", {"pez", "peces", "fish"});
  add("subj", "person", {"persona", "person", "gente", "people", "hombre", "man", "men", "mujer", "woman", "women",
                         "chico", "chica"});
  add("subj", "child", {"nino", "nina", "child", "children", "kid", "boy", "girl"});
  add("subj", "astronaut", {"astronauta", "astronaut"});
  add("subj", "robot", {"robot", "androide", "android"});
  add("subj", "lighthouse", {"faro", "lighthouse"});
  add("subj", "heart", {"corazon", "heart"});
  add("subj", "butterfly", {"mariposa", "butterfly"});
  add("subj", "cactus", {"cactus"});
  add("subj", "mushroom", {"seta", "hongo", "mushroom"});
  add("subj", "horse", {"caballo", "horse"});
  add("subj", "cow", {"vaca", "cow"});
  add("style", "abstract", {"abstracto", "abstract", "fractal", "patron", "pattern"});
  return m;
}();

const std::map<std::string, int> NUMBERS = {
    {"un", 1},     {"una", 1},     {"uno", 1},    {"a", 1},      {"an", 1},       {"one", 1},  {"dos", 2},
    {"two", 2},    {"tres", 3},    {"three", 3},  {"cuatro", 4}, {"four", 4},     {"cinco", 5}, {"five", 5},
    {"varios", 3}, {"varias", 3},  {"several", 3}, {"muchos", 5}, {"muchas", 5},  {"many", 5}, {"some", 3},
    {"unos", 2},   {"unas", 2}};
const std::set<std::string> SKY_SUBJECTS = {"bird", "rocket", "ufo", "butterfly"};
const std::set<std::string> WATER_SUBJECTS = {"boat", "fish"};
const std::map<std::string, Rgb> DEFAULT_COLOR = {
    {"cat", {225, 135, 55}},       {"dog", {165, 115, 70}},        {"house", {232, 205, 165}},
    {"castle", {150, 150, 162}},   {"tree", {45, 140, 60}},        {"pine", {30, 110, 60}},
    {"flower", {230, 60, 90}},     {"car", {200, 40, 40}},         {"boat", {240, 240, 235}},
    {"fish", {250, 140, 30}},      {"person", {60, 110, 200}},     {"child", {240, 170, 40}},
    {"astronaut", {238, 238, 240}}, {"robot", {175, 182, 195}},    {"lighthouse", {240, 240, 235}},
    {"heart", {220, 40, 70}},      {"butterfly", {250, 150, 40}},  {"cactus", {60, 150, 70}},
    {"mushroom", {210, 40, 40}},   {"horse", {130, 80, 45}},       {"cow", {245, 245, 240}},
    {"bird", {40, 40, 50}},        {"rocket", {235, 235, 240}},    {"ufo", {170, 180, 195}}};
const std::map<std::string, double> SIZE = {
    {"house", 0.30}, {"castle", 0.36}, {"tree", 0.34},     {"pine", 0.36},  {"lighthouse", 0.46}, {"person", 0.24},
    {"child", 0.17}, {"astronaut", 0.24}, {"robot", 0.24}, {"cat", 0.13},   {"dog", 0.15},        {"horse", 0.24},
    {"cow", 0.2},    {"car", 0.13},    {"flower", 0.12},   {"cactus", 0.22}, {"mushroom", 0.1},   {"heart", 0.2},
    {"boat", 0.18},  {"fish", 0.09},   {"bird", 0.05},     {"rocket", 0.22}, {"ufo", 0.12},       {"butterfly", 0.07}};

const std::map<std::string, std::vector<Rgb>> SKIES = {
    {"day", {{70, 140, 225}, {170, 210, 245}}},
    {"sunset", {{55, 45, 115}, {230, 105, 85}, {255, 190, 110}}},
    {"sunrise", {{110, 140, 215}, {250, 175, 150}, {255, 215, 160}}},
    {"night", {{6, 8, 28}, {30, 36, 85}}},
    {"storm", {{55, 60, 72}, {105, 110, 125}}},
    {"space", {{2, 2, 10}, {14, 10, 34}}}};
const std::map<std::string, double> LIGHT = {{"day", 1.0},  {"sunrise", 0.85}, {"sunset", 0.75},
                                             {"night", 0.42}, {"storm", 0.62}, {"space", 0.8}};

template <class C, class T>
bool has(const C& c, const T& v) {
  return std::find(c.begin(), c.end(), v) != c.end();
}

const Hit* lookup(const std::string& tok) {
  auto find = [](const std::string& t) -> const Hit* {
    auto it = WORDS.find(t);
    return it == WORDS.end() ? nullptr : &it->second;
  };
  if (auto* h = find(tok)) return h;
  if (ends_with(tok, "s"))
    if (auto* h = find(tok.substr(0, tok.size() - 1))) return h;
  if (ends_with(tok, "es"))
    if (auto* h = find(tok.substr(0, tok.size() - 2))) return h;
  return nullptr;
}

std::optional<Rgb> color_of(const std::string& tok) {
  auto it = COLORS.find(tok);
  if (it != COLORS.end()) return it->second;
  if (ends_with(tok, "s")) {
    it = COLORS.find(tok.substr(0, tok.size() - 1));
    if (it != COLORS.end()) return it->second;
  }
  return std::nullopt;
}

std::vector<std::string> tokens(const std::string& prompt) {
  static const Rx TOK(R"([a-z0-9]+)");
  std::vector<std::string> out;
  for (auto& m : TOK.findall(normalize(prompt))) out.push_back(m);
  return out;
}

bool is_digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// ---------------------------------------------------------------------------------------- utilidades de color
Rgb mix(const Rgb& a, const Rgb& b, double t) {
  Rgb o;
  for (int k = 0; k < 3; ++k) o[k] = (int)(a[k] + (b[k] - a[k]) * t);
  return o;
}

Rgb shade(const Rgb& c, double f) {
  Rgb o;
  for (int k = 0; k < 3; ++k) o[k] = std::clamp((int)(c[k] * f), 0, 255);
  return o;
}

// ---------------------------------------------------------------------------------------- dibujo (como ImageDraw)
// las cajas aceptan las esquinas en cualquier orden (las figuras se dibujan simetricas)
struct Draw {
  Canvas& cv;
  explicit Draw(Canvas& c) : cv(c) {}

  void put(int x, int y, const Rgb& c) {
    uint8_t p[3] = {(uint8_t)std::clamp(c[0], 0, 255), (uint8_t)std::clamp(c[1], 0, 255), (uint8_t)std::clamp(c[2], 0, 255)};
    cv.plot(x, y, p);
  }
  static void box(double& x0, double& y0, double& x1, double& y1) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
  }
  void rect(double x0, double y0, double x1, double y1, const Rgb& c) {
    box(x0, y0, x1, y1);
    int ya = std::max(0, (int)std::lround(y0)), yb = std::min(cv.h - 1, (int)std::lround(y1));
    int xa = std::max(0, (int)std::lround(x0)), xb = std::min(cv.w - 1, (int)std::lround(x1));
    for (int y = ya; y <= yb; ++y)
      for (int x = xa; x <= xb; ++x) put(x, y, c);
  }
  // elipse (o su mitad de arriba: chord de 180 a 360 grados)
  void ellipse(double x0, double y0, double x1, double y1, const Rgb& c, bool top_half = false) {
    box(x0, y0, x1, y1);
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = std::max((x1 - x0) / 2, 0.5), ry = std::max((y1 - y0) / 2, 0.5);
    int ya = std::max(0, (int)std::floor(y0)), yb = std::min(cv.h - 1, (int)std::ceil(top_half ? cy : y1));
    int xa = std::max(0, (int)std::floor(x0)), xb = std::min(cv.w - 1, (int)std::ceil(x1));
    for (int y = ya; y <= yb; ++y) {
      double dy = (y + 0.5 - cy) / ry;
      if (top_half && y + 0.5 > cy + 0.5) continue;
      for (int x = xa; x <= xb; ++x) {
        double dx = (x + 0.5 - cx) / rx;
        if (dx * dx + dy * dy <= 1.0) put(x, y, c);
      }
    }
  }
  void chord(double x0, double y0, double x1, double y1, const Rgb& c) { ellipse(x0, y0, x1, y1, c, true); }
  void rrect(double x0, double y0, double x1, double y1, double r, const Rgb& c) {
    box(x0, y0, x1, y1);
    r = std::max(0.0, std::min(r, std::min(x1 - x0, y1 - y0) / 2));
    int ya = std::max(0, (int)std::lround(y0)), yb = std::min(cv.h - 1, (int)std::lround(y1));
    int xa = std::max(0, (int)std::lround(x0)), xb = std::min(cv.w - 1, (int)std::lround(x1));
    for (int y = ya; y <= yb; ++y)
      for (int x = xa; x <= xb; ++x) {
        double px = x + 0.5, py = y + 0.5;
        double qx = px < x0 + r ? x0 + r : px > x1 - r ? x1 - r : px;
        double qy = py < y0 + r ? y0 + r : py > y1 - r ? y1 - r : py;
        if ((px - qx) * (px - qx) + (py - qy) * (py - qy) <= r * r + 0.25) put(x, y, c);
      }
  }
  void polygon(const std::vector<Pt>& pts, const Rgb& c) {
    if (pts.size() < 3) return;
    double ymin = pts[0].second, ymax = pts[0].second;
    for (auto& p : pts) {
      ymin = std::min(ymin, p.second);
      ymax = std::max(ymax, p.second);
    }
    int ya = std::max(0, (int)std::floor(ymin)), yb = std::min(cv.h - 1, (int)std::ceil(ymax));
    std::vector<double> xs;
    for (int y = ya; y <= yb; ++y) {
      double sy = y + 0.5;
      xs.clear();
      for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        auto [xi, yi] = pts[i];
        auto [xj, yj] = pts[j];
        if ((yi > sy) != (yj > sy)) xs.push_back(xi + (sy - yi) * (xj - xi) / (yj - yi));
      }
      std::sort(xs.begin(), xs.end());
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {
        int xa = std::max(0, (int)std::lround(xs[k])), xb = std::min(cv.w - 1, (int)std::lround(xs[k + 1]) - 1);
        for (int x = xa; x <= xb; ++x) put(x, y, c);
      }
    }
  }
  void line(const std::vector<Pt>& pts, const Rgb& c, int width = 1) {
    uint8_t p[3] = {(uint8_t)std::clamp(c[0], 0, 255), (uint8_t)std::clamp(c[1], 0, 255), (uint8_t)std::clamp(c[2], 0, 255)};
    cv.line(pts, p, width);
  }
  void ellipse_outline(double x0, double y0, double x1, double y1, const Rgb& c, int width) {
    box(x0, y0, x1, y1);
    uint8_t p[3] = {(uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]};
    cv.ellipse_outline(x0, y0, x1, y1, p, width);
  }
};

class Painter {
 public:
  Painter(const Scene& sc, uint64_t seed, int size)
      : sc_(sc), W(size * 2), rng_(seed), img_(W, W, 0), glow_(W, W, 0), d(img_), g(glow_) {
    bool flowers = std::any_of(sc.subjects.begin(), sc.subjects.end(), [](auto& s) { return s.kind == "flower"; });
    place_ = !sc.places.empty() ? sc.places[0] : flowers ? "garden" : "field";
    mode_ = place_ == "space" ? "space" : has(sc.weather, std::string("storm")) ? "storm" : sc.time;
    light_ = LIGHT.at(mode_);
    H = (int)(W * (water_place() ? 0.55 : 0.62));
    if (place_ == "space") H = W;
    water_bottom_ = H;
  }

  Canvas paint();

 private:
  const Scene& sc_;
  int W, H = 0;
  std::mt19937_64 rng_;
  Canvas img_, glow_;
  Draw d, g;
  std::string place_, mode_;
  double light_ = 1.0;
  int water_bottom_;

  double uni(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng_); }
  double rnd() { return uni(0, 1); }
  bool water_place() const { return place_ == "sea" || place_ == "beach" || place_ == "lake"; }
  bool snow() const { return has(sc_.weather, std::string("snow")); }
  bool night_like() const { return mode_ == "night" || mode_ == "sunset" || mode_ == "storm"; }

  // color de un objeto bajo la luz de la escena (de noche, oscuro y azulado)
  Rgb lit(const Rgb& c) const {
    Rgb o = shade(c, light_);
    return mode_ == "night" ? mix(o, {20, 30, 70}, 0.25) : o;
  }

  // ------------------------------------------------------------------------------------------ fondo
  void sky() {
    const auto& stops = SKIES.at(mode_);
    std::vector<float> arr((size_t)W * W * 3);
    double cx = 0, cy = 0;
    Rgb tint = sc_.colors.empty() ? Rgb{110, 60, 200} : sc_.colors[0];
    if (mode_ == "space") {
      cx = uni(0.3, 0.7);
      cy = uni(0.3, 0.7);
    }
    for (int y = 0; y < W; ++y) {
      double t = W > 1 ? (double)y / (W - 1) : 0;
      double col[3];
      for (int k = 0; k < 3; ++k) {
        if (stops.size() == 2)
          col[k] = (1 - t) * stops[0][k] + t * stops[1][k];
        else
          col[k] = t < 0.5 ? (1 - t * 2) * stops[0][k] + t * 2 * stops[1][k]
                           : (2 - t * 2) * stops[1][k] + (t * 2 - 1) * stops[2][k];
      }
      for (int x = 0; x < W; ++x) {
        double add = 0;
        if (mode_ == "space") {   // nebulosa con el color del prompt
          double xx = (double)x / W, yy = (double)y / W;
          add = std::exp(-(std::pow((xx - cx) * 1.6, 2) + std::pow((yy - cy) * 2.6, 2)) * 3) *
                (0.6 + 0.4 * std::sin(xx * 9 + yy * 5));
        }
        uint8_t* p = &img_.px[((size_t)y * W + x) * 3];
        for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::clamp((int)(col[k] + add * tint[k] * 0.55), 0, 255);
      }
    }
  }

  void stars(int n) {
    for (int i = 0; i < n; ++i) {
      double x = uni(0, W), y = uni(0, H * 0.9), r = uni(1, 3.2);
      d.ellipse(x - r, y - r, x + r, y + r, {255, 255, 235});
    }
  }

  void sun(double x, double y, double r) {
    Rgb col = mode_ == "sunset" ? Rgb{255, 150, 70} : mode_ == "sunrise" ? Rgb{255, 200, 120} : Rgb{255, 230, 120};
    g.ellipse(x - r * 2.2, y - r * 2.2, x + r * 2.2, y + r * 2.2, shade(col, 0.55));
    d.ellipse(x - r, y - r, x + r, y + r, col);
  }

  void moon(double x, double y, double r) {
    g.ellipse(x - r * 2, y - r * 2, x + r * 2, y + r * 2, {70, 80, 110});
    d.ellipse(x - r, y - r, x + r, y + r, {245, 242, 220});
    const double cr[3][3] = {{-0.3, -0.2, 0.18}, {0.25, 0.15, 0.12}, {-0.05, 0.35, 0.1}};
    for (auto& k : cr) {
      double cx = x + k[0] * r, cy = y + k[1] * r, rr = k[2];
      d.ellipse(cx - rr * r, cy - rr * r, cx + rr * r, cy + rr * r, {215, 212, 195});
    }
  }

  void cloud(double x, double y, double s) {
    Rgb col = mode_ != "storm" ? lit({245, 245, 250}) : Rgb{85, 88, 100};
    const double parts[5][3] = {{0, 0, 0.5}, {0.45, 0.1, 0.38}, {-0.45, 0.12, 0.36}, {0.2, -0.25, 0.4}, {-0.2, -0.2, 0.32}};
    for (auto& p : parts) {
      double cx = x + p[0] * s, cy = y + p[1] * s, rr = p[2] * s;
      d.ellipse(cx - rr, cy - rr, cx + rr, cy + rr, col);
    }
  }

  void planet(double x, double y, double r) {
    Rgb base = sc_.colors.empty() ? Rgb{220, 150, 90} : sc_.colors.back();
    g.ellipse(x - r * 1.5, y - r * 1.5, x + r * 1.5, y + r * 1.5, shade(base, 0.35));
    d.ellipse(x - r, y - r, x + r, y + r, base);
    for (int k = 0; k < 3; ++k) {
      double yy = y - r * 0.5 + k * r * 0.45;
      double w = std::sqrt(std::max(r * r - (yy - y) * (yy - y), 0.0));
      d.line({{x - w * 0.95, yy}, {x + w * 0.95, yy}}, shade(base, 0.8), (int)(r * 0.12));
    }
    d.ellipse_outline(x - r * 1.8, y - r * 0.35, x + r * 1.8, y + r * 0.35, {235, 225, 200}, std::max(2, (int)(r * 0.08)));
  }

  void hills(double base, const Rgb& col, double amp) {
    double ph = uni(0, 6.3);
    std::vector<Pt> pts;
    for (int x = 0; x <= W; x += 12) pts.push_back({x, base - (std::sin((double)x / W * 5 + ph) * 0.5 + 0.5) * amp * W});
    pts.push_back({W, W});
    pts.push_back({0, W});
    d.polygon(pts, col);
  }

  void ground() {
    if (place_ == "space") return;
    if (water_place()) {
      Rgb water = lit(place_ != "lake" ? Rgb{30, 110, 175} : Rgb{50, 120, 150});
      int bottom = place_ == "sea" ? H : (int)(H + (W - H) * (place_ == "beach" ? 0.45 : 0.55));
      if (place_ == "lake") hills(H - (int)(W * 0.06), lit({60, 130, 60}), 0.03);   // orilla con hierba detras del agua
      d.rect(0, H, W, W, water);
      for (int k = 0; k < 40; ++k) {   // olas
        double y = H + std::pow(k / 40.0, 1.5) * (W - H);
        double x = uni(-50, W);
        double w = 20 + (y - H) * 0.4;
        d.line({{x, y}, {x + w, y}}, shade(water, 1.35), std::max(2, (int)((y - H) * 0.02) + 2));
      }
      if (place_ == "beach") {
        Rgb sand = lit(!snow() ? Rgb{235, 210, 150} : Rgb{240, 240, 245});
        std::vector<Pt> pts = {{0, (double)bottom}};
        for (int x = 0; x <= W; x += 16) pts.push_back({x, bottom + std::sin((double)x / W * 6 + 1) * W * 0.02});
        std::vector<Pt> poly = pts;
        poly.push_back({W, W});
        poly.push_back({0, W});
        d.polygon(poly, sand);
        d.line(pts, {250, 250, 245}, 6);
      }
      water_bottom_ = bottom;
      return;
    }
    if (place_ == "mountain") {
      const std::pair<double, int> layers[2] = {{0.7, 4}, {1.0, 3}};
      for (int layer = 0; layer < 2; ++layer) {
        auto [shd, peaks] = layers[layer];
        std::vector<Pt> pts = {{0, (double)H}};
        for (int k = 0; k < peaks * 2 + 1; ++k) {
          double x = k * (double)W / (peaks * 2);
          bool top = k % 2 == 1;
          double y = H - (top ? uni(0.28, 0.42) : uni(0.08, 0.16)) * W * (1.15 - layer * 0.25);
          pts.push_back({x, y});
        }
        pts.push_back({W, H});
        d.polygon(pts, lit(shade({110, 112, 130}, shd)));
        for (size_t k = 1; k + 1 < pts.size(); k += 2) {   // nieve en las cumbres
          auto [x0, y0] = pts[k - 1];
          auto [x1, y1] = pts[k];
          auto [x2, y2] = pts[k + 1];
          double f = 0.28;
          d.polygon({{x1, y1}, {x1 + (x2 - x1) * f, y1 + (y2 - y1) * f}, {x1 + (x0 - x1) * f, y1 + (y0 - y1) * f}},
                    lit({245, 245, 250}));
        }
      }
      hills(H, lit(snow() ? Rgb{240, 240, 245} : Rgb{70, 135, 65}), 0.02);
      return;
    }
    if (place_ == "desert") {
      hills(H - (int)(W * 0.04), lit({215, 170, 100}), 0.05);
      hills(H + (int)(W * 0.08), lit({235, 195, 120}), 0.04);
      return;
    }
    if (place_ == "city") {
      double x = -10;
      while (x < W) {
        double w = uni(0.07, 0.14) * W, h = uni(0.18, 0.5) * W;
        Rgb body = lit(shade({80, 90, 115}, uni(0.8, 1.2)));
        d.rect(x, H - h, x + w, H, body);
        bool lit_win = night_like();
        for (double wy = H - h + 12; wy < H - 10; wy += 22)
          for (double wx = x + 8; wx < x + w - 10; wx += 18) {
            bool on = lit_win && rnd() < 0.55;
            d.rect(wx, wy, wx + 9, wy + 12, on ? Rgb{255, 215, 120} : lit({150, 175, 205}));
            if (on) g.rect(wx - 4, wy - 4, wx + 13, wy + 16, {90, 70, 30});
          }
        x += w + uni(2, 10);
      }
      d.rect(0, H, W, W, lit(!snow() ? Rgb{60, 62, 70} : Rgb{225, 228, 235}));
      for (int k = 0; k < 6; ++k) {   // marcas de la calle
        double xx = k * W / 6.0 + 20;
        d.rect(xx, H + (W - H) * 0.55, xx + W / 14.0, H + (W - H) * 0.58, lit({230, 220, 150}));
      }
      return;
    }
    Rgb green = snow() ? Rgb{240, 240, 245} : place_ != "garden" ? Rgb{75, 150, 60} : Rgb{70, 160, 70};
    hills(H - (int)(W * 0.05), lit(shade(green, 0.8)), 0.04);
    hills(H, lit(green), 0.025);
    if (place_ == "garden") {
      std::vector<Rgb> cols = sc_.colors;
      if (cols.empty()) cols = {{230, 60, 90}, {250, 200, 40}, {150, 80, 210}, {245, 245, 240}};
      for (int k = 0; k < 18; ++k) {
        double fx = uni(0, W), fy = H + (W - H) * uni(0.15, 0.95), fh = W * uni(0.05, 0.08);
        flower(fx, fy, fh, cols[k % cols.size()]);
      }
    }
    if (place_ == "forest")
      for (int k = 0; k < 14; ++k) {
        double x = (k + uni(-0.3, 0.3)) * W / 13;
        double y = H + uni(-0.02, 0.03) * W, h = W * uni(0.22, 0.32);
        pine(x, y, h, shade({30, 110, 60}, uni(0.7, 1.0)));
      }
  }

  // ------------------------------------------------------------------------------------------ sujetos
  void pine(double x, double y, double h, std::optional<Rgb> c0 = std::nullopt) {
    Rgb col = lit(c0.value_or(Rgb{30, 110, 60}));
    d.rect(x - h * 0.04, y - h * 0.2, x + h * 0.04, y, lit({90, 60, 35}));
    for (int k = 0; k < 3; ++k) {
      double top = y - h * (1 - k * 0.22), w = h * (0.22 + k * 0.08);
      d.polygon({{x, top}, {x - w, top + h * 0.45}, {x + w, top + h * 0.45}}, shade(col, 1 - k * 0.08));
    }
    if (snow()) d.polygon({{x, y - h}, {x - h * 0.1, y - h * 0.82}, {x + h * 0.1, y - h * 0.82}}, {250, 250, 255});
  }

  void tree(double x, double y, double h, const Rgb& col) {
    d.rect(x - h * 0.06, y - h * 0.5, x + h * 0.06, y, lit({100, 65, 35}));
    const double parts[4][3] = {{0, -0.72, 0.3}, {-0.2, -0.55, 0.22}, {0.2, -0.55, 0.22}, {0, -0.92, 0.2}};
    for (auto& p : parts) {
      double cx = x + p[0] * h, cy = y + p[1] * h, rr = p[2] * h;
      d.ellipse(cx - rr, cy - rr, cx + rr, cy + rr, lit(col));
    }
  }

  void house(double x, double y, double h, const Rgb& col) {
    double w = h * 1.1;
    d.rect(x - w / 2, y - h * 0.6, x + w / 2, y, lit(col));
    d.polygon({{x - w * 0.62, y - h * 0.58}, {x, y - h}, {x + w * 0.62, y - h * 0.58}}, lit({165, 55, 45}));
    d.rect(x - w * 0.1, y - h * 0.3, x + w * 0.1, y, lit({110, 70, 40}));
    bool warm = night_like();
    Rgb win = warm ? Rgb{255, 215, 120} : lit({160, 200, 235});
    for (double wx : {x - w * 0.32, x + w * 0.2}) {
      d.rect(wx, y - h * 0.45, wx + w * 0.13, y - h * 0.32, win);
      if (warm) g.rect(wx - 8, y - h * 0.45 - 8, wx + w * 0.13 + 8, y - h * 0.32 + 8, {110, 80, 30});
    }
  }

  void castle(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    double w = h * 1.2;
    d.rect(x - w / 2, y - h * 0.55, x + w / 2, y, c);
    for (int t = 0; t < 3; ++t) {
      double tx = t == 0 ? x - w / 2 : t == 1 ? x + w / 2 - w * 0.22 : x - w * 0.11;
      double th = t == 2 ? h : h * 0.8;
      d.rect(tx, y - th, tx + w * 0.22, y, shade(c, 0.92));
      for (int k = 0; k < 3; ++k) {
        double bx = tx + k * w * 0.08;
        d.rect(bx, y - th - h * 0.06, bx + w * 0.05, y - th, shade(c, 0.92));
      }
      d.polygon({{tx - 4, y - th - h * 0.06}, {tx + w * 0.11, y - th - h * 0.3}, {tx + w * 0.22 + 4, y - th - h * 0.06}},
                lit({60, 70, 150}));
    }
    d.chord(x - w * 0.1, y - h * 0.32, x + w * 0.1, y + h * 0.05, lit({60, 40, 30}));
    d.rect(x - w * 0.1, y - h * 0.14, x + w * 0.1, y, lit({60, 40, 30}));
  }

  void lighthouse(double x, double y, double h, const Rgb& col) {
    double w0 = h * 0.16, w1 = h * 0.1;
    d.polygon({{x - w0, y}, {x - w1, y - h * 0.8}, {x + w1, y - h * 0.8}, {x + w0, y}}, lit(col));
    for (double k : {0.2, 0.5}) {
      double yy = y - h * k, ww = w0 - (w0 - w1) * k;
      d.polygon({{x - ww, yy}, {x - ww * 0.95, yy - h * 0.12}, {x + ww * 0.95, yy - h * 0.12}, {x + ww, yy}},
                lit({200, 40, 40}));
    }
    d.rect(x - w1, y - h * 0.92, x + w1, y - h * 0.8, {255, 225, 120});
    d.polygon({{x - w1 * 1.2, y - h * 0.92}, {x, y - h}, {x + w1 * 1.2, y - h * 0.92}}, lit({60, 60, 70}));
    if (night_like()) {
      g.polygon({{x, y - h * 0.86}, {x + W, y - h * 1.05}, {x + W, y - h * 0.65}}, {90, 80, 40});
      g.ellipse(x - h * 0.15, y - h, x + h * 0.15, y - h * 0.72, {160, 140, 60});
    }
  }

  void cat(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    d.ellipse(x - h * 0.45, y - h * 0.55, x + h * 0.35, y, c);                                         // cuerpo
    d.line({{x + h * 0.3, y - h * 0.15}, {x + h * 0.6, y - h * 0.35}, {x + h * 0.55, y - h * 0.7}}, c,
           std::max(3, (int)(h * 0.09)));                                                               // cola
    double hx = x - h * 0.35, hy = y - h * 0.7, r = h * 0.25;
    d.ellipse(hx - r, hy - r, hx + r, hy + r, c);                                                       // cabeza
    for (int s : {-1, 1}) {
      d.polygon({{hx + s * r * 0.9, hy - r * 0.2}, {hx + s * r * 0.75, hy - r * 1.45}, {hx + s * r * 0.15, hy - r * 0.75}},
                c);                                                                                     // orejas
      double ex = hx + s * r * 0.4;
      d.ellipse(ex - r * 0.16, hy - r * 0.2, ex + r * 0.16, hy + r * 0.12, {120, 220, 90});
      d.ellipse(ex - r * 0.05, hy - r * 0.16, ex + r * 0.05, hy + r * 0.08, {10, 10, 10});
    }
  }

  void dog(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    d.rrect(x - h * 0.45, y - h * 0.62, x + h * 0.35, y - h * 0.28, h * 0.15, c);
    for (double lx : {-0.38, -0.2, 0.12, 0.28}) d.rect(x + lx * h, y - h * 0.35, x + lx * h + h * 0.09, y, c);
    d.line({{x + h * 0.33, y - h * 0.55}, {x + h * 0.55, y - h * 0.8}}, c, std::max(3, (int)(h * 0.07)));
    double hx = x - h * 0.5, hy = y - h * 0.8, r = h * 0.22;
    d.ellipse(hx - r, hy - r, hx + r, hy + r, c);
    d.ellipse(hx - r * 1.5, hy - r * 0.1, hx - r * 0.4, hy + r * 0.6, c);                        // hocico
    d.ellipse(hx + r * 0.2, hy - r * 0.6, hx + r * 0.75, hy + r * 0.9, shade(c, 0.7));          // oreja
    d.ellipse(hx - r * 0.35, hy - r * 0.35, hx - r * 0.1, hy - r * 0.1, {15, 15, 15});
    d.ellipse(hx - r * 1.6, hy - r * 0.05, hx - r * 1.25, hy + r * 0.25, {15, 15, 15});
  }

  void horse(double x, double y, double h, const Rgb& col, bool cow) {
    Rgb c = lit(col);
    d.ellipse(x - h * 0.5, y - h * 0.75, x + h * 0.45, y - h * 0.35, c);
    for (double lx : {-0.42, -0.28, 0.2, 0.33}) d.rect(x + lx * h, y - h * 0.45, x + lx * h + h * 0.08, y, c);
    if (cow) {
      for (auto [sx, sy] : {Pt{-0.2, -0.6}, Pt{0.15, -0.5}})
        d.ellipse(x + sx * h, y + sy * h, x + sx * h + h * 0.18, y + sy * h + h * 0.12, {30, 30, 30});
      d.ellipse(x - h * 0.75, y - h * 0.8, x - h * 0.45, y - h * 0.5, c);
      d.ellipse(x - h * 0.8, y - h * 0.65, x - h * 0.55, y - h * 0.48, lit({240, 170, 170}));
      return;
    }
    d.polygon({{x - h * 0.4, y - h * 0.68}, {x - h * 0.55, y - h * 1.05}, {x - h * 0.42, y - h * 1.1}, {x - h * 0.25, y - h * 0.7}},
              c);   // cuello
    d.polygon({{x - h * 0.55, y - h * 1.05}, {x - h * 0.82, y - h * 0.9}, {x - h * 0.78, y - h * 0.82}, {x - h * 0.48, y - h * 0.9}},
              c);   // cabeza
    d.line({{x - h * 0.43, y - h * 1.08}, {x - h * 0.25, y - h * 0.75}}, shade(c, 0.5), std::max(3, (int)(h * 0.05)));   // crin
    d.line({{x + h * 0.45, y - h * 0.65}, {x + h * 0.58, y - h * 0.3}}, shade(c, 0.5), std::max(3, (int)(h * 0.05)));
  }

  void person(double x, double y, double h, const Rgb& col, bool helmet = false) {
    Rgb skin = lit({230, 185, 150}), c = lit(col);
    Rgb legs = helmet ? c : lit({50, 55, 80});
    d.rect(x - h * 0.12, y - h * 0.38, x - h * 0.03, y, legs);
    d.rect(x + h * 0.03, y - h * 0.38, x + h * 0.12, y, legs);
    d.rrect(x - h * 0.17, y - h * 0.75, x + h * 0.17, y - h * 0.35, h * 0.06, c);
    d.line({{x - h * 0.16, y - h * 0.7}, {x - h * 0.28, y - h * 0.42}}, c, std::max(3, (int)(h * 0.07)));
    d.line({{x + h * 0.16, y - h * 0.7}, {x + h * 0.28, y - h * 0.42}}, c, std::max(3, (int)(h * 0.07)));
    double r = h * 0.12;
    if (helmet) {
      d.ellipse(x - r * 1.4, y - h * 0.9 - r * 1.4, x + r * 1.4, y - h * 0.9 + r * 1.4, c);
      d.ellipse(x - r, y - h * 0.9 - r * 0.7, x + r, y - h * 0.9 + r * 0.7, {40, 60, 110});
      d.rect(x + h * 0.15, y - h * 0.72, x + h * 0.25, y - h * 0.45, shade(c, 0.85));
      return;
    }
    d.ellipse(x - r, y - h * 0.88 - r, x + r, y - h * 0.88 + r, skin);
    d.chord(x - r * 1.05, y - h * 0.88 - r * 1.1, x + r * 1.05, y - h * 0.88 + r * 0.6, lit({70, 45, 30}));
  }

  void robot(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    d.rect(x - h * 0.13, y - h * 0.3, x - h * 0.04, y, shade(c, 0.8));
    d.rect(x + h * 0.04, y - h * 0.3, x + h * 0.13, y, shade(c, 0.8));
    d.rrect(x - h * 0.22, y - h * 0.68, x + h * 0.22, y - h * 0.28, h * 0.04, c);
    d.rect(x - h * 0.08, y - h * 0.58, x + h * 0.08, y - h * 0.45, {80, 200, 240});
    for (int s : {-1, 1}) d.rect(x + s * h * 0.22, y - h * 0.65, x + s * h * 0.3, y - h * 0.38, shade(c, 0.85));
    d.rrect(x - h * 0.15, y - h * 0.92, x + h * 0.15, y - h * 0.7, h * 0.03, c);
    for (int s : {-1, 1}) {
      double ex = x + s * h * 0.07;
      d.ellipse(ex - h * 0.035, y - h * 0.85, ex + h * 0.035, y - h * 0.78, {90, 230, 255});
      g.ellipse(ex - h * 0.07, y - h * 0.88, ex + h * 0.07, y - h * 0.75, {30, 100, 130});
    }
    d.line({{x, y - h * 0.92}, {x, y - h}}, c, std::max(2, (int)(h * 0.02)));
    d.ellipse(x - h * 0.03, y - h * 1.03, x + h * 0.03, y - h * 0.97, {240, 60, 60});
  }

  void car(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    double w = h * 2.4;
    d.rrect(x - w / 2, y - h * 0.75, x + w / 2, y - h * 0.2, h * 0.15, c);
    d.polygon({{x - w * 0.28, y - h * 0.72}, {x - w * 0.16, y - h * 1.15}, {x + w * 0.2, y - h * 1.15}, {x + w * 0.32, y - h * 0.72}},
              c);
    d.polygon({{x - w * 0.22, y - h * 0.75}, {x - w * 0.13, y - h * 1.07}, {x + w * 0.17, y - h * 1.07}, {x + w * 0.26, y - h * 0.75}},
              lit({170, 210, 235}));
    for (double wx : {x - w * 0.3, x + w * 0.3}) {
      d.ellipse(wx - h * 0.25, y - h * 0.45, wx + h * 0.25, y + h * 0.05, {25, 25, 28});
      d.ellipse(wx - h * 0.1, y - h * 0.3, wx + h * 0.1, y - h * 0.1, {170, 170, 175});
    }
    if (mode_ == "night" || mode_ == "storm")
      g.polygon({{x - w / 2, y - h * 0.55}, {x - w * 1.3, y - h * 0.9}, {x - w * 1.3, y}}, {110, 100, 50});
  }

  void flower(double x, double y, double h, const Rgb& col) {
    d.line({{x, y}, {x, y - h * 0.7}}, lit({50, 140, 50}), std::max(2, (int)(h * 0.06)));
    d.ellipse(x, y - h * 0.4, x + h * 0.25, y - h * 0.3, lit({50, 140, 50}));
    double cx = x, cy = y - h * 0.78, r = h * 0.13;
    for (int k = 0; k < 6; ++k) {
      double a = k * M_PI / 3, px = cx + std::cos(a) * r * 1.3, py = cy + std::sin(a) * r * 1.3;
      d.ellipse(px - r, py - r, px + r, py + r, lit(col));
    }
    d.ellipse(cx - r * 0.8, cy - r * 0.8, cx + r * 0.8, cy + r * 0.8, lit({250, 210, 60}));
  }

  void cactus(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    double w = h * 0.1;
    d.rrect(x - w, y - h, x + w, y, w, c);
    d.rrect(x - w * 3.2, y - h * 0.7, x - w * 1.6, y - h * 0.35, w * 0.8, c);
    d.rect(x - w * 3.2, y - h * 0.45, x - w * 0.5, y - h * 0.35, c);
    d.rrect(x + w * 1.6, y - h * 0.8, x + w * 3.2, y - h * 0.45, w * 0.8, c);
    d.rect(x + w * 0.5, y - h * 0.55, x + w * 3.2, y - h * 0.45, c);
  }

  void mushroom(double x, double y, double h, const Rgb& col) {
    d.rect(x - h * 0.12, y - h * 0.55, x + h * 0.12, y, lit({240, 230, 210}));
    d.chord(x - h * 0.45, y - h * 1.0, x + h * 0.45, y - h * 0.2, lit(col));
    for (auto [dx, dy] : {Pt{-0.2, -0.75}, Pt{0.12, -0.82}, Pt{0.25, -0.67}})
      d.ellipse(x + dx * h - h * 0.05, y + dy * h - h * 0.05, x + dx * h + h * 0.05, y + dy * h + h * 0.05, {250, 250, 245});
  }

  void heart(double x, double y, double h, const Rgb& col) {
    Rgb c = mode_ != "night" ? lit(col) : col;
    double r = h * 0.27;
    g.ellipse(x - h * 0.6, y - h * 1.1, x + h * 0.6, y + h * 0.1, shade(col, 0.4));
    d.ellipse(x - 2 * r, y - h * 0.9, x, y - h * 0.9 + 2 * r, c);
    d.ellipse(x, y - h * 0.9, x + 2 * r, y - h * 0.9 + 2 * r, c);
    d.polygon({{x - 2 * r + 2, y - h * 0.9 + r * 1.3}, {x + 2 * r - 2, y - h * 0.9 + r * 1.3}, {x, y}}, c);
  }

  void boat(double x, double y, double h, const Rgb& col) {
    d.polygon({{x - h * 0.6, y - h * 0.22}, {x + h * 0.6, y - h * 0.22}, {x + h * 0.42, y}, {x - h * 0.42, y}}, lit({120, 70, 40}));
    d.line({{x, y - h * 0.22}, {x, y - h * 1.1}}, lit({90, 60, 35}), std::max(2, (int)(h * 0.04)));
    d.polygon({{x + h * 0.03, y - h * 1.05}, {x + h * 0.03, y - h * 0.3}, {x + h * 0.5, y - h * 0.3}}, lit(col));
    d.polygon({{x - h * 0.03, y - h * 0.9}, {x - h * 0.03, y - h * 0.32}, {x - h * 0.38, y - h * 0.32}}, shade(lit(col), 0.9));
  }

  void fish(double x, double y, double h, const Rgb& col) {
    Rgb c = lit(col);
    d.ellipse(x - h * 0.6, y - h * 0.3, x + h * 0.4, y + h * 0.3, c);
    d.polygon({{x + h * 0.3, y}, {x + h * 0.75, y - h * 0.35}, {x + h * 0.75, y + h * 0.35}}, c);
    d.ellipse(x - h * 0.42, y - h * 0.1, x - h * 0.28, y + h * 0.04, {15, 15, 15});
  }

  void bird(double x, double y, double h, const Rgb& col) {
    Rgb c = mode_ == "day" ? col : shade(col, 0.5);
    int wd = std::max(3, (int)(h * 0.15));
    d.line({{x - h, y - h * 0.4}, {x - h * 0.4, y - h * 0.5}, {x, y}}, c, wd);
    d.line({{x, y}, {x + h * 0.4, y - h * 0.5}, {x + h, y - h * 0.4}}, c, wd);
  }

  void butterfly(double x, double y, double h, const Rgb& col) {
    Rgb c = mode_ != "night" ? lit(col) : col;
    for (int s : {-1, 1}) {
      d.ellipse(std::min(x, x + s * h * 0.6), y - h * 0.55, std::max(x, x + s * h * 0.6), y + h * 0.05, c);
      d.ellipse(std::min(x, x + s * h * 0.45), y, std::max(x, x + s * h * 0.45), y + h * 0.4, shade(c, 0.8));
    }
    d.line({{x, y - h * 0.5}, {x, y + h * 0.4}}, {30, 30, 30}, std::max(2, (int)(h * 0.08)));
  }

  void rocket(double x, double y, double h, const Rgb& col) {
    Rgb c = mode_ != "space" ? lit(col) : col;
    double w = h * 0.16;
    g.polygon({{x - w, y}, {x + w, y}, {x, y + h * 0.7}}, {160, 90, 20});
    d.polygon({{x - w * 0.7, y}, {x + w * 0.7, y}, {x, y + h * 0.45}}, {255, 170, 40});
    d.rrect(x - w, y - h * 0.75, x + w, y, w * 0.5, c);
    d.polygon({{x - w, y - h * 0.72}, {x, y - h}, {x + w, y - h * 0.72}}, {210, 40, 45});
    for (int s : {-1, 1}) d.polygon({{x + s * w, y - h * 0.25}, {x + s * w * 2, y + h * 0.02}, {x + s * w, y}}, {210, 40, 45});
    d.ellipse(x - w * 0.5, y - h * 0.58, x + w * 0.5, y - h * 0.58 + w, {80, 170, 230});
  }

  void ufo(double x, double y, double h, const Rgb& col) {
    g.polygon({{x - h * 0.3, y}, {x + h * 0.3, y}, {x + h * 0.8, y + h * 3}, {x - h * 0.8, y + h * 3}}, {60, 110, 70});
    d.ellipse(x - h * 0.35, y - h * 0.55, x + h * 0.35, y + h * 0.05, {150, 220, 240});
    d.ellipse(x - h, y - h * 0.25, x + h, y + h * 0.25, col);
    for (int k = 0; k < 5; ++k) {
      double lx = x - h * 0.7 + k * h * 0.35;
      d.ellipse(lx - h * 0.06, y - h * 0.05, lx + h * 0.06, y + h * 0.07, {255, 230, 90});
    }
  }

  void draw_subject(const std::string& kind, double x, double y, double h, const Rgb& col) {
    if (kind == "cat") cat(x, y, h, col);
    else if (kind == "dog") dog(x, y, h, col);
    else if (kind == "house") house(x, y, h, col);
    else if (kind == "castle") castle(x, y, h, col);
    else if (kind == "tree") tree(x, y, h, col);
    else if (kind == "pine") pine(x, y, h, col);
    else if (kind == "flower") flower(x, y, h, col);
    else if (kind == "car") car(x, y, h, col);
    else if (kind == "boat") boat(x, y, h, col);
    else if (kind == "fish") fish(x, y, h, col);
    else if (kind == "person" || kind == "child") person(x, y, h, col);
    else if (kind == "astronaut") person(x, y, h, col, true);
    else if (kind == "robot") robot(x, y, h, col);
    else if (kind == "lighthouse") lighthouse(x, y, h, col);
    else if (kind == "heart") heart(x, y, h, col);
    else if (kind == "cactus") cactus(x, y, h, col);
    else if (kind == "mushroom") mushroom(x, y, h, col);
    else if (kind == "horse" || kind == "cow") horse(x, y, h, col, kind == "cow");
    else if (kind == "bird") bird(x, y, h, col);
    else if (kind == "rocket") rocket(x, y, h, col);
    else if (kind == "ufo") ufo(x, y, h, col);
    else if (kind == "butterfly") butterfly(x, y, h, col);
  }

  // sombra suave bajo el sujeto (una elipse desenfocada que oscurece el suelo)
  void shadow(double x, double y, double h, const std::string& kind) {
    if (mode_ == "night" || mode_ == "space" || kind == "fish" || kind == "boat") return;
    double w = h * (kind != "car" && kind != "castle" && kind != "house" ? 0.7 : 1.3);
    int pad = 12;
    int x0 = (int)std::floor(x - w * 0.5) - pad, y0 = (int)std::floor(y - h * 0.04) - pad;
    int bw = (int)std::ceil(w) + 2 * pad + 2, bh = (int)std::ceil(h * 0.09) + 2 * pad + 2;
    if (bw <= 0 || bh <= 0) return;
    Canvas m(bw, bh, 0);
    Draw(m).ellipse(x - w * 0.5 - x0, y - h * 0.04 - y0, x + w * 0.5 - x0, y + h * 0.05 - y0, {90, 90, 90});
    m.gaussian_blur(4);
    for (int yy = 0; yy < bh; ++yy)
      for (int xx = 0; xx < bw; ++xx) {
        int X = x0 + xx, Y = y0 + yy;
        if (X < 0 || Y < 0 || X >= W || Y >= W) continue;
        double a = m.px[((size_t)yy * bw + xx) * 3] / 255.0;
        uint8_t* p = &img_.px[((size_t)Y * W + X) * 3];
        for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::lround(p[k] * (1 - a));
      }
  }

  void weather() {
    const auto& wx = sc_.weather;
    bool rain = has(wx, std::string("rain")), storm = has(wx, std::string("storm"));
    if (rain || storm)
      for (int i = 0; i < 500; ++i) {
        double x = uni(-40, W), y = uni(0, W);
        d.line({{x, y}, {x + 8, y + 34}}, {175, 190, 215}, 2);
      }
    if (storm) {
      double x = uni(0.2, 0.8) * W, y = W * 0.12;
      std::vector<Pt> pts = {{x, y}};
      for (int k = 0; k < 6; ++k) {
        x += uni(-40, 40);
        y += W * 0.06;
        pts.push_back({x, y});
      }
      g.line(pts, {160, 160, 220}, 14);
      d.line(pts, {250, 250, 255}, 5);
    }
    if (has(wx, std::string("snow")))
      for (int i = 0; i < 350; ++i) {
        double x = uni(0, W), y = uni(0, W), r = uni(2, 6);
        d.ellipse(x - r, y - r, x + r, y + r, {250, 250, 255});
      }
  }
};

Canvas Painter::paint() {
  sky();
  bool has_moon = has(sc_.sky, std::string("moon")), has_sun = has(sc_.sky, std::string("sun"));
  bool has_cloud = has(sc_.sky, std::string("cloud")), rain = has(sc_.weather, std::string("rain"));
  if (mode_ == "night" || mode_ == "space" || has(sc_.sky, std::string("star"))) stars(mode_ == "space" ? 260 : 120);
  double cx = W * uni(0.62, 0.82);
  double top = std::min((double)H, W * 0.62);
  if (has_moon || (mode_ == "night" && !has_sun))
    moon(cx, top * uni(0.22, 0.32), W * 0.07);
  else if (mode_ == "sunset" || mode_ == "sunrise")
    sun(cx, H - W * 0.06, W * 0.09);
  else if (has_sun || (mode_ == "day" && !has_cloud))
    sun(cx, top * 0.25, W * 0.065);
  if (has(sc_.sky, std::string("planet")) || mode_ == "space") {
    double px = W * uni(0.25, 0.4), py = W * uni(0.3, 0.45);
    planet(px, py, W * 0.13);
  }
  if (has_cloud || mode_ == "storm" || rain) {
    int n = mode_ == "storm" || rain ? 5 : 3;
    for (int i = 0; i < n; ++i) {
      double x = uni(0, W), y = uni(0.08, 0.3) * W, s = W * uni(0.12, 0.2);
      cloud(x, y, s);
    }
  } else if (mode_ == "day") {
    for (int i = 0; i < 2; ++i) {
      double x = uni(0, W * 0.5), y = uni(0.1, 0.25) * W, s = W * uni(0.07, 0.1);
      cloud(x, y, s);
    }
  }
  ground();

  // sujetos: en el suelo de izquierda a derecha, en el cielo arriba y en el agua sobre las olas
  std::vector<Subject> items;
  for (auto& s : sc_.subjects)
    for (int k = 0; k < std::max(1, std::min(s.count, 6)); ++k) items.push_back(s);
  if (items.empty() && (place_ == "field" || place_ == "garden") && sc_.sky.empty())
    items = {Subject{"tree", std::nullopt, 1}, Subject{"tree", std::nullopt, 1}};
  std::vector<Subject> grounded, water, flying;
  for (auto& s : items) {
    if (SKY_SUBJECTS.count(s.kind))
      flying.push_back(s);
    else if (WATER_SUBJECTS.count(s.kind) && water_place())
      water.push_back(s);
    else
      grounded.push_back(s);
  }
  auto sz = [](const std::string& k) {
    auto it = SIZE.find(k);
    return it == SIZE.end() ? 0.2 : it->second;
  };
  std::stable_sort(grounded.begin(), grounded.end(), [&](auto& a, auto& b) { return sz(a.kind) > sz(b.kind); });   // lo grande detras
  double floor = water_place() ? water_bottom_ : H;
  if (place_ == "sea" && !grounded.empty()) {   // en alta mar lo que va en tierra se apoya en una isla/roca
    floor = (int)(H + (W - H) * 0.55);
    d.ellipse(W * 0.05, floor - W * 0.04, W * 0.95, floor + W * 0.1, lit({205, 185, 135}));
  }
  size_t n = grounded.size();
  for (size_t k = 0; k < n; ++k) {
    const auto& s = grounded[k];
    double slot = (k + 0.5) / n;
    double x = W * (0.12 + 0.76 * slot) + uni(-0.04, 0.04) * W;
    double y = n > 1 ? floor + (W - floor) * (0.25 + 0.5 * std::fmod(k * 0.37, 1.0)) : floor + (W - floor) * 0.45;
    double base = sz(s.kind);
    double h = W * base * (n == 1 ? (base < 0.2 ? 1.6 : 1.15) : 1.0) * uni(0.92, 1.08);
    Rgb col = s.color ? *s.color : DEFAULT_COLOR.count(s.kind) ? DEFAULT_COLOR.at(s.kind) : Rgb{200, 200, 200};
    if ((s.kind == "flower" || s.kind == "tree") && !s.color && !sc_.colors.empty()) col = sc_.colors[k % sc_.colors.size()];
    shadow(x, y, h, s.kind);
    draw_subject(s.kind, x, y, h, col);
  }
  for (size_t k = 0; k < water.size(); ++k) {
    const auto& s = water[k];
    double y = s.kind == "boat" ? H + (W - H) * (0.2 + 0.25 * (k % 2)) : H + (W - H) * uni(0.5, 0.9);
    draw_subject(s.kind, W * (0.2 + 0.6 * (k + 0.5) / water.size()), y, W * SIZE.at(s.kind),
                 s.color ? *s.color : DEFAULT_COLOR.at(s.kind));
  }
  for (size_t k = 0; k < flying.size(); ++k) {
    const auto& s = flying[k];
    double x = W * (0.15 + 0.7 * (k + 0.5) / flying.size()) + uni(-30, 30);
    double y = W * uni(0.12, 0.35);
    draw_subject(s.kind, x, y, W * SIZE.at(s.kind), s.color ? *s.color : DEFAULT_COLOR.at(s.kind));
  }

  weather();
  glow_.gaussian_blur(W / 40.0);
  for (size_t i = 0; i < img_.px.size(); ++i) img_.px[i] = (uint8_t)std::min(255, (int)img_.px[i] + glow_.px[i]);
  if (has(sc_.weather, std::string("fog"))) {
    const int fogc[3] = {200, 205, 212};
    for (size_t i = 0; i < img_.px.size(); ++i) img_.px[i] = (uint8_t)std::lround(img_.px[i] * 0.65 + fogc[i % 3] * 0.35);
  }
  Canvas out(W / 2, W / 2, 0);
  stbir_resize_uint8(img_.px.data(), W, W, 0, out.px.data(), out.w, out.h, 0, 3);
  return out;
}

}  // namespace

json Scene::to_json() const {
  json subj = json::array();
  for (auto& s : subjects)
    subj.push_back({{"kind", s.kind}, {"color", s.color ? json(*s.color) : json(nullptr)}, {"count", s.count}});
  json cols = json::array();
  for (auto& c : colors) cols.push_back(c);
  return {{"time", time},  {"weather", weather}, {"places", places}, {"sky", sky},
          {"subjects", subj}, {"colors", cols},  {"abstract", abstract}};
}

Scene parse(const std::string& prompt) {
  auto toks = tokens(prompt);
  Scene sc;
  std::string time;
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& tok = toks[i];
    if (auto c = color_of(tok)) sc.colors.push_back(*c);
    // «rosa» es color salvo que vaya como sustantivo («una rosa», «rosas rojas»)
    const Hit* hit;
    static const std::set<std::string> DET = {"una", "unas", "las", "la", "de", "con"};
    if ((tok == "rosa" || tok == "rosas") && i && DET.count(toks[i - 1]) && !(i + 1 < toks.size() && WORDS.count(toks[i + 1])))
      hit = &WORDS.at("rosa_flor");
    else
      hit = lookup(tok);
    if (!hit) continue;
    const auto& [cat, key] = *hit;
    if (cat == "time") {
      if (time.empty()) time = key;
    } else if (cat == "weather") {
      sc.weather.push_back(key);
    } else if (cat == "place") {
      if (!has(sc.places, key)) sc.places.push_back(key);
    } else if (cat == "sky") {
      if (!has(sc.sky, key)) sc.sky.push_back(key);
    } else if (cat == "style") {
      sc.abstract = true;
    } else {
      // espanol: «gato negro»; ingles: «black cat»; se mira tambien dos palabras atras/adelante
      std::optional<Rgb> color;
      for (long j : {(long)i + 1, (long)i - 1, (long)i + 2, (long)i - 2}) {
        if (j < 0 || j >= (long)toks.size()) continue;
        auto c = color_of(toks[j]);
        if (c && !(j > (long)i && lookup(toks[j - 1]) && j - 1 != (long)i)) {
          color = c;
          break;
        }
      }
      int count = 1;
      bool found = false;
      for (long j : {(long)i - 1, (long)i - 2}) {
        if (j < 0 || j >= (long)toks.size()) continue;
        auto it = NUMBERS.find(toks[j]);
        if (it != NUMBERS.end()) {
          count = it->second;
          found = true;
          break;
        }
        if (is_digits(toks[j])) {
          count = std::min(std::stoi(toks[j].substr(0, 6)), 6);
          found = true;
          break;
        }
      }
      if (!found && ends_with(tok, "s") && lookup(tok) && !WORDS.count(tok)) count = 2;   // plural sin numero
      if ((key == "person" || key == "child") && (tok == "gente" || tok == "people" || tok == "children"))
        count = std::max(count, 3);
      sc.subjects.push_back({key, color, count});
    }
  }
  if (time.empty())
    time = has(sc.sky, std::string("moon")) || has(sc.sky, std::string("star")) || has(sc.places, std::string("space"))
               ? "night"
               : "day";
  sc.time = time;
  return sc;
}

Canvas compose(const std::string& prompt, long long seed, int size) {
  Scene sc = parse(prompt);
  // el azar depende de la escena (lugar, hora, clima, cielo) y de la semilla: cambiar solo el sujeto no mueve el fondo
  std::string key = json{sc.places, sc.time, sc.weather, sc.sky}.dump();
  return Painter(sc, (uint64_t)crc32(key) ^ (uint64_t)seed, std::max(16, size)).paint();
}

bool understood(const std::string& prompt) {
  Scene s = parse(prompt);
  if (!s.places.empty() || !s.subjects.empty() || !s.sky.empty() || !s.weather.empty()) return true;
  for (auto& t : tokens(prompt))
    if (auto* h = lookup(t); h && h->cat == "time") return true;
  return false;
}

}  // namespace nx::media::scene
