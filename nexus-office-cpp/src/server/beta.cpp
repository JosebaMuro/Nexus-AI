// Version beta: tiempos de la API, banco de pruebas y opiniones sobre el aspecto (ver beta.h).
#include "server/beta.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/services.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "server/app.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/text.h"

namespace nx::beta {

using server::Ctx;
using server::HttpErr;
using server::Out;
using server::Raw;
using Clock = std::chrono::steady_clock;

namespace {

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
double r2(double x) { return std::round(x * 100.0) / 100.0; }

fs::path beta_dir() { return paths::sub({"beta"}); }

// ------------------------------------------------------------------ tiempos de la API
constexpr size_t VENTANA = 512;   // ultimas duraciones por ruta para los percentiles

struct Ruta {
  std::deque<double> ms;
  long long n = 0, errores = 0;
  double total = 0, maximo = 0;
};

struct Http {
  std::mutex mu;
  std::map<std::string, Ruta> rutas;
  double desde = now();
};
Http& H() {
  static Http h;
  return h;
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double idx = p * (double)(v.size() - 1);
  size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
  return v[lo] + (v[hi] - v[lo]) * (idx - (double)lo);
}

json read_jsonl(const fs::path& p, size_t n) {
  json out = json::array();
  auto t = read_file(p);
  if (!t) return out;
  auto lines = splitlines(*t);
  size_t from = lines.size() > n ? lines.size() - n : 0;
  for (size_t i = from; i < lines.size(); ++i) {
    json j = jparse(lines[i]);
    if (!j.is_discarded() && j.is_object()) out.push_back(std::move(j));
  }
  return out;
}

// ------------------------------------------------------------------ banco de pruebas del sistema
// trabajo entero sin memoria (xorshift + mezcla): mide la CPU, no la cache
uint64_t cpu_work(uint64_t iters, uint64_t seed) {
  uint64_t x = seed | 1, acc = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    acc += (x * 0x9E3779B97F4A7C15ULL) >> 29;
  }
  return acc;
}

json bench_cpu() {
  const uint64_t ITERS = 60'000'000;
  auto t0 = Clock::now();
  volatile uint64_t sink = cpu_work(ITERS, 42);
  (void)sink;
  double s1 = ms_since(t0) / 1000.0;
  double mops1 = ITERS / s1 / 1e6;

  unsigned n = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> th;
  std::atomic<uint64_t> total{0};
  t0 = Clock::now();
  for (unsigned i = 0; i < n; ++i) th.emplace_back([&, i] { total += cpu_work(ITERS, 42 + i); });
  for (auto& t : th) t.join();
  double sn = ms_since(t0) / 1000.0;
  double mopsn = ITERS * (double)n / sn / 1e6;
  return {{"hilos", n},
          {"mops_1_hilo", r2(mops1)},
          {"mops_todos", r2(mopsn)},
          {"escalado", r2(mopsn / std::max(1e-9, mops1))},
          {"eficiencia_pct", r2(100.0 * mopsn / std::max(1e-9, mops1) / n)}};
}

json bench_memoria() {
  const size_t MB = 64, N = MB << 20;
  std::vector<char> a(N, 1), b(N, 0);
  const int REPS = 8;
  auto t0 = Clock::now();
  for (int i = 0; i < REPS; ++i) {
    std::memcpy(b.data(), a.data(), N);
    a[(size_t)i * 4096 % N] = (char)b[(size_t)i * 8192 % N];   // que el compilador no se salte las copias
  }
  double s = ms_since(t0) / 1000.0;
  return {{"bloque_mb", MB}, {"copia_gbs", r2((double)N * REPS / s / 1e9)}};
}

json bench_disco() {
  const size_t MB = 64, N = MB << 20;
  fs::path f = beta_dir() / "bench-disco.tmp";
  std::vector<char> buf(1 << 20);
  for (size_t i = 0; i < buf.size(); ++i) buf[i] = (char)(i * 31 + 7);
  json r = {{"bloque_mb", MB}};
  auto t0 = Clock::now();
  {
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    if (!o) return {{"error", "no se puede escribir en " + S(beta_dir())}};
    for (size_t w = 0; w < N; w += buf.size()) o.write(buf.data(), (std::streamsize)buf.size());
    o.flush();
  }
  r["escritura_mbs"] = r2((double)MB / (ms_since(t0) / 1000.0));
  t0 = Clock::now();
  {
    std::ifstream in(f, std::ios::binary);
    size_t got = 0;
    while (in.read(buf.data(), (std::streamsize)buf.size()) || in.gcount() > 0) got += (size_t)in.gcount();
    r["lectura_mbs"] = r2((double)got / (1 << 20) / (ms_since(t0) / 1000.0));
  }
  r["nota"] = "la lectura puede venir de la cache del sistema";
  std::error_code ec;
  fs::remove(f, ec);
  return r;
}

// ------------------------------------------------------------------ banco de pruebas del LLM
const std::vector<std::pair<std::string, std::string>> PROMPTS = {
    {"corta", "Responde en una sola frase: ¿qué es una factura proforma?"},
    {"lista", "Enumera cinco tareas administrativas que un agente de IA puede hacer en una pyme. Solo la lista."},
    {"redaccion", "Redacta un correo breve y cordial a un cliente recordándole que su factura vence el viernes."},
};

json bench_llm(Services& s, int rondas, int max_tokens) {
  json r = json::object();
  if (!s.llm || !s.llm->has_real_llm()) {
    r["omitido"] = "No hay un modelo de IA real activo (instala o activa uno en Modelos)";
    return r;
  }
  json st = s.llm->status();
  r["backend"] = jstr(st, "backend");
  r["modelo"] = jstr(st, "model");
  json filas = json::array();
  std::vector<double> ttfts, tps;
  double tot_tok = 0, tot_s = 0;
  for (int i = 0; i < rondas; ++i) {
    auto& [tipo, prompt] = PROMPTS[(size_t)i % PROMPTS.size()];
    json msgs = json::array({{{"role", "user"}, {"content", prompt}}});
    llm::Opts o;
    o.o = {{"max_tokens", max_tokens}, {"temperature", 0.2}, {"reasoning", "off"}};
    auto t0 = Clock::now();
    double first = -1;
    long piezas = 0;
    size_t chars = 0;
    json fila = {{"ronda", i + 1}, {"tipo", tipo}};
    try {
      s.llm->stream(msgs, "default", o, [&](const std::string& piece) {
        if (piece.empty()) return;
        if (first < 0) first = ms_since(t0);
        ++piezas;
        chars += piece.size();
      });
    } catch (const std::exception& e) {
      fila["error"] = e.what();
      filas.push_back(fila);
      continue;
    }
    double total = ms_since(t0);
    // los tokens reales los cuenta el backend; si no los da, cada trozo del stream es un token
    long tokens = piezas;
    if (auto b = s.llm->backend(s.llm->last_used.first); b && b->last_stats.tokens > 0) tokens = b->last_stats.tokens;
    double gen_s = std::max(1e-3, (total - std::max(0.0, first)) / 1000.0);
    double tok_s = tokens > 1 ? (double)(tokens - 1) / gen_s : 0;
    fila["ttft_ms"] = r2(std::max(0.0, first));
    fila["total_ms"] = r2(total);
    fila["tokens"] = tokens;
    fila["caracteres"] = chars;
    fila["tok_s"] = r2(tok_s);
    filas.push_back(fila);
    if (first >= 0) ttfts.push_back(first);
    if (tok_s > 0) tps.push_back(tok_s);
    tot_tok += (double)tokens;
    tot_s += total / 1000.0;
  }
  r["rondas"] = filas;
  r["ttft_ms_p50"] = r2(pct(ttfts, 0.5));
  r["ttft_ms_max"] = r2(ttfts.empty() ? 0 : *std::max_element(ttfts.begin(), ttfts.end()));
  r["tok_s_media"] = r2(tps.empty() ? 0 : [&] {
    double a = 0;
    for (double x : tps) a += x;
    return a / (double)tps.size();
  }());
  r["tok_s_global"] = r2(tot_s > 0 ? tot_tok / tot_s : 0);
  return r;
}

}  // namespace

// ------------------------------------------------------------------ tiempos de la API
void record(const std::string& method, const std::string& pattern, double ms, int status) {
  auto& h = H();
  std::lock_guard<std::mutex> lk(h.mu);
  auto& r = h.rutas[method + " " + pattern];
  ++r.n;
  if (status >= 500) ++r.errores;
  r.total += ms;
  r.maximo = std::max(r.maximo, ms);
  r.ms.push_back(ms);
  if (r.ms.size() > VENTANA) r.ms.pop_front();
}

json http_stats() {
  auto& h = H();
  std::lock_guard<std::mutex> lk(h.mu);
  json rutas = json::array();
  std::vector<double> todas;
  long long n = 0, err = 0;
  for (auto& [k, r] : h.rutas) {
    std::vector<double> v(r.ms.begin(), r.ms.end());
    todas.insert(todas.end(), v.begin(), v.end());
    n += r.n;
    err += r.errores;
    rutas.push_back({{"ruta", k},
                     {"n", r.n},
                     {"errores", r.errores},
                     {"media_ms", r2(r.total / (double)std::max(1LL, r.n))},
                     {"p50_ms", r2(pct(v, 0.5))},
                     {"p95_ms", r2(pct(v, 0.95))},
                     {"max_ms", r2(r.maximo)}});
  }
  std::sort(rutas.begin(), rutas.end(), [](const json& a, const json& b) { return a["p95_ms"] > b["p95_ms"]; });
  return {{"desde", h.desde},
          {"rutas", rutas},
          {"total", {{"n", n}, {"errores", err}, {"p50_ms", r2(pct(todas, 0.5))}, {"p95_ms", r2(pct(todas, 0.95))}}}};
}

void reset_http() {
  auto& h = H();
  std::lock_guard<std::mutex> lk(h.mu);
  h.rutas.clear();
  h.desde = now();
}

// ------------------------------------------------------------------ banco de pruebas
BenchOpts bench_opts(const json& body) {
  BenchOpts o;
  if (!body.is_object()) return o;
  o.rondas = std::clamp((int)jnum(body.value("rondas", json(3)), 3), 1, 10);
  o.max_tokens = std::clamp((int)jnum(body.value("max_tokens", json(128)), 128), 16, 512);
  o.llm = body.value("llm", json(true)).is_boolean() ? body.value("llm", true) : true;
  o.sistema = body.value("sistema", json(true)).is_boolean() ? body.value("sistema", true) : true;
  if (body.contains("cliente") && body["cliente"].is_object()) o.cliente = body["cliente"];
  return o;
}

json run_bench(Services& s, const BenchOpts& o) {
  static std::mutex running;
  std::unique_lock<std::mutex> lk(running, std::try_to_lock);
  if (!lk.owns_lock()) throw std::runtime_error("Ya hay una medicion en marcha; espera a que termine");
  auto t0 = Clock::now();
  json r = {{"id", token_hex(6)},
            {"ts", now()},
            {"version", NEXUS_VERSION},
            {"canal", CANAL},
            {"equipo",
             {{"cpu", s.hw.cpu_name},
              {"nucleos", s.hw.cores_logical},
              {"ram_gb", r2(s.hw.ram_gb)},
              {"so", s.hw.os + " " + s.hw.os_version},
              {"arch", s.hw.arch},
              {"acelerador", jstr(s.profile, "accelerator")},
              {"gpu", s.hw.gpus.empty() ? std::string() : s.hw.gpus[0].name}}},
            {"opciones", {{"rondas", o.rondas}, {"max_tokens", o.max_tokens}, {"llm", o.llm}, {"sistema", o.sistema}}}};
  if (o.sistema) {
    r["cpu"] = bench_cpu();
    r["memoria"] = bench_memoria();
    r["disco"] = bench_disco();
  }
  if (o.llm) r["llm"] = bench_llm(s, o.rondas, o.max_tokens);
  r["api"] = http_stats()["total"];
  if (!o.cliente.is_null()) r["cliente"] = o.cliente;
  r["duracion_s"] = r2(ms_since(t0) / 1000.0);
  append_file(beta_dir() / "bench.jsonl", jdump(r) + "\n");
  return r;
}

json bench_history(size_t n) { return read_jsonl(beta_dir() / "bench.jsonl", n); }

// ------------------------------------------------------------------ opiniones sobre el aspecto
json save_opinion(const json& body) {
  static const std::vector<std::string> ZONAS = {"general", "chat", "salon", "nucleo", "estudio", "bolsa", "agentes",
                                                 "modelos", "herramientas", "sistema", "ajustes", "movil", "rendimiento"};
  if (!body.is_object()) throw std::invalid_argument("Se esperaba un objeto JSON");
  std::string zona = jstr(body, "zona", "general");
  if (std::find(ZONAS.begin(), ZONAS.end(), zona) == ZONAS.end())
    throw std::invalid_argument("zona no valida. Validas: " + join(ZONAS, ", "));
  int nota = (int)jnum(body.value("nota", json(0)));
  if (nota < 1 || nota > 5) throw std::invalid_argument("la nota va de 1 a 5");
  std::string texto = jstr(body, "texto");
  if (texto.size() > 4000) texto = texto.substr(0, 4000);
  json o = {{"id", token_hex(6)}, {"ts", now()},   {"version", NEXUS_VERSION}, {"zona", zona},
            {"nota", nota},        {"texto", texto}, {"tema", jstr(body, "tema")}, {"disposicion", jstr(body, "disposicion")},
            {"efectos", jstr(body, "efectos")}};
  for (const char* k : {"pantalla", "vitals"})
    if (body.contains(k) && body[k].is_object()) o[k] = body[k];
  append_file(beta_dir() / "opiniones.jsonl", jdump(o) + "\n");
  return o;
}

json opinions(size_t n) { return read_jsonl(beta_dir() / "opiniones.jsonl", n); }

// ------------------------------------------------------------------ informacion
json info(Services& s) {
  std::string compilador =
#if defined(__clang__)
      "Clang " __clang_version__;
#elif defined(__GNUC__)
      "GCC " __VERSION__;
#elif defined(_MSC_VER)
      "MSVC " + std::to_string(_MSC_VER);
#else
      "desconocido";
#endif
  json st = s.llm ? s.llm->status() : json::object();
  return {{"version", NEXUS_VERSION},
          {"canal", CANAL},
          {"compilado", std::string(__DATE__) + " " + __TIME__},
          {"compilador", compilador},
#ifdef NDEBUG
          {"optimizado", true},
#else
          {"optimizado", false},
#endif
          {"llm", {{"backend", jstr(st, "backend")}, {"modelo", jstr(st, "model")}}},
          {"acelerador", jstr(s.profile, "accelerator")},
          {"mediciones", bench_history(1000).size()},
          {"opiniones", opinions(100000).size()}};
}

std::string report_markdown(Services& s) {
  json in = info(s), hist = bench_history(5), http = http_stats(), ops = opinions(200);
  std::string md = "# Informe de la beta de Nexus-Office " + jstr(in, "version") + "\n\n";
  md += "- Compilado: " + jstr(in, "compilado") + " con " + jstr(in, "compilador") + "\n";
  md += "- Equipo: " + s.hw.cpu_name + ", " + std::to_string(s.hw.cores_logical) + " hilos, " +
        std::to_string((int)std::round(s.hw.ram_gb)) + " GB de RAM, " + jstr(s.profile, "accelerator") + "\n";
  md += "- IA: " + jstr(in["llm"], "backend") + " · " + jstr(in["llm"], "modelo") + "\n\n";
  md += "## Mediciones (últimas " + std::to_string(hist.size()) + ")\n\n";
  md += "| Fecha | CPU 1 hilo (Mops/s) | CPU todos | Memoria (GB/s) | Disco escr. (MB/s) | TTFT p50 (ms) | tok/s |\n";
  md += "|---|---|---|---|---|---|---|\n";
  auto num = [](const json& j, const char* a, const char* b) -> std::string {
    if (!j.contains(a) || !j[a].is_object() || !j[a].contains(b)) return "—";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", jnum(j[a][b]));
    return buf;
  };
  for (auto& h : hist) {
    std::time_t t = (std::time_t)jnum(h["ts"]);
    char fecha[32];
    std::strftime(fecha, sizeof fecha, "%Y-%m-%d %H:%M", std::localtime(&t));
    md += std::string("| ") + fecha + " | " + num(h, "cpu", "mops_1_hilo") + " | " + num(h, "cpu", "mops_todos") + " | " +
          num(h, "memoria", "copia_gbs") + " | " + num(h, "disco", "escritura_mbs") + " | " + num(h, "llm", "ttft_ms_p50") +
          " | " + num(h, "llm", "tok_s_global") + " |\n";
  }
  md += "\n## API (rutas más lentas por p95)\n\n| Ruta | Peticiones | p50 (ms) | p95 (ms) | Máx. (ms) |\n|---|---|---|---|---|\n";
  size_t k = 0;
  for (auto& r : http["rutas"]) {
    if (k++ >= 10) break;
    char fila[512];
    std::snprintf(fila, sizeof fila, "| `%s` | %lld | %.1f | %.1f | %.1f |\n", jstr(r, "ruta").c_str(),
                  (long long)jnum(r["n"]), jnum(r["p50_ms"]), jnum(r["p95_ms"]), jnum(r["max_ms"]));
    md += fila;
  }
  md += "\n## Opiniones sobre el aspecto\n\n";
  std::map<std::string, std::pair<double, int>> por_zona;
  for (auto& o : ops) {
    auto& z = por_zona[jstr(o, "zona")];
    z.first += jnum(o["nota"]);
    ++z.second;
  }
  if (por_zona.empty()) md += "Todavía no hay opiniones.\n";
  else {
    md += "| Zona | Opiniones | Nota media |\n|---|---|---|\n";
    for (auto& [z, v] : por_zona) {
      char fila[128];
      std::snprintf(fila, sizeof fila, "| %s | %d | %.1f |\n", z.c_str(), v.second, v.first / v.second);
      md += fila;
    }
    md += "\n";
    for (auto& o : ops)
      if (!jstr(o, "texto").empty())
        md += "- **" + jstr(o, "zona") + "** (" + std::to_string((int)jnum(o["nota"])) + "/5, tema " + jstr(o, "tema") + "): " +
              jstr(o, "texto") + "\n";
  }
  return md;
}

// ------------------------------------------------------------------ rutas
void register_routes(server::App& app) {
  app.get("/api/beta", [](Ctx& c) -> Out { return info(c.app.S()); });

  app.get("/api/beta/http", [](Ctx&) -> Out { return http_stats(); });
  app.del("/api/beta/http", [](Ctx&) -> Out {
    reset_http();
    return json{{"ok", true}};
  }, true);

  app.get("/api/beta/bench", [](Ctx& c) -> Out { return json{{"mediciones", bench_history((size_t)std::clamp(c.qi("n", 50), 1LL, 1000LL))}}; });
  app.post("/api/beta/bench", [](Ctx& c) -> Out {
    try {
      return run_bench(c.app.S(), bench_opts(c.body));
    } catch (const std::runtime_error& e) {
      throw HttpErr(409, e.what());
    }
  });

  app.post("/api/beta/opinion", [](Ctx& c) -> Out {
    try {
      return save_opinion(c.body);
    } catch (const std::invalid_argument& e) {
      throw HttpErr(400, e.what());
    }
  });
  app.get("/api/beta/opiniones", [](Ctx& c) -> Out { return json{{"opiniones", opinions((size_t)std::clamp(c.qi("n", 500), 1LL, 100000LL))}}; },
          true);

  // informe en Markdown con equipo, mediciones, API y opiniones (para adjuntarlo a una incidencia)
  app.get("/api/beta/informe", [](Ctx& c) -> Out {
    Raw r;
    r.body = report_markdown(c.app.S());
    r.mime = "text/markdown; charset=utf-8";
    r.headers.push_back({"Content-Disposition", "attachment; filename=\"nexus-office-beta-informe.md\""});
    return r;
  });
}

}  // namespace nx::beta
