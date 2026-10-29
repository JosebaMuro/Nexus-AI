// Imagen (media/image.py): generacion (stable-diffusion.cpp > arte procedural sin modelo) y analisis
// (LLM multimodal > estadisticas de la imagen).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "llm/llm.h"
#include "media/media.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
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

// ======================================================================== stable-diffusion.cpp
std::set<std::string> sd_flags(const std::string& help) {
  static const Rx OPT(R"((--[a-z0-9][a-z0-9_-]+))");
  std::set<std::string> out;
  for (auto& m : OPT.finditer(help)) out.insert(m.str(1));
  return out;
}

SdPlan sd_plan(const json& model, const json& profile, const std::string& variant, const std::set<std::string>& flags,
               int size) {
  SdPlan p;
  fs::path path = P(jstr(model, "path"));
  auto has = [&](const char* f) { return flags.empty() || flags.count(f) > 0; };
  json dif = model.value("difusion", json());
  if (!dif.is_object() && jstr(model, "format") == "gguf") dif = models::diffusion_info(path);   // modelos de antes
  p.arch = dif.is_object() ? jstr(dif, "arch") : "";
  double need_gb = (is_file(path) ? fsize(path) : 0) / 1073741824.0;
  if (dif.is_object() && truthy(dif.value("solo_difusion", json(false)))) {
    p.model_args = {"--diffusion-model", S(path)};
    p.piezas = models::find_components(path, dif, model.value("componentes", json::object()));
    std::vector<std::string> missing;
    for (auto& n : dif.value("necesita", json::array())) {
      std::string piece = n.get<std::string>();
      if (!p.piezas.contains(piece)) {
        missing.push_back(piece);
        continue;
      }
      std::string f = jstr(p.piezas, piece);
      need_gb += fsize(P(f)) / 1073741824.0;
      // el codificador LLM (Z-Image, Qwen-Image) se llamaba --qwen2vl en las versiones antiguas de sd.cpp
      std::string flag = piece == "llm" ? (has("--llm") || !flags.count("--qwen2vl") ? "--llm" : "--qwen2vl")
                                        : "--" + piece;
      p.model_args.insert(p.model_args.end(), {flag, f});
    }
    if (!missing.empty()) {
      static const std::map<std::string, std::string> DESC = {
          {"vae", "la VAE (p. ej. ae.safetensors)"}, {"clip_l", "el codificador clip_l"}, {"clip_g", "el codificador clip_g"},
          {"t5xxl", "el codificador T5-XXL"}, {"llm", "el codificador de texto (un LLM: Qwen3-4B en Z-Image)"}};
      std::vector<std::string> d;
      std::string enc = models::llm_encoder_for(p.arch, S(path.filename()) + " " + S(path.parent_path().filename()));
      for (auto& m : missing)
        d.push_back(m == "llm" && !enc.empty() ? "el codificador de texto (el LLM " + enc + " en GGUF)"
                    : DESC.count(m)            ? DESC.at(m)
                                               : m);
      p.error = "El modelo " + jstr(model, "id") + " (" + p.arch + ") es solo el modelo de difusion y le falta " +
                join(d, ", ") + ": descargalo en Modelos y ponlo en la misma carpeta";
      // habia LLM en la carpeta de modelos pero de otra familia o tamano: no se entienden con este modelo de imagen
      if (p.piezas.contains("_descartados"))
        p.error += ". Los LLM que hay (" + join(p.piezas["_descartados"].get<std::vector<std::string>>(), ", ") +
                   ") no sirven como codificador: este modelo necesita " + (enc.empty() ? std::string("otro") : enc);
    }
  } else {
    p.model_args = {"-m", S(path)};
    json comps = model.value("componentes", json::object());
    if (comps.is_object() && !jstr(comps, "vae").empty() && is_file(P(jstr(comps, "vae"))))
      p.model_args.insert(p.model_args.end(), {"--vae", jstr(comps, "vae")});
  }
  // ajustes por hardware
  bool gpu = !starts_with(variant, "cpu") && variant != "system";
  double vram = jnum(profile.value("vram_gb", json(0))), ram = jnum(profile.value("ram_gb", json(0)));
  bool metal = jstr(profile, "accelerator") == "metal";
  if (gpu) {
    // atencion rapida (menos memoria): las versiones nuevas la ponen solas (--flash-attn auto); las de antes, a mano
    if (flags.count("--diffusion-fa") && !flags.count("--flash-attn")) p.perf_args.push_back("--diffusion-fa");
    double room = metal ? ram * 0.65 : vram * 0.9;
    // modelo + codificadores no caben en la VRAM: los codificadores van a la RAM y, si aun asi no cabe, los pesos se
    // suben a la GPU por partes. Las versiones con --auto-fit ya reparten solas entre VRAM, RAM y disco.
    if (room > 0 && need_gb > room && !flags.count("--auto-fit")) {
      if (has("--clip-on-cpu") && p.model_args[0] == "--diffusion-model") p.perf_args.push_back("--clip-on-cpu");
      if (has("--offload-to-cpu")) p.perf_args.push_back("--offload-to-cpu");
    }
    if ((size >= 1024 || (room > 0 && room < 6)) && has("--vae-tiling")) p.perf_args.push_back("--vae-tiling");
  } else {
    int threads = (int)jnum(profile.value("llm", json::object()).value("n_threads", json(0)), 0);
    if (threads > 0) p.perf_args.insert(p.perf_args.end(), {"-t", std::to_string(threads)});
    if (size >= 1024 && has("--vae-tiling")) p.perf_args.push_back("--vae-tiling");
  }
  json gen = model.value("gen", json::object());
  if (gen.is_object() && !jstr(gen, "sampler").empty() && has("--sampling-method"))
    p.perf_args.insert(p.perf_args.end(), {"--sampling-method", jstr(gen, "sampler")});
  return p;
}

// ======================================================================== ImageEngine
ImageEngine::ImageEngine(Config& config, models::Catalog& catalog, runtimes::RuntimeManager& rt, ProfileGetter profile,
                         llm::LLMManager* llm)
    : config_(config), catalog_(catalog), rt_(rt), profile_(std::move(profile)), llm_(llm) {
  // el sd-server se para tras unos minutos sin uso: libera la memoria del modelo de imagen para el LLM
  // el LLM va a cargar en la GPU: el sd-server parado sin uso suelta la VRAM (si esta generando, no se toca)
  if (llm_)
    llm_->before_llm_gpu_start = [this] {
      std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
      if (lk.owns_lock() && server_ && server_->running()) {
        L.info("El LLM vuelve a la GPU: se para el sd-server para dejarle la VRAM");
        stop_server();
      }
    };
  idle_ = std::thread([this] {
    while (!quit_) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      std::lock_guard<std::mutex> lk(mu_);
      double mins = config_.num("media.sd_servidor_minutos", 10);
      if (server_ && server_used_ > 0 && now() - server_used_ > mins * 60) {
        L.info("sd-server sin uso desde hace ", fnum(mins, 0), " min: se para para liberar memoria");
        stop_server();
      }
    }
  });
}

ImageEngine::~ImageEngine() {
  if (llm_) llm_->before_llm_gpu_start = nullptr;
  quit_ = true;
  if (idle_.joinable()) idle_.join();
  std::lock_guard<std::mutex> lk(mu_);
  stop_server();
}

void ImageEngine::stop_server() {
  if (server_ && server_->running()) {
    server_->terminate();
    if (!server_->wait(10)) server_->kill();
  }
  server_.reset();
  server_key_.clear();
}

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
  json st = {{"model", m.is_null() ? json() : m["id"]},
             {"runtime", sd ? json(sd->variant) : json()},
             {"engine", !m.is_null() && sd ? "sd.cpp" : "escena"}};
  if (!m.is_null() && sd) {
    SdPlan plan = sd_plan(m, profile_(), sd->variant, flags_of(sd->exe));
    st["arch"] = plan.arch;
    st["piezas"] = plan.piezas;
    if (!plan.error.empty()) st["falta"] = plan.error;
    std::lock_guard<std::mutex> lk(mu_);
    st["servidor"] = server_ && server_->running();
  }
  return st;
}

const std::set<std::string>& ImageEngine::flags_of(const fs::path& exe) {
  std::lock_guard<std::mutex> lk(mu_);
  std::string k = S(exe);
  if (!flags_.count(k)) {
    proc::Options o;
    o.cwd = exe.parent_path();
    o.capture = true;
    o.env = runtimes::lib_env(exe);
    auto r = proc::run({k, "--help"}, 30, o);
    flags_[k] = sd_flags(r.out + "\n" + r.err);
  }
  return flags_[k];
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
    // el prompt en ingles se prepara con el LLM ANTES de que el modelo de imagen ocupe la GPU
    const std::string en = english(prompt);
    // el LLM y el modelo de imagen en la misma GPU: si no caben los dos, el LLM la presta mientras se genera y el
    // sd-server la suelta al terminar. Antes se cargaban los dos, sd.cpp se quedaba sin VRAM y acababa en la CPU
    // (minutos) o en la imagen sin IA.
    bool lent = false;
    auto lend = [&](const std::string& reason) {
      if (lent || !llm_) return;
      lent = true;
      vram_taken_ = 0;
      llm_->lend_gpu(reason);
    };
    struct Back {
      ImageEngine* self;
      bool& lent;
      ~Back() {
        self->vram_taken_ = 0;
        if (!lent) return;
        {
          std::lock_guard<std::mutex> lk(self->mu_);
          self->stop_server();
        }
        self->llm_->return_gpu();
      }
    } back{this, lent};
    double llm_gb = llm_ && !starts_with(sd->variant, "cpu") && sd->variant != "system" ? llm_->llm_gpu_gb() : 0;
    if (llm_gb > 0) {
      SdPlan plan = sd_plan(m, profile_(), sd->variant, flags_of(sd->exe), size.value_or(512));
      if (plan.error.empty() && must_lend(sd->variant, plan, size.value_or(512), llm_gb))
        lend("se genera la imagen");
      else
        vram_taken_ = llm_gb;
    }
    static const Rx OOM(R"(out of memory|failed to allocate|cudaMalloc|ErrorOutOfDeviceMemory|insufficient memory|alloc.*buffer|bad_alloc|OOM)", Rx::I);
    try {
      return sdcpp(sd->exe, sd->variant, m, en, negative, size, steps, seed, out);
    } catch (const std::exception& e) {
      std::string msg = trim(e.what());
      why = "stable-diffusion.cpp fallo: " + uright(msg, 300);
      // sin memoria en la GPU con el LLM cargado: se le pide la GPU y se repite en la GPU antes de ir a la CPU
      if (!lent && llm_gb > 0 && OOM.test(msg)) {
        L.warning("sd.cpp sin memoria en la GPU con el LLM cargado: se le presta la GPU y se repite");
        {
          std::lock_guard<std::mutex> lk(mu_);
          stop_server();
        }
        lend("se repite la imagen sin memoria");
        try {
          json r = sdcpp(sd->exe, sd->variant, m, en, negative, size, steps, seed, out);
          r["note"] = "El LLM ha dejado la GPU al modelo de imagen mientras se generaba";
          return r;
        } catch (const std::exception& e2) {
          msg = trim(e2.what());
          why = "stable-diffusion.cpp fallo: " + uright(msg, 300);
        }
      }
      // una GPU que no arranca (Metal virtual, driver roto...): antes del compositor se prueba en la CPU
      auto cpu = starts_with(sd->variant, "cpu") ? std::nullopt : rt_.exe("sd", profile_(), "cpu");
      if (cpu && starts_with(cpu->variant, "cpu")) {
        L.warning("sd.cpp fallo en ", sd->variant, " (", uright(msg, 200), "); se reintenta en la CPU");
        try {
          json r = sdcpp(cpu->exe, cpu->variant, m, en, negative, size, steps, seed, out);
          r["note"] = "La GPU (" + sd->variant + ") no pudo generar la imagen; se ha hecho en la CPU";
          return r;
        } catch (const std::exception& e2) {
          why = "stable-diffusion.cpp fallo en " + sd->variant + " y en la CPU: " + uright(trim(e2.what()), 300);
        }
      }
      L.warning("sd.cpp fallo (", uright(why, 300), "); usando el compositor procedural");
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

bool ImageEngine::must_lend(const std::string& variant, const SdPlan& plan, int size, double llm_gb) {
  std::string mode = config_.str("media.gpu_compartida", "auto");
  if (mode == "no" || mode == "off" || starts_with(variant, "cpu") || variant == "system") return false;
  if (mode == "siempre" || mode == "always") return true;
  // lo que ocupa el modelo de imagen: sus ficheros (difusion, VAE, codificadores) y la memoria de trabajo
  double need = 0;
  for (auto& a : plan.model_args)
    if (!starts_with(a, "-") && is_file(P(a))) need += fsize(P(a)) / 1073741824.0;
  need += size >= 1024 ? 2.5 : 1.2;
  json prof = profile_();
  double vram = jnum(prof.value("vram_gb", json(0))), ram = jnum(prof.value("ram_gb", json(0)));
  double room = jstr(prof, "accelerator") == "metal" ? ram * 0.65 : vram * 0.92;
  if (room <= 0) return true;   // no se sabe cuanta VRAM hay: mejor no arriesgar
  bool lend = need + llm_gb > room;
  L.info("Imagen en la GPU: necesita ", fnum(need, 1), " GB, el LLM ocupa ", fnum(llm_gb, 1), " GB de ", fnum(room, 1),
         lend ? ": el LLM deja la GPU mientras se genera" : ": caben los dos");
  return lend;
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

bool ImageEngine::via_server(const fs::path& cli, const SdPlan& plan, const json& req, const fs::path& out,
                             std::string& why) {
  fs::path exe = cli.parent_path() / P(std::string("sd-server") + (sys::is_windows() ? ".exe" : ""));
  std::string mode = config_.str("media.sd_servidor", "auto");
  if (mode == "no" || mode == "off" || !is_file(exe)) return false;
  std::lock_guard<std::mutex> lk(mu_);
  std::string key = S(exe) + "|" + join(plan.model_args, "|") + "|" + join(plan.perf_args, "|");
  if (!(server_ && server_->running() && server_key_ == key)) {
    stop_server();
    server_port_ = net::free_port(7861);
    std::vector<std::string> cmd = {S(exe)};
    cmd.insert(cmd.end(), plan.model_args.begin(), plan.model_args.end());
    for (size_t i = 0; i < plan.perf_args.size(); ++i) {
      // -t (hilos) vale igual; las opciones de muestreo van en cada peticion
      if (plan.perf_args[i] == "--sampling-method") {
        ++i;
        continue;
      }
      cmd.push_back(plan.perf_args[i]);
    }
    cmd.insert(cmd.end(), {"--listen-ip", "127.0.0.1", "--listen-port", std::to_string(server_port_)});
    proc::Options po;
    po.cwd = exe.parent_path();
    po.env = runtimes::lib_env(exe);
    po.output_file = paths::logs_dir() / "sd-server.log";
    server_ = std::make_unique<proc::Process>();
    if (!server_->start(cmd, po)) {
      why = "sd-server no arranca: " + server_->error();
      server_.reset();
      return false;
    }
    double t0 = now();
    bool up = false;
    while (now() - t0 < 900 && server_->running()) {
      // el servidor responde cuando ha cargado el modelo
      if (http::get(cat("http://127.0.0.1:", server_port_, "/sdapi/v1/sd-models"), {}, 2).status == 200) {
        up = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!up) {
      std::string tail = read_text(paths::logs_dir() / "sd-server.log");
      why = "sd-server no cargo el modelo: " + uright(valid_utf8(tail), 300);
      stop_server();
      return false;
    }
    server_key_ = key;
    L.info("sd-server listo con el modelo cargado en ", fnum(now() - t0, 1), "s");
  }
  auto r = http::post_json(cat("http://127.0.0.1:", server_port_, "/sdapi/v1/txt2img"), req, {}, 1800);
  server_used_ = now();
  json j = r.json_body();
  if (r.status != 200 || !j.is_object() || !j.contains("images") || !j["images"].is_array() || j["images"].empty()) {
    why = cat("sd-server respondio ", r.status, ": ", uleft(r.body, 200));
    if (!server_->running()) stop_server();
    return false;
  }
  std::string b64 = j["images"][0].get<std::string>();
  if (auto comma = b64.find("base64,"); comma != std::string::npos) b64 = b64.substr(comma + 7);
  return write_file(out, b64decode(b64));
}

json ImageEngine::sdcpp(const fs::path& exe, const std::string& variant, const json& model, const std::string& prompt,
                        const std::string& negative, std::optional<int> size_opt, std::optional<int> steps_opt,
                        long long seed, const fs::path& out) {
  json gen = model.value("gen", json::object());
  if (!gen.is_object()) gen = json::object();
  json prof = profile_();
  // el LLM se queda en la GPU: sd.cpp reparte con lo que queda (codificadores a la CPU, pesos por partes...)
  if (vram_taken_ > 0 && jnum(prof.value("vram_gb", json(0))) > 0)
    prof["vram_gb"] = std::max(0.5, jnum(prof["vram_gb"]) - vram_taken_);
  int size = size_opt && *size_opt ? *size_opt
             : truthy(gen.value("size", json()))
                 ? (int)jnum(gen["size"])
                 : (int)jnum(prof["image"]["resolution"], 512);
  int steps = steps_opt && *steps_opt ? *steps_opt : truthy(gen.value("steps", json())) ? (int)jnum(gen["steps"]) : 4;
  double cfg = jnum(gen.value("cfg", json(1.0)), 1.0);
  SdPlan plan = sd_plan(model, prof, variant, flags_of(exe), size);
  if (!plan.error.empty()) throw std::runtime_error(plan.error);
  double t0 = now();
  std::string why;
  json req = {{"prompt", prompt}, {"negative_prompt", negative}, {"width", size}, {"height", size},
              {"steps", steps},   {"cfg_scale", cfg},          {"seed", seed}};
  if (!jstr(gen, "sampler").empty()) req["sampler_name"] = jstr(gen, "sampler");
  std::string engine = "sd.cpp (" + variant + ")";
  if (via_server(exe, plan, req, out, why)) {
    engine = "sd.cpp servidor (" + variant + ")";
  } else {
    if (!why.empty()) L.warning(why, ": se usa sd-cli");
    std::vector<std::string> cmd = {S(exe)};
    cmd.insert(cmd.end(), plan.model_args.begin(), plan.model_args.end());
    cmd.insert(cmd.end(), {"-p", prompt, "--steps", std::to_string(steps), "--cfg-scale", pyfloat(cfg), "-W",
                           std::to_string(size), "-H", std::to_string(size), "-s", std::to_string(seed), "-o", S(out)});
    cmd.insert(cmd.end(), plan.perf_args.begin(), plan.perf_args.end());
    if (!negative.empty()) cmd.insert(cmd.end(), {"-n", negative});
    proc::Options po;
    po.cwd = exe.parent_path();
    po.capture = true;
    po.env = runtimes::lib_env(exe);
    auto r = proc::run(cmd, 1800, po);
    if (!r.ok() || !path_exists(out)) {
      std::string msg = !r.err.empty() ? r.err : r.out;
      if (!r.started) msg = "no se pudo iniciar sd-cli";
      if (r.timed_out) msg = "tiempo agotado";
      throw std::runtime_error(uright(msg, 400));
    }
  }
  return {{"engine", engine},
          {"model", model["id"]},
          {"arch", plan.arch},
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
    // corte en un cambio de valor: dos cajas con el mismo color darian colores dominantes repetidos
    auto same = [&](size_t i) { return pxs[i][best_ch] == pxs[i - 1][best_ch]; };
    size_t up = mid, down = mid;
    while (up < e && same(up)) ++up;
    while (down > s + 1 && same(down)) --down;
    mid = up < e && (up - mid <= mid - down || down <= s + 1 && same(down)) ? up : down;
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
  std::vector<uint8_t> thumb = rgb_in;
  int w = w0, h = h0;
  thumbnail(thumb, w, h, 128);
  auto q = median_cut(thumb, 5);
  std::vector<std::string> colors;
  for (size_t i = 0; i < q.size() && i < 5; ++i) {
    char buf[8];
    snprintf(buf, sizeof buf, "#%02x%02x%02x", q[i].second[0], q[i].second[1], q[i].second[2]);
    colors.push_back(buf);
  }
  std::vector<int> lum((size_t)w * h);
  double lsum = 0;
  for (size_t i = 0; i < lum.size(); ++i) {
    lum[i] = (thumb[i * 3] * 299 + thumb[i * 3 + 1] * 587 + thumb[i * 3 + 2] * 114) / 1000;
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
