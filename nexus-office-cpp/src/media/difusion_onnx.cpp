// Motor de difusion sobre ONNX Runtime (ver difusion_onnx.h).
#include "media/difusion_onnx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>

#include "hal/cpu.h"
#include "media/media.h"
#include "models/runtimes.h"
#include "onnxruntime_c_api.h"
#include "util/dynlib.h"
#include "util/log.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::difusion {

namespace {

const auto L = log::get("difusion");

using Reloj = std::chrono::steady_clock;
double ms_desde(Reloj::time_point t) { return std::chrono::duration<double, std::milli>(Reloj::now() - t).count(); }

}  // namespace

// ================================================================ utilidades
uint16_t a_fp16(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000u, mant = x & 0x007fffffu;
  int32_t exp = (int32_t)((x >> 23) & 0xff) - 127 + 15;
  if (((x >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00u | (mant ? 0x200u : 0));   // inf / nan
  if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
  if (exp <= 0) {
    if (exp < -10) return (uint16_t)sign;
    mant |= 0x00800000u;
    uint32_t t = 14 - exp;
    uint32_t h = mant >> t;
    if ((mant >> (t - 1)) & 1u) ++h;   // redondeo
    return (uint16_t)(sign | h);
  }
  uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
  if (mant & 0x1000u) ++h;   // redondeo al mas cercano
  return (uint16_t)h;
}

float de_fp16(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ffu, x;
  if (exp == 0) {
    if (mant == 0) {
      x = sign;
    } else {   // subnormal
      exp = 1;
      while (!(mant & 0x400u)) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3ffu;
      x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    x = sign | 0x7f800000u | (mant << 13);
  } else {
    x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &x, 4);
  return f;
}

std::vector<uint8_t> latente_a_rgb(const float* lat, int h, int w) {
  // proyeccion lineal de los 4 canales del latente de SD 1.x/2.x a RGB (la que usan las vistas previas rapidas)
  static const float F[4][3] = {{0.3512f, 0.2297f, 0.3227f}, {0.3250f, 0.4974f, 0.2350f},
                                {-0.2829f, 0.1762f, 0.2721f}, {-0.2120f, -0.2616f, -0.7177f}};
  const size_t n = (size_t)h * w;
  std::vector<uint8_t> rgb(n * 3);
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) {
      float v = 0;
      for (int k = 0; k < 4; ++k) v += lat[k * n + i] * F[k][c];
      v = (v + 1.f) * 127.5f;
      rgb[i * 3 + c] = (uint8_t)std::clamp(v, 0.f, 255.f);
    }
  return rgb;
}

bool es_modelo(const fs::path& d) {
  return is_file(d / "unet" / "model.onnx") && is_file(d / "vae_decoder" / "model.onnx") &&
         is_file(d / "text_encoder" / "model.onnx");
}

std::optional<fs::path> biblioteca_ort(const std::string& variante) {
  const std::string name = dynlib_name("onnxruntime");
  std::string forced = env("NEXUS_ORT_LIB");
  if (!forced.empty()) {
    if (is_file(P(forced))) return P(forced);
    if (is_file(P(forced) / name)) return P(forced) / name;
  }
  std::vector<fs::path> bases = {paths::sub({"runtimes"}) / "onnx", runtimes::bundled_dir() / "onnx"};
  if (!variante.empty())
    for (auto& b : bases)
      if (is_file(b / variante / name)) return b / variante / name;
  // sin variante: la de CPU primero (la mas pequena y la que siempre funciona), despues cualquiera
  for (auto& b : bases)
    if (is_file(b / "cpu" / name)) return b / "cpu" / name;
  for (auto& b : bases) {
    if (!is_dir(b)) continue;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(b, ec))
      if (e.is_directory() && is_file(e.path() / name)) return e.path() / name;
  }
  // Android: junto a las bibliotecas de la app (lib/arm64-v8a de la APK)
  std::string cpu = env("NEXUS_RUNTIME_LLAMA_CPU");
  if (!cpu.empty() && is_file(P(cpu).parent_path() / name)) return P(cpu).parent_path() / name;
  return std::nullopt;
}

std::string variante_de(const std::string& p) {
  if (p == "cuda" || p == "tensorrt") return "cuda";
  if (p == "dml") return "dml";
  if (p == "qnn") return "qnn";
  if (starts_with(p, "openvino")) return "openvino";
  if (p == "rocm") return "rocm";
  return "";
}

std::vector<std::string> proveedores_para(const json& perfil) {
  std::string acc = jstr(perfil, "accelerator"), id = lower(jstr(perfil, "id"));
  std::string soc = perfil.contains("npu") && perfil["npu"].is_object() ? lower(jstr(perfil["npu"], "soc")) : "";
  std::string npu = perfil.contains("npu") && perfil["npu"].is_object() ? lower(jstr(perfil["npu"], "vendor")) : "";
  std::vector<std::string> v;
  auto add = [&](const std::string& p) {
    if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
  };
  bool qualcomm = npu.find("qualcomm") != std::string::npos || soc.find("snapdragon") != std::string::npos ||
                  soc.find("sm8") != std::string::npos || soc.find("qcom") != std::string::npos || acc == "hexagon";
  bool intel = npu.find("intel") != std::string::npos || id.find("intel") != std::string::npos;
  if (sys::is_android()) {
    // Snapdragon: NPU Hexagon por QNN; Tensor, Exynos y Dimensity: NNAPI (sus NPU) y si no XNNPACK
    if (qualcomm) add("qnn");
    add("nnapi");
    add("xnnpack");
  } else {
    if (acc == "cuda") {
      add("tensorrt");
      add("cuda");
    }
    if (acc == "rocm") add("rocm");
    if (qualcomm) add("qnn");
    if (intel) {
      if (npu.find("intel") != std::string::npos) add("openvino-npu");
      add("openvino-gpu");
    }
    if (acc == "metal") add("coreml");
#ifdef _WIN32
    if (acc != "cpu") add("dml");
#endif
    if (intel) add("openvino");
    add("xnnpack");
  }
  add("cpu");
  return v;
}

// ================================================================ tokenizador CLIP
namespace {

// bytes -> caracteres unicode imprimibles (bytes_to_unicode de GPT-2/CLIP), en UTF-8
const std::vector<std::string>& byte_unicode() {
  static const std::vector<std::string> m = [] {
    std::vector<int> bs;
    for (int b = '!'; b <= '~'; ++b) bs.push_back(b);
    for (int b = 0xA1; b <= 0xAC; ++b) bs.push_back(b);
    for (int b = 0xAE; b <= 0xFF; ++b) bs.push_back(b);
    std::vector<int> cp(256, -1);
    for (int b : bs) cp[b] = b;
    int n = 0;
    for (int b = 0; b < 256; ++b)
      if (cp[b] < 0) cp[b] = 256 + n++;
    std::vector<std::string> out(256);
    for (int b = 0; b < 256; ++b) {
      int c = cp[b];
      std::string s;
      if (c < 0x80) {
        s += (char)c;
      } else {
        s += (char)(0xC0 | (c >> 6));
        s += (char)(0x80 | (c & 0x3F));
      }
      out[b] = s;
    }
    return out;
  }();
  return m;
}

bool es_espacio(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool es_letra(unsigned char c) { return std::isalpha(c) || c >= 0x80; }   // UTF-8 no ASCII: letras (aproximado)
bool es_digito(unsigned char c) { return c >= '0' && c <= '9'; }

// trozos del texto con el patron de CLIP: especiales, contracciones, letras, digitos sueltos y signos
std::vector<std::string> trocear(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (es_espacio(c)) {
      ++i;
      continue;
    }
    bool hecho = false;
    for (const char* esp : {"<|startoftext|>", "<|endoftext|>"}) {
      size_t l = std::strlen(esp);
      if (s.compare(i, l, esp) == 0) {
        out.emplace_back(esp);
        i += l;
        hecho = true;
        break;
      }
    }
    if (hecho) continue;
    if (c == '\'') {
      for (const char* k : {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"}) {
        size_t l = std::strlen(k);
        if (s.compare(i, l, k) == 0) {
          out.emplace_back(k);
          i += l;
          hecho = true;
          break;
        }
      }
      if (hecho) continue;
    }
    size_t j = i;
    if (es_letra(c)) {
      while (j < n && es_letra((unsigned char)s[j])) ++j;
    } else if (es_digito(c)) {
      j = i + 1;
    } else {
      while (j < n && !es_espacio((unsigned char)s[j]) && !es_letra((unsigned char)s[j]) && !es_digito((unsigned char)s[j]))
        ++j;
    }
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

}  // namespace

bool TokenizadorClip::cargar(const fs::path& d) {
  json v = jparse(read_text(d / "vocab.json"));
  if (!v.is_object() || v.empty()) return false;
  vocab_.clear();
  for (auto it = v.begin(); it != v.end(); ++it)
    if (it.value().is_number_integer()) vocab_[it.key()] = it.value().get<int>();
  rangos_.clear();
  int r = 0;
  for (auto& linea : splitlines(read_text(d / "merges.txt"))) {
    if (linea.empty() || starts_with(linea, "#version")) continue;
    rangos_[linea] = r++;
  }
  if (rangos_.empty()) return false;
  auto id = [&](const std::string& t, int def) {
    auto it = vocab_.find(t);
    return it == vocab_.end() ? def : it->second;
  };
  inicio = id("<|startoftext|>", 49406);
  fin = id("<|endoftext|>", 49407);
  relleno = fin;
  // SD 2.x rellena con «!» (id 0) en vez de con el token de fin
  json esp = jparse(read_text(d / "special_tokens_map.json"));
  if (!esp.is_object()) esp = jparse(read_text(d / "tokenizer_config.json"));
  if (esp.is_object() && esp.contains("pad_token")) {
    const json& p = esp["pad_token"];
    std::string t = p.is_string() ? p.get<std::string>() : p.is_object() ? jstr(p, "content") : "";
    if (!t.empty()) relleno = id(t, fin);
  }
  cache_.clear();
  return true;
}

std::vector<std::string> TokenizadorClip::bpe(const std::string& palabra) const {
  auto c = cache_.find(palabra);
  if (c != cache_.end()) return c->second;
  const auto& bu = byte_unicode();
  std::vector<std::string> sim;
  for (unsigned char b : palabra) sim.push_back(bu[b]);
  if (sim.empty()) return sim;
  sim.back() += "</w>";
  while (sim.size() > 1) {
    int mejor = -1;
    size_t pos = 0;
    for (size_t i = 0; i + 1 < sim.size(); ++i) {
      auto it = rangos_.find(sim[i] + " " + sim[i + 1]);
      if (it != rangos_.end() && (mejor < 0 || it->second < mejor)) {
        mejor = it->second;
        pos = i;
      }
    }
    if (mejor < 0) break;
    const std::string a = sim[pos], b = sim[pos + 1];
    std::vector<std::string> nuevo;
    for (size_t i = 0; i < sim.size();) {
      if (i + 1 < sim.size() && sim[i] == a && sim[i + 1] == b) {
        nuevo.push_back(a + b);
        i += 2;
      } else {
        nuevo.push_back(sim[i++]);
      }
    }
    sim.swap(nuevo);
  }
  if (cache_.size() < 20000) cache_[palabra] = sim;
  return sim;
}

std::vector<int32_t> TokenizadorClip::codificar(const std::string& texto, int largo) const {
  // limpieza de CLIP: minusculas y espacios seguidos a uno
  std::string t;
  bool esp = false;
  for (unsigned char c : texto) {
    if (es_espacio(c)) {
      esp = !t.empty();
      continue;
    }
    if (esp) t += ' ';
    esp = false;
    t += (char)std::tolower(c);
  }
  std::vector<int32_t> ids = {inicio};
  for (auto& trozo : trocear(t)) {
    if (trozo == "<|startoftext|>" || trozo == "<|endoftext|>") continue;
    for (auto& s : bpe(trozo)) {
      auto it = vocab_.find(s);
      if (it != vocab_.end()) ids.push_back(it->second);
    }
    if ((int)ids.size() >= largo - 1) break;
  }
  if ((int)ids.size() > largo - 1) ids.resize(largo - 1);
  ids.push_back(fin);
  while ((int)ids.size() < largo) ids.push_back(relleno);
  return ids;
}

// ================================================================ planificador
Planificador Planificador::leer(const fs::path& f) {
  Planificador p;
  json j = jparse(read_text(f));
  if (!j.is_object()) return p;
  p.beta_inicio = j.value("beta_start", p.beta_inicio);
  p.beta_fin = j.value("beta_end", p.beta_fin);
  p.betas = j.value("beta_schedule", p.betas);
  p.espaciado = j.value("timestep_spacing", p.espaciado);
  p.prediccion = j.value("prediction_type", p.prediccion);
  p.pasos_entrenamiento = j.value("num_train_timesteps", p.pasos_entrenamiento);
  p.desplazamiento = j.value("steps_offset", p.desplazamiento);
  return p;
}

void Planificador::preparar(int pasos) {
  pasos = std::max(1, pasos);
  const int T = pasos_entrenamiento;
  std::vector<double> ac(T);
  double acum = 1;
  for (int i = 0; i < T; ++i) {
    double beta;
    if (betas == "linear") {
      beta = beta_inicio + (beta_fin - beta_inicio) * i / std::max(1, T - 1);
    } else {   // scaled_linear
      double a = std::sqrt(beta_inicio), b = std::sqrt(beta_fin);
      double r = a + (b - a) * i / std::max(1, T - 1);
      beta = r * r;
    }
    acum *= 1 - beta;
    ac[i] = acum;
  }
  std::vector<double> sig(T);
  for (int i = 0; i < T; ++i) sig[i] = std::sqrt((1 - ac[i]) / ac[i]);
  timesteps.assign(pasos, 0);
  if (espaciado == "trailing") {
    double r = (double)T / pasos;
    for (int i = 0; i < pasos; ++i) timesteps[i] = std::round(T - i * r) - 1;
  } else if (espaciado == "linspace") {
    for (int i = 0; i < pasos; ++i) timesteps[i] = pasos == 1 ? T - 1 : (double)(T - 1) * (pasos - 1 - i) / (pasos - 1);
  } else {   // leading
    int r = std::max(1, T / pasos);
    for (int i = 0; i < pasos; ++i) timesteps[i] = (double)(pasos - 1 - i) * r + desplazamiento;
  }
  sigmas.assign(pasos + 1, 0);
  for (int i = 0; i < pasos; ++i) {
    double t = std::clamp(timesteps[i], 0.0, (double)(T - 1));
    int a = (int)std::floor(t), b = std::min(T - 1, a + 1);
    double f = t - a;
    sigmas[i] = sig[a] * (1 - f) + sig[b] * f;   // interpolacion como np.interp
  }
  double mx = *std::max_element(sigmas.begin(), sigmas.end());
  sigma_inicial = (espaciado == "linspace" || espaciado == "trailing") ? mx : std::sqrt(mx * mx + 1);
}

// ================================================================ ONNX Runtime
namespace {

struct Ort {
  DynLib lib;
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  std::string version;
  int api_version = 0;
};

std::mutex g_ort_mu;

// una sola carga por biblioteca (nunca se descarga: los proveedores registran estado global)
Ort& cargar_ort(const fs::path& lib) {
  static std::map<std::string, std::unique_ptr<Ort>> cargadas;
  std::lock_guard<std::mutex> lk(g_ort_mu);
  auto& o = cargadas[S(lib)];
  if (o && o->api) return *o;
  o = std::make_unique<Ort>();
  if (!o->lib.open(lib)) throw std::runtime_error("no se pudo cargar ONNX Runtime: " + o->lib.error());
  const OrtApiBase*(ORT_API_CALL * base)(void) = nullptr;
  if (!o->lib.get("OrtGetApiBase", base) || !base) throw std::runtime_error("la biblioteca no es ONNX Runtime");
  const OrtApiBase* b = base();
  o->version = b->GetVersionString ? b->GetVersionString() : "";
  // la API es compatible hacia atras: la mas nueva que conozca la biblioteca (las funciones usadas son de la 16)
  for (int v = ORT_API_VERSION; v >= 16 && !o->api; --v)
    if ((o->api = b->GetApi((uint32_t)v))) o->api_version = v;
  if (!o->api) throw std::runtime_error("ONNX Runtime " + o->version + " demasiado antiguo (hace falta 1.16 o mas)");
  OrtStatus* s = o->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "agente-ia", &o->env);
  if (s) {
    std::string e = o->api->GetErrorMessage(s);
    o->api->ReleaseStatus(s);
    o->api = nullptr;
    throw std::runtime_error("ONNX Runtime: " + e);
  }
  return *o;
}

struct Comprobar {
  const OrtApi* api;
  void operator()(OrtStatus* s, const std::string& que) const {
    if (!s) return;
    std::string e = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    throw std::runtime_error(que + ": " + e);
  }
};

size_t bytes_de(ONNXTensorElementDataType t) {
  switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return 2;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return 8;
    default: return 4;
  }
}

struct Entrada {
  std::string nombre;
  ONNXTensorElementDataType tipo = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
  std::vector<int64_t> forma;   // -1 = dinamica
};

struct Sesion {
  OrtSession* s = nullptr;
  std::vector<Entrada> entradas, salidas;
  std::string proveedor;
  const Entrada* entrada(const std::string& n) const {
    for (auto& e : entradas)
      if (e.nombre == n) return &e;
    return nullptr;
  }
};

// tensor de ONNX Runtime sobre un bloque del asignador (sin copia: ORT lee y escribe en el bloque)
struct Tensor {
  hal::Buffer buf;
  OrtValue* v = nullptr;
  ONNXTensorElementDataType tipo = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
  size_t n = 0;
  // escribe floats convirtiendo al tipo del tensor
  void poner(const float* x) {
    if (tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
      auto* d = (uint16_t*)buf.data();
      for (size_t i = 0; i < n; ++i) d[i] = a_fp16(x[i]);
    } else if (tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
      auto* d = (int64_t*)buf.data();
      for (size_t i = 0; i < n; ++i) d[i] = (int64_t)std::llround(x[i]);
    } else if (tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
      auto* d = (int32_t*)buf.data();
      for (size_t i = 0; i < n; ++i) d[i] = (int32_t)std::lround(x[i]);
    } else {
      std::memcpy(buf.data(), x, n * 4);
    }
  }
  void leer(float* x) const {
    if (tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
      auto* d = (const uint16_t*)buf.data();
      for (size_t i = 0; i < n; ++i) x[i] = de_fp16(d[i]);
    } else {
      std::memcpy(x, buf.data(), n * 4);
    }
  }
};

}  // namespace

json info_ort(const fs::path& lib) {
  try {
    Ort& o = cargar_ort(lib);
    char** p = nullptr;
    int n = 0;
    json prov = json::array();
    if (!o.api->GetAvailableProviders(&p, &n)) {
      for (int i = 0; i < n; ++i) prov.push_back(p[i]);
      o.api->ReleaseAvailableProviders(p, n);
    }
    return {{"version", o.version}, {"api", o.api_version}, {"proveedores", prov}, {"lib", S(lib)}};
  } catch (const std::exception& e) {
    return {{"error", e.what()}, {"lib", S(lib)}};
  }
}

namespace {

class MotorOrt : public Motor {
 public:
  explicit MotorOrt(const Opciones& o) : op_(o), ort_(cargar_ort(o.lib)), ck_{ort_.api} {
    const OrtApi* a = ort_.api;
    if (!tok_.cargar(o.modelo / "tokenizer")) throw std::runtime_error("el modelo no trae tokenizer/vocab.json y merges.txt");
    plan_ = Planificador::leer(o.modelo / "scheduler" / "scheduler_config.json");
    json vae = jparse(read_text(o.modelo / "vae_decoder" / "config.json"));
    if (vae.is_object()) escala_vae_ = vae.value("scaling_factor", escala_vae_);
    ck_(a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), "memoria de la CPU");
    auto t0 = Reloj::now();
    texto_ = abrir(o.modelo / "text_encoder" / "model.onnx", "texto");
    unet_ = abrir(o.modelo / "unet" / "model.onnx", "unet");
    vae_ = abrir(o.modelo / "vae_decoder" / "model.onnx", "vae");
    carga_ms_ = ms_desde(t0);
    for (const char* n : {"sample", "timestep", "encoder_hidden_states"})
      if (!unet_.entrada(n))
        throw std::runtime_error(std::string("la UNet no tiene la entrada «") + n + "» (SDXL y otros aun no se admiten)");
    if (unet_.entradas.size() > 3)
      throw std::runtime_error("la UNet pide entradas de SDXL (text_embeds, time_ids): modelo aun no admitido");
    L.info("difusion ONNX: ", S(o.modelo.filename()), " en ", o.proveedor, " (ORT ", ort_.version, ", ", (int)carga_ms_,
           " ms)");
  }

  ~MotorOrt() override {
    const OrtApi* a = ort_.api;
    for (Sesion* s : {&texto_, &unet_, &vae_})
      if (s->s) a->ReleaseSession(s->s);
    if (mem_) a->ReleaseMemoryInfo(mem_);
  }

  std::string proveedor() const override { return op_.proveedor; }

  json estado() const override {
    return {{"motor", "onnxruntime"},  {"version", ort_.version},          {"proveedor", op_.proveedor},
            {"modelo", S(op_.modelo)}, {"carga_ms", (long long)carga_ms_}, {"memoria", hal::nombre(op_.memoria)},
            {"hilos", hilos()},        {"prediccion", plan_.prediccion},   {"espaciado", plan_.espaciado}};
  }

  Resultado generar(const Peticion& p) override;

 private:
  int hilos() const { return op_.hilos > 0 ? op_.hilos : hal::hilos_calculo(); }

  void proveedor_en(OrtSessionOptions* so, const std::string& etiqueta) {
    const OrtApi* a = ort_.api;
    const std::string& p = op_.proveedor;
    auto generico = [&](const char* nombre, std::vector<std::pair<std::string, std::string>> kv) {
      std::vector<const char*> k, v;
      for (auto& [x, y] : kv) k.push_back(x.c_str()), v.push_back(y.c_str());
      ck_(a->SessionOptionsAppendExecutionProvider(so, nombre, k.data(), v.data(), k.size()), std::string("proveedor ") + nombre);
    };
    fs::path cache = op_.cache.empty() ? fs::path() : op_.cache;
    if (p == "cpu") return;
    if (p == "xnnpack") {
      generico("XNNPACK", {{"intra_op_num_threads", std::to_string(hilos())}});
    } else if (p == "cuda" || p == "tensorrt") {
      if (p == "tensorrt") {
        OrtTensorRTProviderOptionsV2* t = nullptr;
        ck_(a->CreateTensorRTProviderOptions(&t), "TensorRT");
        std::string dir = S(cache / "tensorrt");
        mkdirs(cache / "tensorrt");
        const char* k[] = {"device_id", "trt_fp16_enable", "trt_engine_cache_enable", "trt_engine_cache_path", "trt_timing_cache_enable"};
        const char* v[] = {"0", "1", "1", dir.c_str(), "1"};
        OrtStatus* s = a->UpdateTensorRTProviderOptions(t, k, v, 5);
        if (!s) s = a->SessionOptionsAppendExecutionProvider_TensorRT_V2(so, t);
        a->ReleaseTensorRTProviderOptions(t);
        ck_(s, "proveedor TensorRT");
      }
      // CUDA tambien detras de TensorRT: los nodos que TensorRT no admite van a CUDA y no a la CPU
      OrtCUDAProviderOptionsV2* c = nullptr;
      ck_(a->CreateCUDAProviderOptions(&c), "CUDA");
      const char* k[] = {"device_id", "cudnn_conv_algo_search", "do_copy_in_default_stream", "cudnn_conv_use_max_workspace"};
      const char* v[] = {"0", "HEURISTIC", "1", "1"};
      OrtStatus* s = a->UpdateCUDAProviderOptions(c, k, v, 4);
      if (!s) s = a->SessionOptionsAppendExecutionProvider_CUDA_V2(so, c);
      a->ReleaseCUDAProviderOptions(c);
      ck_(s, "proveedor CUDA");
    } else if (p == "rocm") {
      OrtROCMProviderOptions r;
      ck_(a->SessionOptionsAppendExecutionProvider_ROCM(so, &r), "proveedor ROCm");
    } else if (p == "dml") {
      OrtStatus*(ORT_API_CALL * f)(OrtSessionOptions*, int) = nullptr;
      if (!ort_.lib.get("OrtSessionOptionsAppendExecutionProvider_DML", f))
        throw std::runtime_error("esta ONNX Runtime no trae DirectML");
      ck_(a->DisableMemPattern(so), "DirectML");
      ck_(a->SetSessionExecutionMode(so, ORT_SEQUENTIAL), "DirectML");
      ck_(f(so, 0), "proveedor DirectML");
    } else if (p == "qnn") {
#ifdef _WIN32
      std::string backend = "QnnHtp.dll";
#else
      std::string backend = "libQnnHtp.so";
#endif
      // el grafo compilado para la NPU se guarda (contexto EP): la segunda carga no recompila (minutos -> segundos)
      if (!cache.empty()) {
        mkdirs(cache / "qnn");
        ck_(a->AddSessionConfigEntry(so, "ep.context_enable", "1"), "QNN");
        ck_(a->AddSessionConfigEntry(so, "ep.context_file_path", S(cache / "qnn" / (etiqueta + "_ctx.onnx")).c_str()), "QNN");
      }
      generico("QNN", {{"backend_path", backend},
                       {"htp_performance_mode", "burst"},
                       {"htp_graph_finalization_optimization_mode", "3"},
                       {"enable_htp_fp16_precision", "1"}});
    } else if (starts_with(p, "openvino")) {
      std::string dev = p == "openvino-npu" ? "NPU" : p == "openvino-gpu" ? "GPU" : "CPU";
      std::vector<std::pair<std::string, std::string>> kv = {{"device_type", dev}};
      if (!cache.empty()) {
        mkdirs(cache / "openvino");
        kv.push_back({"cache_dir", S(cache / "openvino")});
      }
      generico("OpenVINO", kv);
    } else if (p == "nnapi") {
      OrtStatus*(ORT_API_CALL * f)(OrtSessionOptions*, uint32_t) = nullptr;
      if (!ort_.lib.get("OrtSessionOptionsAppendExecutionProvider_Nnapi", f))
        throw std::runtime_error("esta ONNX Runtime no trae NNAPI");
      ck_(f(so, 0x001 /* NNAPI_FLAG_USE_FP16 */), "proveedor NNAPI");
    } else if (p == "coreml") {
      generico("CoreML", {{"ModelFormat", "MLProgram"}, {"MLComputeUnits", "ALL"}});
    } else if (p == "webgpu") {
      generico("WebGPU", {});
    } else {
      throw std::runtime_error("proveedor desconocido: " + p);
    }
  }

  Sesion abrir(const fs::path& f, const std::string& etiqueta) {
    const OrtApi* a = ort_.api;
    OrtSessionOptions* so = nullptr;
    ck_(a->CreateSessionOptions(&so), "opciones de sesion");
    struct Fin {
      const OrtApi* a;
      OrtSessionOptions* so;
      ~Fin() { a->ReleaseSessionOptions(so); }
    } fin{a, so};
    ck_(a->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "optimizacion");
    ck_(a->SetIntraOpNumThreads(so, hilos()), "hilos");
    ck_(a->SetInterOpNumThreads(so, 1), "hilos");
    // los hilos de ONNX Runtime esperan activos entre pasos (cada paso de la UNet empieza enseguida)
    a->AddSessionConfigEntry(so, "session.intra_op.allow_spinning", "1");
    proveedor_en(so, S(op_.modelo.filename()) + "_" + etiqueta);
    Sesion s;
    s.proveedor = op_.proveedor;
    fs::path modelo = f;
    // QNN: si ya hay contexto compilado, se carga ese
    if (op_.proveedor == "qnn" && !op_.cache.empty()) {
      fs::path ctx = op_.cache / "qnn" / (S(op_.modelo.filename()) + "_" + etiqueta + "_ctx.onnx");
      if (is_file(ctx)) modelo = ctx;
    }
#ifdef _WIN32
    ck_(a->CreateSession(ort_.env, modelo.wstring().c_str(), so, &s.s), "cargar " + S(f));
#else
    ck_(a->CreateSession(ort_.env, modelo.c_str(), so, &s.s), "cargar " + S(f));
#endif
    OrtAllocator* al = nullptr;
    ck_(a->GetAllocatorWithDefaultOptions(&al), "asignador");
    auto info = [&](bool salida, size_t i) {
      Entrada e;
      char* nombre = nullptr;
      ck_(salida ? a->SessionGetOutputName(s.s, i, al, &nombre) : a->SessionGetInputName(s.s, i, al, &nombre), "nombres");
      e.nombre = nombre;
      a->AllocatorFree(al, nombre);
      OrtTypeInfo* ti = nullptr;
      ck_(salida ? a->SessionGetOutputTypeInfo(s.s, i, &ti) : a->SessionGetInputTypeInfo(s.s, i, &ti), "tipos");
      const OrtTensorTypeAndShapeInfo* tt = nullptr;
      if (!a->CastTypeInfoToTensorInfo(ti, &tt) && tt) {
        a->GetTensorElementType(tt, &e.tipo);
        size_t n = 0;
        a->GetDimensionsCount(tt, &n);
        e.forma.assign(n, -1);
        if (n) a->GetDimensions(tt, e.forma.data(), n);
      }
      a->ReleaseTypeInfo(ti);
      return e;
    };
    size_t ni = 0, no = 0;
    ck_(a->SessionGetInputCount(s.s, &ni), "entradas");
    ck_(a->SessionGetOutputCount(s.s, &no), "salidas");
    for (size_t i = 0; i < ni; ++i) s.entradas.push_back(info(false, i));
    for (size_t i = 0; i < no; ++i) s.salidas.push_back(info(true, i));
    return s;
  }

  // tensor nuevo en un bloque del asignador (memoria del acelerador si la hay)
  Tensor tensor(ONNXTensorElementDataType tipo, const std::vector<int64_t>& forma) {
    Tensor t;
    t.tipo = tipo;
    t.n = 1;
    for (auto d : forma) t.n *= (size_t)std::max<int64_t>(1, d);
    t.buf = hal::reservar(std::max<size_t>(1, t.n * bytes_de(tipo)), op_.memoria);
    std::memset(t.buf.data(), 0, t.buf.size());
    ck_(ort_.api->CreateTensorWithDataAsOrtValue(mem_, t.buf.data(), t.n * bytes_de(tipo), forma.data(), forma.size(),
                                                 tipo, &t.v),
        "tensor");
    return t;
  }
  void soltar(Tensor& t) {
    if (t.v) ort_.api->ReleaseValue(t.v);
    t.v = nullptr;
  }

  // codificador de texto: [1, 77, D] en float
  std::vector<float> codificar(const std::string& texto, int64_t& d) {
    const OrtApi* a = ort_.api;
    const Entrada& e = texto_.entradas.at(0);
    auto ids = tok_.codificar(texto, 77);
    std::vector<float> f(ids.begin(), ids.end());
    Tensor in = tensor(e.tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ? ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64
                                                                     : ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32,
                       {1, 77});
    in.poner(f.data());
    const char* in_n[] = {e.nombre.c_str()};
    const char* out_n[] = {texto_.salidas.at(0).nombre.c_str()};
    OrtValue* out = nullptr;
    OrtStatus* s = a->Run(texto_.s, nullptr, in_n, &in.v, 1, out_n, 1, &out);
    soltar(in);
    ck_(s, "codificador de texto");
    OrtTensorTypeAndShapeInfo* ti = nullptr;
    ck_(a->GetTensorTypeAndShape(out, &ti), "forma");
    size_t nd = 0;
    a->GetDimensionsCount(ti, &nd);
    std::vector<int64_t> forma(nd);
    a->GetDimensions(ti, forma.data(), nd);
    ONNXTensorElementDataType tipo;
    a->GetTensorElementType(ti, &tipo);
    a->ReleaseTensorTypeAndShapeInfo(ti);
    d = nd == 3 ? forma[2] : 0;
    size_t n = 1;
    for (auto x : forma) n *= (size_t)x;
    void* p = nullptr;
    ck_(a->GetTensorMutableData(out, &p), "datos");
    std::vector<float> r(n);
    if (tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
      for (size_t i = 0; i < n; ++i) r[i] = de_fp16(((uint16_t*)p)[i]);
    else
      std::memcpy(r.data(), p, n * 4);
    a->ReleaseValue(out);
    if (nd != 3 || forma[1] != 77) throw std::runtime_error("el codificador de texto no devuelve [1, 77, D]");
    return r;
  }

  Opciones op_;
  Ort& ort_;
  Comprobar ck_;
  OrtMemoryInfo* mem_ = nullptr;
  TokenizadorClip tok_;
  Planificador plan_;
  double escala_vae_ = 0.18215, carga_ms_ = 0;
  Sesion texto_, unet_, vae_;
  std::mutex mu_;   // una imagen cada vez por motor
};

Resultado MotorOrt::generar(const Peticion& p) {
  std::lock_guard<std::mutex> lk(mu_);
  const OrtApi* a = ort_.api;
  Resultado res;
  res.proveedor = op_.proveedor;
  auto t0 = Reloj::now();

  // tamano: el que pide la UNet si lo tiene fijo (modelos compilados para la NPU), si no el pedido (multiplo de 8)
  const Entrada& es = *unet_.entrada("sample");
  int h = std::max(8, p.alto / 8), w = std::max(8, p.ancho / 8);
  if (es.forma.size() == 4 && es.forma[2] > 0) h = (int)es.forma[2];
  if (es.forma.size() == 4 && es.forma[3] > 0) w = (int)es.forma[3];
  const bool cfg = p.guia > 1.0;
  const bool lote_fijo = es.forma.size() == 4 && es.forma[0] == 1;
  const int B = cfg && !lote_fijo ? 2 : 1;

  // texto: condicionado (y sin condicionar para la guia)
  int64_t d = 0;
  std::vector<float> cond = codificar(p.prompt, d), incond;
  if (cfg) incond = codificar(p.negativo, d);
  res.tiempos["texto"] = (long long)ms_desde(t0);

  // tensores de la UNet, una vez por imagen
  const size_t nlat = (size_t)4 * h * w;
  const Entrada& et = *unet_.entrada("timestep");
  const Entrada& ee = *unet_.entrada("encoder_hidden_states");
  Tensor t_sample = tensor(es.tipo, {B, 4, h, w});
  std::vector<int64_t> forma_t;
  if (!et.forma.empty()) forma_t.assign(1, et.forma[0] > 0 ? et.forma[0] : B);
  Tensor t_t = tensor(et.tipo, forma_t);
  Tensor t_ehs = tensor(ee.tipo, {B, 77, d});
  Tensor t_out = tensor(unet_.salidas.at(0).tipo, {B, 4, h, w});
  struct Fin {
    MotorOrt* m;
    std::vector<Tensor*> t;
    ~Fin() {
      for (auto* x : t) m->soltar(*x);
    }
  } fin{this, {&t_sample, &t_t, &t_ehs, &t_out}};
  // con lote 2 van [sin condicionar, condicionado]; con lote fijo 1 se pasa cada uno por separado
  std::vector<float> ehs;
  if (B == 2) {
    ehs = incond;
    ehs.insert(ehs.end(), cond.begin(), cond.end());
  } else {
    ehs = cond;
  }
  t_ehs.poner(ehs.data());

  // ruido inicial
  plan_.preparar(p.pasos);
  std::mt19937_64 rng(p.semilla);
  std::normal_distribution<float> normal(0.f, 1.f);
  std::vector<float> lat(nlat), entrada((size_t)B * nlat), eps((size_t)B * nlat), x0(nlat);
  for (auto& v : lat) v = normal(rng) * (float)plan_.sigma_inicial;

  const char* in_n[] = {"sample", "timestep", "encoder_hidden_states"};
  const char* out_n[] = {unet_.salidas.at(0).nombre.c_str()};
  const int total = (int)plan_.timesteps.size();
  auto t1 = Reloj::now();
  auto correr = [&]() {
    OrtValue* in_v[] = {t_sample.v, t_t.v, t_ehs.v};
    ck_(a->Run(unet_.s, nullptr, in_n, in_v, 3, out_n, 1, &t_out.v), "UNet");   // salida preasignada
  };
  for (int i = 0; i < total; ++i) {
    if (p.cancelar && p.cancelar->load()) throw std::runtime_error("cancelado");
    const double sigma = plan_.sigmas[i], sig_sig = plan_.sigmas[i + 1];
    const float escala = (float)(1.0 / std::sqrt(sigma * sigma + 1));
    for (int b = 0; b < B; ++b)
      for (size_t k = 0; k < nlat; ++k) entrada[b * nlat + k] = lat[k] * escala;
    std::vector<float> tv(t_t.n, (float)plan_.timesteps[i]);
    t_t.poner(tv.data());
    t_sample.poner(entrada.data());
    if (cfg && B == 1) {
      // lote fijo 1: sin condicionar y condicionado en dos pasadas
      std::vector<float> e_u(nlat);
      t_ehs.poner(incond.data());
      correr();
      t_out.leer(e_u.data());
      t_ehs.poner(cond.data());
      correr();
      t_out.leer(eps.data());
      for (size_t k = 0; k < nlat; ++k) eps[k] = e_u[k] + (float)p.guia * (eps[k] - e_u[k]);
    } else {
      correr();
      t_out.leer(eps.data());
      if (B == 2)
        for (size_t k = 0; k < nlat; ++k) eps[k] = eps[k] + (float)p.guia * (eps[nlat + k] - eps[k]);
    }
    // Euler: x0 estimado y paso hasta el sigma siguiente
    for (size_t k = 0; k < nlat; ++k) {
      float x = lat[k], e = eps[k];
      float s = (float)sigma;
      x0[k] = plan_.prediccion == "v_prediction" ? e * (-s / std::sqrt(s * s + 1)) + x / (s * s + 1) : x - s * e;
      float deriv = (x - x0[k]) / s;
      lat[k] = x + deriv * (float)(sig_sig - sigma);
    }
    // vista previa del paso al bufer de pantalla (el final lo pinta el VAE)
    if (p.vista && i + 1 < total) {
      auto rgb = latente_a_rgb(x0.data(), h, w);
      media::Canvas c(w, h);
      c.px = std::move(rgb);
      std::string png = c.png();
      hal::Buffer b = hal::pool_imagenes().pedir(png.size());
      std::memcpy(b.data(), png.data(), png.size());
      p.vista(b, i + 1, total);
    }
  }
  double unet_ms = ms_desde(t1);
  res.tiempos["unet"] = (long long)unet_ms;
  res.tiempos["por_paso"] = (long long)(unet_ms / std::max(1, total));

  // VAE: latente / escala -> imagen [-1, 1]
  auto t2 = Reloj::now();
  const Entrada& ev = vae_.entradas.at(0);
  Tensor t_lat = tensor(ev.tipo, {1, 4, h, w});
  for (auto& v : lat) v /= (float)escala_vae_;
  t_lat.poner(lat.data());
  const char* vin[] = {ev.nombre.c_str()};
  const char* vout[] = {vae_.salidas.at(0).nombre.c_str()};
  OrtValue* img = nullptr;
  OrtStatus* s = a->Run(vae_.s, nullptr, vin, &t_lat.v, 1, vout, 1, &img);
  soltar(t_lat);
  ck_(s, "VAE");
  OrtTensorTypeAndShapeInfo* ti = nullptr;
  ck_(a->GetTensorTypeAndShape(img, &ti), "forma");
  size_t nd = 0;
  a->GetDimensionsCount(ti, &nd);
  std::vector<int64_t> forma(nd);
  a->GetDimensions(ti, forma.data(), nd);
  ONNXTensorElementDataType tipo;
  a->GetTensorElementType(ti, &tipo);
  a->ReleaseTensorTypeAndShapeInfo(ti);
  if (nd != 4 || forma[1] != 3) {
    a->ReleaseValue(img);
    throw std::runtime_error("el VAE no devuelve [1, 3, H, W]");
  }
  const int H = (int)forma[2], W = (int)forma[3];
  void* dp = nullptr;
  ck_(a->GetTensorMutableData(img, &dp), "datos");
  const size_t np = (size_t)H * W;
  res.rgb.resize(np * 3);
  for (int c = 0; c < 3; ++c)
    for (size_t i = 0; i < np; ++i) {
      float v = tipo == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ? de_fp16(((uint16_t*)dp)[c * np + i]) : ((float*)dp)[c * np + i];
      res.rgb[i * 3 + c] = (uint8_t)std::clamp((v + 1.f) * 127.5f + 0.5f, 0.f, 255.f);
    }
  a->ReleaseValue(img);
  res.tiempos["vae"] = (long long)ms_desde(t2);
  res.ancho = W;
  res.alto = H;
  res.pasos = total;
  media::Canvas c(W, H);
  c.px = res.rgb;
  std::string png = c.png();
  res.png = hal::pool_imagenes().pedir(png.size());
  std::memcpy(res.png.data(), png.data(), png.size());
  res.tiempos["total"] = (long long)ms_desde(t0);
  return res;
}

}  // namespace

std::unique_ptr<Motor> Motor::abrir(const Opciones& o) {
  if (!es_modelo(o.modelo)) throw std::runtime_error("no es un modelo ONNX de difusion: " + S(o.modelo));
  if (!is_file(o.lib)) throw std::runtime_error("no hay biblioteca de ONNX Runtime");
  return std::make_unique<MotorOrt>(o);
}

}  // namespace nx::difusion
