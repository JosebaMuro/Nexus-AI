// Imagen (media/image.py): generacion (stable-diffusion.cpp > arte procedural sin modelo) y analisis
// (LLM multimodal > estadisticas de la imagen).
#include <algorithm>
#include <cmath>
#include <thread>

#include "llm/llm.h"
#include "media/media.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"

namespace nx::media {

static auto L = log::get("image");

static const std::vector<std::pair<std::string, std::array<uint8_t, 3>>> COLOR_WORDS = {
    {"rojo", {255, 40, 60}},     {"red", {255, 40, 60}},        {"azul", {40, 120, 255}},    {"blue", {40, 120, 255}},
    {"verde", {40, 220, 120}},   {"green", {40, 220, 120}},     {"amarillo", {255, 220, 40}}, {"yellow", {255, 220, 40}},
    {"morado", {170, 60, 255}},  {"purple", {170, 60, 255}},    {"violeta", {170, 60, 255}}, {"rosa", {255, 90, 200}},
    {"pink", {255, 90, 200}},    {"naranja", {255, 140, 30}},   {"orange", {255, 140, 30}},  {"cian", {0, 240, 255}},
    {"cyan", {0, 240, 255}},     {"neon", {0, 255, 200}},       {"oro", {255, 200, 80}},     {"gold", {255, 200, 80}},
    {"fuego", {255, 90, 20}},    {"fire", {255, 90, 20}},       {"mar", {20, 140, 200}},     {"ocean", {20, 140, 200}},
    {"bosque", {30, 160, 70}},   {"forest", {30, 160, 70}},     {"noche", {30, 30, 90}},     {"night", {30, 30, 90}},
    {"atardecer", {255, 120, 80}}, {"sunset", {255, 120, 80}},  {"espacio", {90, 60, 200}},  {"space", {90, 60, 200}},
};

std::string slug_name(const std::string& t, size_t max, const std::string& fallback) {
  static const Rx NON(R"([^a-z0-9]+)");
  std::string s = trim(uleft(NON.sub(normalize(t), "-"), max), "-");
  return s.empty() ? fallback : s;
}

// ======================================================================== lienzo
Canvas::Canvas(int width, int height, uint32_t rgb) : w(width), h(height), px((size_t)width * height * 3) {
  for (size_t i = 0; i < px.size(); i += 3) {
    px[i] = (rgb >> 16) & 255;
    px[i + 1] = (rgb >> 8) & 255;
    px[i + 2] = rgb & 255;
  }
}

void Canvas::plot(int x, int y, const uint8_t c[3]) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  uint8_t* p = &px[((size_t)y * w + x) * 3];
  p[0] = c[0];
  p[1] = c[1];
  p[2] = c[2];
}

void Canvas::disc(double cx, double cy, double r, const uint8_t c[3]) {
  if (r <= 0.5) {
    plot((int)std::floor(cx), (int)std::floor(cy), c);
    return;
  }
  int x0 = (int)std::floor(cx - r), x1 = (int)std::ceil(cx + r), y0 = (int)std::floor(cy - r), y1 = (int)std::ceil(cy + r);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      if ((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy) <= r * r) plot(x, y, c);
}

void Canvas::line(const std::vector<std::pair<double, double>>& pts, const uint8_t c[3], int width) {
  for (size_t i = 1; i < pts.size(); ++i) {
    auto [xa, ya] = pts[i - 1];
    auto [xb, yb] = pts[i];
    double len = std::hypot(xb - xa, yb - ya);
    int n = std::max(1, (int)std::ceil(len));
    for (int k = 0; k <= n; ++k) {
      double t = (double)k / n;
      disc(xa + (xb - xa) * t, ya + (yb - ya) * t, width / 2.0, c);
    }
  }
}

void Canvas::ellipse_outline(double x0, double y0, double x1, double y1, const uint8_t c[3], int width) {
  double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
  int n = std::max(32, (int)(std::max(rx, ry) * 7));
  for (int k = 0; k < n; ++k) {
    double a = 2 * M_PI * k / n;
    for (int wdt = 0; wdt < width; ++wdt) disc(cx + (rx - wdt) * std::cos(a), cy + (ry - wdt) * std::sin(a), 0.6, c);
  }
}

// tres pasadas de caja aproximan un desenfoque gaussiano (como ImageFilter.GaussianBlur)
void Canvas::gaussian_blur(double radius) {
  int r = std::max(1, (int)std::lround(radius * 0.58));
  std::vector<float> a(px.begin(), px.end()), b(a.size());
  auto pass = [&](bool horiz) {
    int len = horiz ? w : h, lines = horiz ? h : w;
    for (int l = 0; l < lines; ++l)
      for (int ch = 0; ch < 3; ++ch) {
        auto idx = [&](int i) { return horiz ? ((size_t)l * w + i) * 3 + ch : ((size_t)i * w + l) * 3 + ch; };
        double acc = 0;
        for (int i = -r; i <= r; ++i) acc += a[idx(std::clamp(i, 0, len - 1))];
        for (int i = 0; i < len; ++i) {
          b[idx(i)] = (float)(acc / (2 * r + 1));
          acc += a[idx(std::clamp(i + r + 1, 0, len - 1))] - a[idx(std::clamp(i - r, 0, len - 1))];
        }
      }
    std::swap(a, b);
  };
  for (int k = 0; k < 3; ++k) {
    pass(true);
    pass(false);
  }
  for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)std::clamp((int)std::lround(a[i]), 0, 255);
}

static void png_sink(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, size); }

std::string Canvas::png() const {
  std::string out;
  stbi_write_png_to_func(png_sink, &out, w, h, 3, px.data(), w * 3);
  return out;
}

bool Canvas::save_png(const fs::path& p) const { return write_file(p, png()); }

// ======================================================================== ImageEngine
ImageEngine::ImageEngine(Config& config, models::Catalog& catalog, runtimes::RuntimeManager& rt, ProfileGetter profile,
                         llm::LLMManager* llm)
    : config_(config), catalog_(catalog), rt_(rt), profile_(std::move(profile)), llm_(llm) {}

json ImageEngine::model() {
  std::string mid = config_.str("media.image_model", "");
  json entry = mid.empty() ? json() : catalog_.get(mid);
  if (entry.is_null())
    for (auto& m : catalog_.list())
      if (jstr(m, "kind") == "image") return m;
  return entry;
}

json ImageEngine::status() {
  json m = model();
  auto sd = rt_.exe("sd", profile_());
  return {{"model", m.is_null() ? json() : m["id"]},
          {"runtime", sd ? json(sd->variant) : json()},
          {"engine", !m.is_null() && sd ? "sd.cpp" : "escena"}};
}

json ImageEngine::generate(const std::string& prompt, const std::string& negative, std::optional<int> size,
                           std::optional<int> steps, std::optional<long long> seed_opt, const std::string& idea) {
  long long seed = seed_opt ? *seed_opt : (long long)now() % 100000;
  fs::path out = paths::sub({"outputs", "images"}) /
                 P(std::to_string((long long)now()) + "_" + slug_name(idea.empty() ? prompt : idea, 40, "imagen") + ".png");
  json m = model();
  auto sd = rt_.exe("sd", profile_());
  std::string why = m.is_null() ? "No hay modelo de imagen descargado"
                    : !sd       ? "No hay motor stable-diffusion.cpp para este equipo"
                                : "El modelo " + jstr(m, "id") + " no es compatible con sd.cpp (" + jstr(m, "format") + ")";
  std::string fmt = jstr(m, "format");
  if (!m.is_null() && sd && (fmt == "gguf" || fmt == "safetensors" || fmt == "ckpt")) {
    try {
      return sdcpp(sd->exe, sd->variant, m, english(prompt), negative, size, steps, seed, out);
    } catch (const std::exception& e) {
      L.warning("sd.cpp fallo (", e.what(), "); usando el compositor procedural");
      std::string msg = trim(e.what());
      why = "stable-diffusion.cpp fallo: " + uright(msg, 200);
    }
  }
  std::string text = idea.empty() ? prompt : idea;
  Canvas img = procedural(text, seed, size.value_or(512));
  img.save_png(out);
  std::string note = why + ": imagen compuesta sin IA a partir de las palabras del prompt. Para imagenes neuronales "
                           "descarga un modelo de imagen en Modelos (p. ej. SDXS-512 en CPU).";
  return {{"engine", "procedural"}, {"url", "/outputs/images/" + S(out.filename())}, {"path", S(out)},
          {"seed", seed},          {"note", note}};
}

std::string ImageEngine::english(const std::string& prompt) {
  if (!prompt::looks_spanish(prompt)) return prompt;
  if (llm_) {
    try {
      if (llm_->has_real_llm()) {
        llm::Opts o;
        o.o = {{"max_tokens", 120}, {"temperature", 0.2}};
        std::string t = llm_->complete(
            "Translate this image description into a concise English prompt for an image generator. Reply with the "
            "prompt only.\n\n" + prompt, "default", o);
        if (!trim(t).empty()) {
          auto lines = splitlines(trim(trim(t), "\""));
          t = lines.empty() ? "" : trim(lines[0]);
        } else {
          t = "";
        }
        size_t n = ulen(t);
        if (n > 3 && n < 600) return t;
      }
    } catch (const std::exception& e) {
      L.info("Traduccion con LLM no disponible (", e.what(), "): se usa el diccionario");
    }
  }
  return prompt::translate_rules(prompt);
}

json ImageEngine::sdcpp(const fs::path& exe, const std::string& variant, const json& model, const std::string& prompt,
                        const std::string& negative, std::optional<int> size_opt, std::optional<int> steps_opt,
                        long long seed, const fs::path& out) {
  json gen = model.value("gen", json::object());
  if (!gen.is_object()) gen = json::object();
  json prof = profile_();
  int size = size_opt && *size_opt ? *size_opt
             : truthy(gen.value("size", json()))
                 ? (int)jnum(gen["size"])
                 : (int)jnum(prof["image"]["resolution"], 512);
  int steps = steps_opt && *steps_opt ? *steps_opt : truthy(gen.value("steps", json())) ? (int)jnum(gen["steps"]) : 4;
  int threads = variant == "cpu" ? (int)jnum(prof["llm"]["n_threads"], 0) : 0;
  std::vector<std::string> cmd = {S(exe),          "-m", jstr(model, "path"), "-p", prompt, "--steps", std::to_string(steps),
                                  "--cfg-scale",   pyfloat(jnum(gen.value("cfg", json(1.0)), 1.0)),
                                  "-W",            std::to_string(size), "-H", std::to_string(size), "-s",
                                  std::to_string(seed), "-o", S(out)};
  if (!negative.empty()) cmd.insert(cmd.end(), {"-n", negative});
  if (threads)
    cmd.insert(cmd.end(), {"-t", std::to_string(std::max(threads, (int)std::max(1u, std::thread::hardware_concurrency())))});
  double t0 = now();
  auto r = proc::run(cmd, 900, {.cwd = exe.parent_path(), .capture = true});
  if (!r.ok() || !path_exists(out)) {
    std::string msg = !r.err.empty() ? r.err : r.out;
    if (!r.started) msg = "no se pudo iniciar sd-cli";
    if (r.timed_out) msg = "tiempo agotado";
    throw std::runtime_error(uright(msg, 400));
  }
  return {{"engine", "sd.cpp (" + variant + ")"},
          {"model", model["id"]},
          {"url", "/outputs/images/" + S(out.filename())},
          {"path", S(out)},
          {"seed", seed},
          {"steps", steps},
          {"size", size},
          {"seconds", round_to(now() - t0, 1)},
          {"prompt", prompt}};
}

Canvas ImageEngine::procedural(const std::string& prompt, long long seed, int size) {
  // arte abstracto determinista (campo de flujo con la paleta del prompt)
  std::mt19937_64 rng((uint64_t)(crc32(prompt) ^ (uint64_t)seed));
  auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
  auto integer = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b - 1)(rng); };
  std::string low = lower(prompt);
  std::vector<std::array<uint8_t, 3>> palette;
  for (auto& [w, c] : COLOR_WORDS)
    if (contains(low, w)) palette.push_back(c);
  if (palette.empty())
    for (int i = 0; i < 3; ++i) palette.push_back({(uint8_t)integer(40, 255), (uint8_t)integer(40, 255), (uint8_t)integer(40, 255)});
  uint32_t bg = ((uint32_t)(palette[0][0] * 0.12) << 16) | ((uint32_t)(palette[0][1] * 0.12) << 8) | (uint32_t)(palette[0][2] * 0.12);
  Canvas img(size, size, bg), glow(size, size, 0);
  double freq = uni(1.5, 4.5);
  double ph0 = uni(0, 6.28), ph1 = uni(0, 6.28);
  for (int i = 0; i < 260; ++i) {
    double x = uni(0, size), y = uni(0, size);
    auto& col = palette[i % palette.size()];
    std::vector<std::pair<double, double>> pts = {{x, y}};
    int steps = integer(20, 70);
    for (int k = 0; k < steps; ++k) {
      double ang = std::sin(x / size * freq * 3.1 + ph0) * 3 + std::cos(y / size * freq * 3.1 + ph1) * 3;
      x += std::cos(ang) * 4;
      y += std::sin(ang) * 4;
      pts.emplace_back(x, y);
    }
    img.line(pts, col.data(), 1);
    glow.line(pts, col.data(), 3);
  }
  int rings = integer(3, 8);
  for (int k = 0; k < rings; ++k) {
    double cx = uni(0, size), cy = uni(0, size), r = uni(size * 0.03, size * 0.12);
    glow.ellipse_outline(cx - r, cy - r, cx + r, cy + r, palette.back().data(), 2);
  }
  glow.gaussian_blur(6);
  for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = (uint8_t)std::min(255, (int)img.px[i] + glow.px[i]);
  return img;
}

// ------------------------------------------------------------------ analisis
static void thumbnail(std::vector<uint8_t>& rgb, int& w, int& h, int max_side) {
  if (w <= max_side && h <= max_side) return;
  double k = std::min((double)max_side / w, (double)max_side / h);
  int nw = std::max(1, (int)std::lround(w * k)), nh = std::max(1, (int)std::lround(h * k));
  std::vector<uint8_t> out((size_t)nw * nh * 3);
  stbir_resize_uint8(rgb.data(), w, h, 0, out.data(), nw, nh, 0, 3);
  rgb.swap(out);
  w = nw;
  h = nh;
}

// cuantizacion por corte de mediana (como Image.quantize): colores y cuantos pixeles caen en cada uno
static std::vector<std::pair<long, std::array<int, 3>>> median_cut(const std::vector<uint8_t>& rgb, int n) {
  std::vector<std::array<uint8_t, 3>> pxs(rgb.size() / 3);
  for (size_t i = 0; i < pxs.size(); ++i) pxs[i] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
  std::vector<std::pair<size_t, size_t>> boxes = {{0, pxs.size()}};
  while ((int)boxes.size() < n) {
    int best = -1, best_ch = 0, best_range = -1;
    for (size_t b = 0; b < boxes.size(); ++b) {
      auto [s, e] = boxes[b];
      if (e - s < 2) continue;
      for (int ch = 0; ch < 3; ++ch) {
        int lo = 255, hi = 0;
        for (size_t i = s; i < e; ++i) {
          lo = std::min<int>(lo, pxs[i][ch]);
          hi = std::max<int>(hi, pxs[i][ch]);
        }
        if (hi - lo > best_range) {
          best_range = hi - lo;
          best = (int)b;
          best_ch = ch;
        }
      }
    }
    if (best < 0 || best_range <= 0) break;
    auto [s, e] = boxes[best];
    std::sort(pxs.begin() + s, pxs.begin() + e, [&](auto& a, auto& b) { return a[best_ch] < b[best_ch]; });
    size_t mid = s + (e - s) / 2;
    boxes[best] = {s, mid};
    boxes.push_back({mid, e});
  }
  std::vector<std::pair<long, std::array<int, 3>>> out;
  for (auto [s, e] : boxes) {
    long sum[3] = {0, 0, 0};
    for (size_t i = s; i < e; ++i)
      for (int ch = 0; ch < 3; ++ch) sum[ch] += pxs[i][ch];
    long cnt = (long)(e - s);
    if (!cnt) continue;
    out.push_back({cnt, {(int)(sum[0] / cnt), (int)(sum[1] / cnt), (int)(sum[2] / cnt)}});
  }
  std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first > b.first; });
  return out;
}

json ImageEngine::describe_stats(const std::vector<uint8_t>& rgb_in, int w0, int h0) {
  std::vector<uint8_t> small = rgb_in;
  int w = w0, h = h0;
  thumbnail(small, w, h, 128);
  auto q = median_cut(small, 5);
  std::vector<std::string> colors;
  for (size_t i = 0; i < q.size() && i < 5; ++i) {
    char buf[8];
    snprintf(buf, sizeof buf, "#%02x%02x%02x", q[i].second[0], q[i].second[1], q[i].second[2]);
    colors.push_back(buf);
  }
  std::vector<int> lum((size_t)w * h);
  double lsum = 0;
  for (size_t i = 0; i < lum.size(); ++i) {
    lum[i] = (small[i * 3] * 299 + small[i * 3 + 1] * 587 + small[i * 3 + 2] * 114) / 1000;
    lsum += lum[i];
  }
  double brightness = lum.empty() ? 0 : lsum / lum.size() / 255;
  double esum = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (x == 0 || y == 0 || x == w - 1 || y == h - 1) continue;   // FIND_EDGES deja el borde en 0
      int v = 8 * lum[(size_t)y * w + x];
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          if (dx || dy) v -= lum[(size_t)(y + dy) * w + (x + dx)];
      esum += std::clamp(v, 0, 255);
    }
  double edges = w * h ? esum / ((double)w * h) / 255 : 0;
  std::string mood = brightness > 0.6 ? "luminosa" : brightness < 0.35 ? "oscura" : "de luz media";
  std::string detail = edges > 0.12 ? "muy detallada" : edges > 0.05 ? "con detalle moderado" : "suave/minimalista";
  std::vector<std::string> top3(colors.begin(), colors.begin() + std::min<size_t>(3, colors.size()));
  return {{"size", {w0, h0}},
          {"dominant_colors", colors},
          {"brightness", round_to(brightness, 2)},
          {"edge_density", round_to(edges, 3)},
          {"summary", "Imagen " + std::to_string(w0) + "x" + std::to_string(h0) + ", " + mood + ", " + detail +
                          "; colores dominantes " + join(top3, ", ") + "."}};
}

json ImageEngine::analyze(const std::string& data, const std::string& question) {
  int w = 0, h = 0, n = 0;
  unsigned char* raw = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), &w, &h, &n, 3);
  if (!raw) throw std::invalid_argument(std::string("cannot identify image file: ") + stbi_failure_reason());
  std::vector<uint8_t> rgb(raw, raw + (size_t)w * h * 3);
  stbi_image_free(raw);
  json stats = describe_stats(rgb, w, h);
  if (llm_ && vision_available()) {
    thumbnail(rgb, w, h, 896);
    Canvas c(w, h);
    c.px = rgb;
    std::string prompt = question.empty() ? "Describe esta imagen con detalle en espanol." : question;
    try {
      std::string text;
      llm::Opts o;
      o.o = {{"max_tokens", 400}};
      json msgs = json::array({{{"role", "user"}, {"content", prompt}, {"images", {b64encode(c.png())}}}});
      llm_->stream(msgs, "default", o, [&](const std::string& p) { text += p; });
      return {{"engine", "vision-llm"}, {"description", text}, {"stats", stats}};
    } catch (const std::exception& e) {
      L.warning("Analisis con vision fallo: ", e.what());
    }
  }
  return {{"engine", "pil"}, {"description", stats["summary"]}, {"stats", stats}};
}

bool ImageEngine::vision_available() {
  auto [backend, model] = llm_->select();
  if (backend == "llamaserver") {
    auto b = std::dynamic_pointer_cast<llm::LlamaServerBackend>(llm_->backend(backend));
    return b && b->supports_vision(model);
  }
  return (backend == "ollama" || backend == "openai") && truthy(config_.get("media.vision_model"));
}

}  // namespace nx::media
