// Version beta: tiempos de la API (percentiles), opciones del banco de pruebas, opiniones y medicion del sistema.
#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "server/beta.h"
#include "util/fs.h"

using namespace nx;

TEST("beta: tiempos de la API por ruta con percentiles y puesta a cero") {
  beta::reset_http();
  for (int i = 1; i <= 100; ++i) beta::record("GET", "/api/x/{id}", (double)i, i == 100 ? 500 : 200);
  beta::record("POST", "/api/y", 3.0, 200);
  json h = beta::http_stats();
  CHECK_EQ(h["rutas"].size(), (size_t)2);
  json x = h["rutas"][0];   // ordenadas por p95: la lenta primero
  CHECK_EQ(jstr(x, "ruta"), std::string("GET /api/x/{id}"));
  CHECK_EQ(x["n"].get<long long>(), 100LL);
  CHECK_EQ(x["errores"].get<long long>(), 1LL);
  CHECK(x["p50_ms"].get<double>() > 49 && x["p50_ms"].get<double>() < 52);
  CHECK(x["p95_ms"].get<double>() > 94 && x["p95_ms"].get<double>() < 97);
  CHECK_EQ(x["max_ms"].get<double>(), 100.0);
  CHECK_EQ(h["total"]["n"].get<long long>(), 101LL);
  beta::reset_http();
  CHECK(beta::http_stats()["rutas"].empty());
}

TEST("beta: opciones del banco de pruebas con limites") {
  auto o = beta::bench_opts(json{{"rondas", 99}, {"max_tokens", 2}, {"llm", false}, {"cliente", {{"fps", 60}}}});
  CHECK_EQ(o.rondas, 10);
  CHECK_EQ(o.max_tokens, 16);
  CHECK(!o.llm);
  CHECK(o.sistema);
  CHECK_EQ(o.cliente["fps"].get<int>(), 60);
  auto d = beta::bench_opts(nullptr);
  CHECK_EQ(d.rondas, 3);
  CHECK(d.llm);
}

TEST("beta: medicion del sistema, historial, opiniones e informe") {
  auto dir = std::filesystem::temp_directory_path() / ("nexus-beta-" + std::to_string(::time(nullptr)));
  std::filesystem::create_directories(dir);
  set_env("NEXUS_DATA_DIR", S(dir));
  reset_config();
  Services s(false);
  s.start();

  beta::BenchOpts o;
  o.llm = false;
  json r = beta::run_bench(s, o);
  CHECK(r["cpu"]["mops_1_hilo"].get<double>() > 0);
  CHECK(r["cpu"]["mops_todos"].get<double>() > 0);
  CHECK(r["memoria"]["copia_gbs"].get<double>() > 0);
  CHECK(r["disco"]["escritura_mbs"].get<double>() > 0);
  CHECK(!r.contains("llm"));
  CHECK_EQ(jstr(r, "version"), std::string(NEXUS_VERSION));
  CHECK(!std::filesystem::exists(paths::sub({"beta"}) / "bench-disco.tmp"));
  CHECK_EQ(beta::bench_history().size(), (size_t)1);

  // sin LLM real la parte de IA se omite con su motivo, no falla
  beta::BenchOpts solo_llm;
  solo_llm.sistema = false;
  json r2 = beta::run_bench(s, solo_llm);
  CHECK(r2["llm"].contains("omitido"));
  CHECK_EQ(beta::bench_history().size(), (size_t)2);

  bool bad = false;
  try { beta::save_opinion(json{{"zona", "chat"}, {"nota", 9}}); } catch (const std::invalid_argument&) { bad = true; }
  CHECK(bad);
  bad = false;
  try { beta::save_opinion(json{{"zona", "inventada"}, {"nota", 3}}); } catch (const std::invalid_argument&) { bad = true; }
  CHECK(bad);
  json op = beta::save_opinion(json{{"zona", "chat"}, {"nota", 4}, {"texto", "Se lee bien"}, {"tema", "neon"}, {"vitals", {{"fps", 58}}}});
  CHECK_EQ(op["nota"].get<int>(), 4);
  CHECK_EQ(beta::opinions().size(), (size_t)1);
  CHECK_EQ(beta::opinions()[0]["vitals"]["fps"].get<int>(), 58);

  std::string md = beta::report_markdown(s);
  CHECK(md.find("Informe de la beta") != std::string::npos);
  CHECK(md.find("| chat | 1 | 4.0 |") != std::string::npos);
  CHECK(md.find("Se lee bien") != std::string::npos);
  s.shutdown();
}
