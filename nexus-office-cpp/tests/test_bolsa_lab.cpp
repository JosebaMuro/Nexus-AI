// Laboratorio de inversion: senales, backtest, metricas, cartera optima, Monte Carlo, protecciones, mesa de analistas,
// etapas hasta la inversion real y broker (Alpaca con respuestas falsas).
#include <chrono>
#include <cmath>
#include <map>
#include <random>

#include "check.h"
#include "office/bolsa.h"
#include "office/bolsa_agente.h"
#include "office/bolsa_cuant.h"
#include "office/bolsa_mesa.h"
#include "office/bolsa_real.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;
using namespace nx::bolsa;

namespace {

fs::path tmpdb(const std::string& name) {
  auto d = std::filesystem::temp_directory_path() /
           ("nexus-lab-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(d);
  return d / "b.db";
}

double utc(int y, unsigned m, unsigned d, int h, int mi) {
  namespace ch = std::chrono;
  auto days = ch::sys_days(ch::year_month_day(ch::year(y), ch::month(m), ch::day(d))).time_since_epoch().count();
  return days * 86400.0 + h * 3600 + mi * 60;
}

// serie diaria sintetica: deriva + oscilacion + ruido con semilla, con maximo y minimo
json puntos(int n, double p0, double deriva, double ruido, uint64_t semilla, double t0 = 1.75e9) {
  std::mt19937_64 rng(semilla);
  std::normal_distribution<double> g(0, ruido);
  json out = json::array();
  double p = p0;
  for (int i = 0; i < n; ++i) {
    p *= std::exp(deriva + g(rng));
    out.push_back({{"ts", t0 + i * 86400.0}, {"precio", p}, {"maximo", p * 1.008}, {"minimo", p * 0.992}});
  }
  return out;
}

// como la serie de SAN.MC de test_bolsa.cpp: sube con fuerza pero oscilando (RSI razonable)
json sube_oscila(double t0) {
  json out = json::array();
  for (int i = 0; i < 120; ++i)
    out.push_back({{"ts", t0 + i * 86400.0}, {"precio", 5.0 + 3.0 * i / 119 + 0.15 * std::sin(i * 1.3)}});
  return out;
}

std::vector<double> precios(const json& pts) {
  std::vector<double> v;
  for (auto& p : pts) v.push_back(jnum(p["precio"]));
  return v;
}

struct FakeDatos : Proveedor {
  std::map<std::string, json> series;
  json quote(const std::string& sym, double) override {
    if (!series.count(sym)) return nullptr;
    double p = jnum(series[sym].back()["precio"]);
    return {{"precio", p}, {"cierre_anterior", p}, {"fuente", "prueba"}, {"retraso", ""}, {"puntos", json::array()}};
  }
  json serie(const std::string& sym, const std::string&) override { return series.count(sym) ? series[sym] : json::array(); }
  double fx(const std::string&) override { return 0; }
  json estado() override { return {{"en_linea", true}, {"fuentes", json::object()}}; }
};

http::Response resp(int st, const std::string& body) {
  http::Response r;
  r.status = st;
  r.body = body;
  return r;
}

}  // namespace

TEST("bolsa lab: metricas de rendimiento y de operaciones") {
  json m = metricas({100, 110, 99, 121}, 252);
  CHECK_EQ(jnum(m["rentabilidad_pct"]), 21.0);
  CHECK_EQ(jnum(m["max_drawdown_pct"]), 10.0);
  CHECK(jnum(m["sharpe"]) > 0);
  CHECK(jnum(m["var95_pct"]) > 0);
  CHECK_EQ(jnum(metricas({100, 100}, 252)["volatilidad_pct"]), 0.0);
  json o = metricas_operaciones({50, -20, 30, -10});
  CHECK_EQ(jnum(o["aciertos_pct"]), 50.0);
  CHECK_EQ(jnum(o["factor_beneficio"]), 2.67);
  CHECK_EQ(jnum(o["resultado_eur"]), 50.0);
  CHECK(metricas_operaciones({10})["factor_beneficio"].is_null());
  CHECK_EQ(periodos_anio({0, 86400, 172800, 259200}), 252.0);
  CHECK_EQ(periodos_anio({0, 7 * 86400.0, 14 * 86400.0, 21 * 86400.0}), 52.0);
}

TEST("bolsa lab: senales combinadas, MACD, Bollinger y Hurst") {
  CHECK(senales(puntos(40, 10, 0.002, 0.01, 1)).is_null());
  json sube = senales(puntos(200, 10, 0.004, 0.006, 2));
  CHECK_EQ(jstr(sube, "lectura"), std::string("alcista"));
  CHECK_EQ(jnum(sube["estrategias"]["tendencia"]["senal"]), 1.0);
  CHECK(jnum(sube["estrategias"]["momento"]["senal"]) > 0);
  json baja = senales(puntos(200, 10, -0.004, 0.006, 3));
  CHECK_EQ(jstr(baja, "lectura"), std::string("bajista"));
  CHECK(sube["indicadores"]["macd"].is_object() && jnum(sube["indicadores"]["macd"]["macd"]) > 0);
  json bb = bollinger(precios(puntos(60, 10, 0, 0.01, 4)));
  CHECK(jnum(bb["alta"]) > jnum(bb["media"]) && jnum(bb["media"]) > jnum(bb["baja"]));
  auto h = hurst(precios(puntos(400, 10, 0, 0.01, 5)));   // paseo aleatorio: cerca de 0,5
  CHECK(h && *h > 0.35 && *h < 0.65);
  CHECK(!hurst({1, 2, 3}));
  CHECK(ema({1, 2, 3, 4, 5}, 3) && std::abs(*ema({1, 2, 3, 4, 5}, 3) - 4.0) < 1e-9);
}

TEST("bolsa lab: backtest con costes frente a comprar y mantener") {
  std::vector<SerieBT> ss = {serie_bt("SUBE", puntos(260, 20, 0.003, 0.01, 11)),
                             serie_bt("BAJA", puntos(260, 20, -0.003, 0.01, 12)),
                             serie_bt("PLANA", puntos(260, 20, 0, 0.012, 13))};
  json r = backtest(ss, {{"estrategia", "agente"}});
  CHECK_EQ(jstr(r, "estrategia"), std::string("agente"));
  CHECK_EQ(r["simbolos"].size(), (size_t)3);
  CHECK(jnum(r["operaciones"]["operaciones"]) > 0);
  CHECK(r.contains("fuera_muestra"));
  CHECK(!r["curva"].empty() && r["curva"].size() <= 402);
  CHECK(jnum(r["metricas"]["max_drawdown_pct"]) < 30);
  bool compra_sube = false;
  for (auto& o : r["ultimas_operaciones"]) compra_sube |= o["simbolo"] == "SUBE";
  for (auto& p : r["posiciones_abiertas"]) compra_sube |= p["simbolo"] == "SUBE";
  CHECK(compra_sube);
  // mas comision y deslizamiento: peor resultado
  json caro = backtest(ss, {{"estrategia", "agente"}, {"comision_pct", 1.0}, {"comision_min_eur", 20}, {"deslizamiento_bps", 50}});
  CHECK(jnum(caro["metricas"]["valor_final"]) < jnum(r["metricas"]["valor_final"]));
  // comprar y mantener sin costes casi coincide con la referencia
  json bh = backtest(ss, {{"estrategia", "comprar_mantener"}, {"comision_min_eur", 0}, {"comision_pct", 0}, {"deslizamiento_bps", 0}});
  CHECK(std::abs(jnum(bh["metricas"]["rentabilidad_pct"]) - jnum(bh["referencia"]["rentabilidad_pct"])) < 3);
  CHECK(jnum(backtest(ss, {{"estrategia", "senales"}})["metricas"]["valor_inicial"]) > 0);
  CHECK(jnum(backtest(ss, {{"estrategia", "cruce_medias"}})["sesiones"]) > 200);
  bool err = false;
  try {
    backtest(ss, {{"estrategia", "magia"}});
  } catch (const std::invalid_argument&) {
    err = true;
  }
  CHECK(err);
  err = false;
  try {
    backtest({serie_bt("CORTA", puntos(20, 10, 0, 0.01, 1))});
  } catch (const std::invalid_argument&) {
    err = true;
  }
  CHECK(err);
}

TEST("bolsa lab: optimizacion de cartera y asignacion en titulos") {
  std::vector<SerieBT> ss = {serie_bt("CALMA", puntos(250, 50, 0.0005, 0.004, 21)),
                             serie_bt("NERVIO", puntos(250, 50, 0.0008, 0.025, 22)),
                             serie_bt("MEDIO", puntos(250, 50, 0.0006, 0.012, 23))};
  for (auto m : {"igual", "inversa_vol", "min_varianza", "paridad_riesgo", "max_sharpe", "hrp"}) {
    json r = optimizar(ss, m, {{"capital", 10000}});
    double s = 0;
    for (auto it = r["pesos_pct"].begin(); it != r["pesos_pct"].end(); ++it) {
      CHECK(jnum(it.value()) >= -1e-9);
      s += jnum(it.value());
    }
    CHECK(std::abs(s - 100) < 0.1);
    double inv = jnum(r["sobrante_eur"]);
    for (auto it = r["asignacion"].begin(); it != r["asignacion"].end(); ++it) inv += jnum(it.value()["importe_eur"]);
    CHECK(std::abs(inv - 10000) < 0.5);
    CHECK(jnum(r["sobrante_eur"]) >= 0);
    if (std::string(m) != "igual" && std::string(m) != "max_sharpe")
      CHECK(jnum(r["pesos_pct"]["CALMA"]) > jnum(r["pesos_pct"]["NERVIO"]));
  }
  json tope = optimizar(ss, "min_varianza", {{"max_peso_pct", 40}});
  for (auto it = tope["pesos_pct"].begin(); it != tope["pesos_pct"].end(); ++it) CHECK(jnum(it.value()) <= 40.01);
  json rp = optimizar(ss, "paridad_riesgo");   // cada valor aporta mas o menos un tercio del riesgo
  for (auto it = rp["contribucion_riesgo_pct"].begin(); it != rp["contribucion_riesgo_pct"].end(); ++it)
    CHECK(std::abs(jnum(it.value()) - 33.3) < 4);
  bool err = false;
  try {
    optimizar({ss[0]}, "hrp");
  } catch (const std::invalid_argument&) {
    err = true;
  }
  CHECK(err);
}

TEST("bolsa lab: Monte Carlo y protecciones") {
  std::vector<double> rs;
  for (int i = 0; i < 100; ++i) rs.push_back(i % 2 ? 0.01 : -0.008);
  json mc = montecarlo(rs, 500, 100, 1000, 3);
  CHECK(jnum(mc["valor_final"]["p5"]) <= jnum(mc["valor_final"]["p50"]));
  CHECK(jnum(mc["valor_final"]["p50"]) <= jnum(mc["valor_final"]["p95"]));
  CHECK(jnum(montecarlo(std::vector<double>(20, 0.001), 200, 50)["prob_perdida_pct"]) == 0.0);
  double t = 1.79e9;
  json ok = json::array({{{"ts", t - 3 * 86400}, {"total_eur", 10000}}, {{"ts", t - 60}, {"total_eur", 9900}}});
  CHECK(!truthy(protecciones(ok, json::array(), json::object(), t)["bloqueado"]));
  json caida = json::array({{{"ts", t - 5 * 86400}, {"total_eur", 10000}}, {{"ts", t - 60}, {"total_eur", 8000}}});
  json p = protecciones(caida, json::array(), json::object(), t);
  CHECK(truthy(p["bloqueado"]));
  CHECK(!truthy(protecciones(caida, json::array(), {{"max_drawdown_pct", 0}}, t)["bloqueado"]));
  json dia = json::array({{{"ts", t - 3600}, {"total_eur", 10000}}, {{"ts", t - 60}, {"total_eur", 9600}}});
  CHECK(truthy(protecciones(dia, json::array(), json::object(), t)["bloqueado"]));
  json perd = json::array();
  for (int i = 0; i < 4; ++i) perd.push_back({{"ts", t - 3600 * (5 - i)}, {"ganancia_eur", -10}});
  CHECK(truthy(protecciones(ok, perd, json::object(), t)["bloqueado"]));
  CHECK(!truthy(protecciones(ok, perd, json::object(), t + 30 * 3600)["bloqueado"]));   // pasada la pausa
}

TEST("bolsa lab: el agente frena las compras tras una caida fuerte") {
  double t = utc(2026, 10, 20, 8, 0);   // BME abierta
  FakeDatos fd;
  fd.series["SAN.MC"] = sube_oscila(t - 120 * 86400.0);
  Bolsa b(tmpdb("freno"), 3, [&t] { return t; });
  b.datos = &fd;
  AgenteBolsa ag(b, [] { return json{{"universo", {"SAN.MC"}}, {"ia", false}}; });
  ag.cartera();
  // la curva del agente va con la hora del sistema (como su diario)
  b.db().execute("INSERT INTO agente_valor VALUES (?,?,?)", {nx::now() - 86400 * 3, 13000.0, 13000.0});
  b.db().execute("INSERT INTO agente_valor VALUES (?,?,?)", {nx::now() - 60, 10000.0, 10000.0});
  json r = ag.ciclo();
  CHECK(r["acciones"].empty());
  bool nota = false;
  json est = ag.estado();
  for (auto& d : est["diario"]) nota |= d["tipo"] == "proteccion";
  CHECK(nota);
  // sin freno, compra
  Bolsa b2(tmpdb("sin-freno"), 3, [&t] { return t; });
  b2.datos = &fd;
  AgenteBolsa ag2(b2, [] { return json{{"universo", {"SAN.MC"}}, {"ia", false}}; });
  bool compra = false;
  json r2 = ag2.ciclo();
  for (auto& a : r2["acciones"]) compra |= a == "compra SAN.MC";
  CHECK(compra);
}

TEST("bolsa lab: titulares RSS, sentimiento y mesa de analistas") {
  std::string rss = R"(<?xml version="1.0"?><rss><channel><title>x</title>
    <item><title><![CDATA[Santander dispara sus beneficios y eleva el dividendo]]></title><link>https://a/1</link>
      <pubDate>Mon, 19 Oct 2026 08:00:00 GMT</pubDate><source url="x">Expansión</source></item>
    <item><title>Santander cae tras una multa &amp; una investigación</title><link>https://a/2</link>
      <pubDate>Sun, 18 Oct 2026 08:00:00 +0000</pubDate></item></channel></rss>)";
  json it = leer_rss(rss);
  CHECK_EQ(it.size(), (size_t)2);
  CHECK_EQ(jstr(it[0], "fuente"), std::string("Expansión"));
  CHECK(jstr(it[1], "titulo").find("multa & una") != std::string::npos);
  CHECK_EQ(jnum(it[0]["ts"]), utc(2026, 10, 19, 8, 0));
  CHECK(sentimiento(jstr(it[0], "titulo")) > 0);
  CHECK(sentimiento(jstr(it[1], "titulo")) < 0);
  CHECK(sentimiento("Apple beats estimates and raises dividend") > 0);
  CHECK(sentimiento("no sube") < 0);
  CHECK_EQ(sentimiento("reunión de la junta"), 0.0);
  CHECK_EQ(jstr(extraer_json("x {\"decision\": \"comprar\"} y"), "decision"), std::string("comprar"));

  double t = utc(2026, 10, 20, 8, 0);
  FakeDatos fd;
  fd.series["SAN.MC"] = puntos(200, 5, 0.003, 0.006, 41, t - 200 * 86400.0);
  Bolsa b(tmpdb("mesa"), 3, [&t] { return t; });
  b.datos = &fd;
  int pedidas = 0;
  Mesa m(b, [&](const http::Request& rq) {
    ++pedidas;
    CHECK(rq.url.find("https://") == 0);
    return rq.url.find("yahoo") != std::string::npos ? resp(200, rss) : resp(503, "");
  });
  json n = m.noticias("SAN.MC");
  CHECK_EQ(n["noticias"].size(), (size_t)2);
  CHECK(n["en_linea"] == true);
  m.noticias("SAN.MC");   // dentro de la vigencia no vuelve a descargar
  CHECK_EQ(pedidas, 2);
  // sin IA: cada papel lo hace una regla y el informe sale entero
  json inf = m.informe("SAN.MC");
  CHECK(!truthy(inf["con_ia"]));
  for (auto* k : {"tecnico", "noticias", "fundamental"}) CHECK(!jstr(inf["analistas"][k], "texto").empty());
  CHECK_EQ(inf["debate"].size(), (size_t)1);
  std::string d = jstr(inf["decision"], "decision");
  CHECK(d == "comprar" || d == "mantener" || d == "vender");
  CHECK(jnum(inf["riesgo"]["agresivo"]["peso_pct"]) >= jnum(inf["riesgo"]["conservador"]["peso_pct"]));
  CHECK(jnum(inf["riesgo"]["agresivo"]["peso_pct"]) <= 20);
  // con IA: cada papel pregunta al modelo y el gestor tiene la ultima palabra
  std::vector<std::string> prompts;
  m.llm = [&](const std::string& p) -> std::string {
    prompts.push_back(p);
    if (p.find("GESTOR DE CARTERA") != std::string::npos)
      return "Decido. {\"decision\": \"mantener\", \"confianza\": 0.4, \"peso_pct\": 99, \"motivo\": \"esperar resultados\"}";
    if (p.find("EQUIPO DE RIESGO") != std::string::npos)
      return "{\"agresivo\": {\"peso_pct\": 50, \"motivo\": \"a\"}, \"neutral\": {\"peso_pct\": 8, \"motivo\": \"n\"}}";
    if (p.find("OPERADOR") != std::string::npos) return "{\"decision\": \"comprar\", \"motivo\": \"tendencia\"}";
    return "Análisis de prueba.";
  };
  json inf2 = m.informe("SAN.MC", {{"rondas", 2}});
  CHECK(truthy(inf2["con_ia"]));
  CHECK_EQ(inf2["debate"].size(), (size_t)2);
  CHECK_EQ(jstr(inf2["operador"], "decision"), std::string("comprar"));
  CHECK_EQ(jstr(inf2["decision"], "decision"), std::string("mantener"));
  CHECK_EQ(jnum(inf2["decision"]["peso_pct"]), 0.0);   // mantener: no compra nada
  CHECK_EQ(jnum(inf2["riesgo"]["agresivo"]["peso_pct"]), 20.0);   // recortado al peso maximo
  CHECK_EQ(jnum(inf2["riesgo"]["neutral"]["peso_pct"]), 8.0);
  CHECK(prompts.size() >= 9);
  for (auto& p : prompts) CHECK(p.find("no inventes") != std::string::npos);
  CHECK_EQ(m.informes("SAN.MC").size(), (size_t)2);
  CHECK(m.informes("SAN.MC", 1)[0]["informe"].is_object());
  // si la IA falla, no insiste y termina con reglas
  int llamadas = 0;
  m.llm = [&](const std::string&) -> std::string {
    ++llamadas;
    throw std::runtime_error("sin modelo");
  };
  CHECK(!truthy(m.informe("SAN.MC")["con_ia"]));
  CHECK_EQ(llamadas, 1);
}

TEST("bolsa lab: el agente pide permiso a la mesa de analistas") {
  double t = utc(2026, 10, 20, 8, 0);
  FakeDatos fd;
  fd.series["SAN.MC"] = sube_oscila(t - 120 * 86400.0);
  Bolsa b(tmpdb("agente-mesa"), 3, [&t] { return t; });
  b.datos = &fd;
  AgenteBolsa ag(b, [] { return json{{"universo", {"SAN.MC"}}, {"ia", false}, {"mesa", true}}; });
  int consultas = 0;
  ag.mesa = [&](const std::string&) -> json {
    ++consultas;
    return {{"id", 7}, {"decision", {{"decision", "mantener"}, {"motivo", "resultados la semana que viene"}, {"confianza", 0.5}}}};
  };
  json r = ag.ciclo();
  CHECK(r["acciones"].empty());
  ag.ciclo();
  CHECK_EQ(consultas, 1);   // su opinion vale 4 horas
  bool nota = false;
  json est = ag.estado();
  for (auto& d : est["diario"]) nota |= d["tipo"] == "mesa";
  CHECK(nota);
}

TEST("bolsa lab: etapas hasta la inversion real y propuestas por confirmar") {
  double t = utc(2026, 10, 20, 8, 0);
  FakeDatos fd;
  fd.series["SAN.MC"] = sube_oscila(t - 120 * 86400.0);
  Bolsa b(tmpdb("etapas"), 3, [&t] { return t; });
  b.datos = &fd;
  json real_cfg = {{"habilitado", false}};
  json e = etapas(b, real_cfg);
  CHECK_EQ(e["etapas"].size(), (size_t)5);
  CHECK(!truthy(e["puede_proponer_real"]));
  CHECK_EQ(jstr(e["etapas"][4], "estado"), std::string("bloqueada"));
  // banco de datos: guarda la serie diaria
  json bd = banco_descargar(b, {"SAN.MC"});
  CHECK_EQ(jnum(bd["resultado"][0]["sesiones"]), 120.0);
  CHECK_EQ(jstr(etapas(b, real_cfg)["etapas"][0], "estado"), std::string("hecha"));
  // backtest desde la mesa: se guarda y aparece en la lista
  json bt = backtest_mesa(b, {{"simbolos", "SAN.MC"}});
  CHECK(bt["id"].is_number());
  CHECK_EQ(backtests(b).size(), (size_t)1);
  CHECK(backtest_guardado(b, bt["id"].get<long long>())["curva"].is_array());
  // un backtest bueno y una simulacion madura dejan la estrategia lista para el modo real
  json bueno = {{"estrategia", "agente"},
                {"metricas", {{"sharpe", 1.2}, {"max_drawdown_pct", 8}, {"rentabilidad_pct", 15}}},
                {"operaciones", {{"operaciones", 25}}},
                {"fuera_muestra", {{"metricas", {{"rentabilidad_pct", 4}}}}}};
  b.db().execute("INSERT INTO bolsa_backtests (ts, estrategia, simbolos, rango, resultado) VALUES (?,?,?,?,?)",
                 {t, "agente", "SAN.MC", "1y", jdump(bueno)});
  AgenteBolsa ag(b, [] { return json{{"universo", {"SAN.MC"}}, {"ia", false}}; });
  for (int i = 0; i <= 40; ++i) b.db().execute("INSERT INTO agente_valor VALUES (?,?,?)", {t - (40 - i) * 86400.0, 10000.0 + i * 20, 5000.0});
  b.db().execute("INSERT OR REPLACE INTO agente_estado VALUES ('estadisticas', ?)",
                 {jdump(json{{"cerradas", 12}, {"ganadoras", 8}, {"resultado_eur", 800}})});
  e = etapas(b, real_cfg);
  CHECK_EQ(jstr(e["etapas"][2], "estado"), std::string("hecha"));
  CHECK_EQ(jstr(e["etapas"][3], "estado"), std::string("hecha"));
  CHECK_EQ(jstr(e["etapas"][4], "estado"), std::string("lista"));
  CHECK(proponer_real(b, real_cfg, "SAN.MC", "compra", 100, 10000).is_null());   // no habilitado
  json real = b.crear_cartera("Mi bróker", 5000, "broker-online", "real");
  real_cfg = {{"habilitado", true}, {"cartera", real["id"]}};
  CHECK(truthy(etapas(b, real_cfg)["puede_proponer_real"]));
  json o = proponer_real(b, real_cfg, "SAN.MC", "compra", 100, 10000);
  CHECK(o.is_object());
  CHECK_EQ(jstr(o, "estado"), std::string("por_confirmar"));
  CHECK_EQ(jnum(o["cantidad"]), 50.0);   // a escala de los 5.000 € de la cartera real
  CHECK(jstr(o, "origen").find("propuesta") != std::string::npos);
  CHECK(proponer_real(b, real_cfg, "SAN.MC", "venta", 100, 10000).is_null());   // no tiene nada que vender en real
  // el agente lo hace solo tras su compra simulada
  int avisos = 0;
  ag.al_operar = [&](const std::string& sym, const std::string& lado, double qty, double total) {
    ++avisos;
    CHECK_EQ(sym, std::string("SAN.MC"));
    CHECK_EQ(lado, std::string("compra"));
    CHECK(qty > 0 && total > 0);
  };
  ag.ciclo();
  CHECK_EQ(avisos, 1);
}

TEST("bolsa lab: broker Alpaca solo con ordenes confirmadas por la persona") {
  double t = utc(2026, 10, 20, 15, 0);   // Nueva York abierta
  Bolsa b(tmpdb("alpaca"), 3, [&t] { return t; });
  json real = b.crear_cartera("Alpaca", 10000, "broker-online", "real");
  json cfg = {{"tipo", "alpaca"}, {"entorno", "papel"}, {"clave", "K"}, {"secreto", "S"}};
  std::vector<http::Request> pedidas;
  auto fetch = [&](const http::Request& r) {
    pedidas.push_back(r);
    if (r.method == "POST") return resp(200, R"({"id": "ord-1", "status": "accepted"})");
    if (r.url.find("/v2/account") != std::string::npos) return resp(200, R"({"status": "ACTIVE", "cash": "100000", "portfolio_value": "100000"})");
    return resp(200, R"({"id": "ord-1", "status": "filled", "filled_avg_price": "221.5", "filled_qty": "2"})");
  };
  json o = b.ordenar("AAPL", "compra", 2, "mercado", nullptr, nullptr, "dia", real["id"], "persona");
  long long oid = o["id"].get<long long>();
  CHECK_EQ(jstr(o, "estado"), std::string("por_confirmar"));
  bool denegada = false;
  try {
    enviar_orden(b, oid, cfg, fetch);
  } catch (const PermissionDenied&) {
    denegada = true;
  }
  CHECK(denegada);
  CHECK(pedidas.empty());
  b.confirmar(oid, "CONFIRMO");
  json env = enviar_orden(b, oid, cfg, fetch);
  CHECK_EQ(jstr(env["broker"], "id"), std::string("ord-1"));
  CHECK_EQ(pedidas.size(), (size_t)1);
  CHECK_EQ(pedidas[0].url, std::string("https://paper-api.alpaca.markets/v2/orders"));
  json body = jparse(pedidas[0].body);
  CHECK_EQ(jstr(body, "symbol"), std::string("AAPL"));
  CHECK_EQ(jstr(body, "qty"), std::string("2"));
  CHECK_EQ(jstr(body, "side"), std::string("buy"));
  CHECK_EQ(jstr(body, "client_order_id"), "nexus-" + std::to_string(oid));
  bool clave = false;
  for (auto& [k, v] : pedidas[0].headers) clave |= k == "APCA-API-KEY-ID" && v == "K";
  CHECK(clave);
  bool dos = false;
  try {
    enviar_orden(b, oid, cfg, fetch);
  } catch (const std::invalid_argument&) {
    dos = true;
  }
  CHECK(dos);
  json s = sincronizar_broker(b, cfg, fetch);
  CHECK_EQ(jstr(s["actualizadas"][0], "estado"), std::string("registrada"));
  CHECK_EQ(jstr(b.orden(oid), "estado"), std::string("ejecutada"));
  CHECK_EQ(jnum(b.orden(oid)["precio_ejec"]), 221.5);
  CHECK(sincronizar_broker(b, cfg, fetch)["actualizadas"].empty());
  json est = broker_estado(b, cfg, true, fetch);
  CHECK(truthy(est["configurado"]));
  CHECK_EQ(jstr(est["cuenta"], "estado"), std::string("ACTIVE"));
  CHECK_EQ(est["ordenes"].size(), (size_t)1);
  // fuera de EE. UU. no; y sin claves, ni lo intenta
  json o2 = b.ordenar("SAN.MC", "compra", 10, "mercado", nullptr, nullptr, "dia", real["id"], "persona");
  b.confirmar(o2["id"].get<long long>(), "CONFIRMO");
  bool fuera = false;
  try {
    enviar_orden(b, o2["id"].get<long long>(), cfg, fetch);
  } catch (const std::invalid_argument&) {
    fuera = true;
  }
  CHECK(fuera);
  bool sin = false;
  try {
    enviar_orden(b, o2["id"].get<long long>(), {{"tipo", "alpaca"}}, fetch);
  } catch (const std::invalid_argument&) {
    sin = true;
  }
  CHECK(sin);
  // el entorno real solo si se pide expresamente
  json o3 = b.ordenar("MSFT", "compra", 1, "mercado", nullptr, nullptr, "dia", real["id"], "persona");
  b.confirmar(o3["id"].get<long long>(), "CONFIRMO");
  json cfg_real = cfg;
  cfg_real["entorno"] = "real";
  enviar_orden(b, o3["id"].get<long long>(), cfg_real, fetch);
  CHECK(pedidas.back().url.find("https://api.alpaca.markets/") == 0);
}
