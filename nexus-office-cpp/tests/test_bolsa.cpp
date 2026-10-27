// Cotizaciones reales con fuentes falsas y agente de bolsa autonomo (tests/test_bolsa_agente.py).
#include <chrono>
#include <cmath>
#include <map>

#include "check.h"
#include "office/bolsa.h"
#include "office/bolsa_agente.h"
#include "office/bolsa_datos.h"
#include "util/fs.h"
#include "util/text.h"

using namespace nx;
using namespace nx::bolsa;

namespace {

fs::path tmpdb(const std::string& name) {
  auto d = std::filesystem::temp_directory_path() /
           ("nexus-bolsa-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(d);
  return d / "b.db";
}

double utc(int y, unsigned m, unsigned d, int h, int mi) {
  namespace ch = std::chrono;
  auto days = ch::sys_days(ch::year_month_day(ch::year(y), ch::month(m), ch::day(d))).time_since_epoch().count();
  return days * 86400.0 + h * 3600 + mi * 60;
}

json yahoo_chart(double price, double prev, int n = 0, double step = 0, const std::string& currency = "EUR",
                 double t0 = 1'790'000'000) {
  json ts = json::array(), closes = json::array(), hi = json::array(), lo = json::array();
  for (int i = 0; i < n; ++i) {
    double c = price - step * (n - 1 - i);
    ts.push_back(t0 + i * 86400.0);
    closes.push_back(c);
    hi.push_back(c * 1.01);
    lo.push_back(c * 0.99);
  }
  return {{"chart",
           {{"result",
             json::array({{{"meta", {{"regularMarketPrice", price}, {"previousClose", prev}, {"currency", currency},
                                     {"regularMarketTime", t0 + n * 86400.0}}},
                           {"timestamp", ts},
                           {"indicators", {{"quote", json::array({{{"close", closes}, {"high", hi}, {"low", lo}}})}}}}})}}}};
}

http::Response resp(int st, const std::string& body = "") {
  http::Response r;
  r.status = st;
  r.body = body;
  return r;
}

std::string url_sym(const std::string& url) {
  std::string path = url.substr(0, url.find('?'));
  return path.substr(path.rfind('/') + 1);
}

// proveedor determinista: SAN.MC sube con fuerza, TEF.MC cae, el resto plano
struct FakeDatos : Proveedor {
  std::map<std::string, double> px = {{"SAN.MC", 8.0}, {"TEF.MC", 3.0}, {"ITX.MC", 50.0}};
  json quote(const std::string& sym, double) override {
    if (!px.count(sym)) return nullptr;
    return {{"precio", px[sym]}, {"cierre_anterior", px[sym]}, {"fuente", "prueba"}, {"retraso", ""},
            {"puntos", json::array()}};
  }
  json serie(const std::string& sym, const std::string&) override {
    json out = json::array();
    if (!px.count(sym)) return out;
    int n = 120;
    for (int i = 0; i < n; ++i) {
      double p = sym == "SAN.MC"   ? 5.0 + 3.0 * i / (n - 1) + 0.15 * std::sin(i * 1.3)
                 : sym == "TEF.MC" ? 5.0 - 2.0 * i / (n - 1)
                                   : 50.0 + 0.2 * std::sin(i / 3.0);
      out.push_back({{"ts", 1.7e9 + i * 86400.0}, {"precio", p}});
    }
    return out;
  }
  double fx(const std::string&) override { return 0; }
  json estado() override { return {{"en_linea", true}, {"fuentes", json::object()}}; }
};

}  // namespace

TEST("bolsa: cotizaciones reales de Yahoo, Stooq y Coinbase con cache y backoff") {
  DatosMercado d([](const http::Request& r) {
    if (r.url.find("yahoo.com") != std::string::npos) {
      std::string sym = url_sym(r.url);
      if (sym == "SAN.MC") return resp(200, yahoo_chart(7.25, 7.0, 3, 0.1).dump());
      if (sym == "EURUSD=X") return resp(200, yahoo_chart(1.08, 1.07).dump());
      return resp(503);
    }
    if (r.url.find("stooq.com") != std::string::npos)
      return resp(200, "Symbol,Date,Time,Open,High,Low,Close,Volume,Prev\nAAPL.US,2026-10-26,22:00:00,230,233,229,231.5,1000,228.0\n");
    if (r.url.find("api.coinbase.com") != std::string::npos)
      return resp(200, R"({"data": {"amount": "61234.5", "currency": "EUR"}})");
    return resp(404);
  });
  json q = d.quote("SAN.MC");
  CHECK_EQ(jnum(q["precio"]), 7.25);
  CHECK_EQ(jnum(q["cierre_anterior"]), 7.0);
  CHECK_EQ(jstr(q, "fuente"), std::string("Yahoo Finance"));
  CHECK_EQ(q["puntos"].size(), (size_t)3);
  CHECK(d.quote("san.mc") == q);   // cache
  CHECK_EQ(d.fx("USD"), 1.08);
  json a = d.quote("AAPL");   // Yahoo cae (503) -> Google no responde -> Stooq; Yahoo queda apartada
  CHECK_EQ(jstr(a, "fuente"), std::string("Stooq"));
  CHECK_EQ(jnum(a["precio"]), 231.5);
  CHECK_EQ(jnum(a["cierre_anterior"]), 228.0);
  CHECK(!d.disponible("yahoo"));
  json btc = d.quote("BTC-EUR");
  CHECK_EQ(jstr(btc, "fuente"), std::string("Coinbase"));
  CHECK_EQ(jnum(btc["precio"]), 61234.5);
  CHECK(d.estado()["fuentes"]["yahoo"]["fallos"].get<int>() >= 1);
}

TEST("bolsa: CNBC de respaldo cuando Yahoo limita") {
  DatosMercado d([](const http::Request& r) {
    if (r.url.find("yahoo") != std::string::npos) return resp(429);
    if (r.url.find("cnbc") != std::string::npos) {
      CHECK(r.url.find("symbols=SAN-ES") != std::string::npos);
      return resp(200, R"({"FormattedQuoteResult": {"FormattedQuote": [{"symbol": "SAN-ES", "last": "7,512.5",
                          "previous_day_closing": "7,400", "currencyCode": "EUR"}]}})");
    }
    return resp(404);
  });
  json q = d.quote("SAN.MC");
  CHECK_EQ(jstr(q, "fuente"), std::string("CNBC"));
  CHECK_EQ(jnum(q["precio"]), 7512.5);
  CHECK_EQ(jnum(q["cierre_anterior"]), 7400.0);
  CHECK_EQ(jstr(q, "divisa"), std::string("EUR"));
}

TEST("bolsa: velas de CNBC y cierre anterior deducido de la variacion") {
  DatosMercado d([](const http::Request& r) {
    if (r.url.find("yahoo") != std::string::npos) return resp(429);
    if (r.url.find("charts/6M.json") != std::string::npos) {
      CHECK(r.url.find("symbol=SAN-ES") != std::string::npos);
      return resp(200, R"({"barData": {"priceBars": [
        {"open": "7.0", "high": "7.2", "low": "6.9", "close": "7.1", "volume": 10, "tradeTimeinMills": "1761523200000"},
        {"open": "7.1", "high": "7.4", "low": "7.0", "close": "7.3", "volume": 12, "tradeTimeinMills": "1761609600000"}]}})");
    }
    if (r.url.find("restQuote") != std::string::npos)
      return resp(200, R"({"FormattedQuoteResult": {"FormattedQuote": [{"symbol": "AAPL", "last": "110.00",
                          "change": "+10.00", "change_pct": "+10.00%", "currencyCode": "USD"}]}})");
    return resp(404);
  });
  json pts = d.serie("SAN.MC", "6mo");
  CHECK(pts.size() == 2 && jnum(pts[0]["precio"]) == 7.1 && jnum(pts[1]["precio"]) == 7.3);
  CHECK(jnum(pts[0]["ts"]) == 1761523200 && jnum(pts[1]["maximo"]) == 7.4);
  CHECK_EQ(jnum(d.quote("AAPL")["cierre_anterior"]), 100.0);
}

TEST("bolsa: la mesa usa el precio real, pasa peniques a libras y guarda el historico") {
  DatosMercado d([](const http::Request& r) {
    std::string sym = url_sym(r.url);
    if (sym == "SHEL.L") return resp(200, yahoo_chart(2700.0, 2650.0, 0, 0, "GBp").dump());
    return resp(200, yahoo_chart(7.25, 7.0, 40, 0.01).dump());
  });
  double t = utc(2026, 10, 20, 8, 0);
  {
    Bolsa b(tmpdb("real"), 1, [t] { return t; });
    b.datos = &d;
    json q = b.precio("SAN.MC");
    CHECK(q["real"] == true);
    CHECK_EQ(jnum(q["precio"]), 7.25);
    CHECK_EQ(jstr(q, "fuente"), std::string("Yahoo Finance"));
    CHECK_EQ(jnum(q["variacion_pct"]), 3.57);
    CHECK_EQ(jnum(b.precio("SHEL.L")["precio"]), 27.0);   // 2700 peniques = 27 libras
    json s = b.serie("SAN.MC", "6mo");
    CHECK(s["real"] == true);
    CHECK_EQ(s["puntos"].size(), (size_t)40);
    CHECK_EQ(jnum(b.db().scalar("SELECT COUNT(*) FROM historico WHERE simbolo='SAN.MC'")), 40.0);
    json o = b.ordenar("SAN.MC", "compra", 100);
    CHECK_EQ(jstr(o, "estado"), std::string("ejecutada"));
    CHECK(std::abs(jnum(o["precio_ejec"]) - 7.25) < 0.05);
  }
  {   // sin conexion: sigue simulado
    DatosMercado off([](const http::Request&) { return resp(0); });
    Bolsa b(tmpdb("offline"), 1, [t] { return t; });
    b.datos = &off;
    json q = b.precio("SAN.MC");
    CHECK(q["real"] == false);
    CHECK_EQ(jstr(q, "fuente"), std::string("simulado"));
    CHECK(off.estado()["en_linea"] == false);
  }
}

TEST("bolsa: indicadores del agente") {
  std::vector<double> x;
  json pts = json::array();
  for (int i = 0; i < 60; ++i) {
    x.push_back(100 + i);
    pts.push_back({{"precio", 100 + i}});
  }
  double s = 0;
  for (int i = 40; i < 60; ++i) s += x[i];
  CHECK_EQ(*sma(x, 20), s / 20);
  CHECK_EQ(*rsi(x), 100.0);
  json an = analizar(pts);
  CHECK_EQ(jnum(an["factores"]["tendencia"]), 1.0);
  CHECK(jnum(an["factores"]["momento"]) > 0);
  json corta = json::array();
  for (int i = 0; i < 5; ++i) corta.push_back({{"precio", 1}});
  CHECK(analizar(corta).is_null());
}

TEST("bolsa: el agente autonomo compra, pone stop, vende en el objetivo y aprende") {
  double t = utc(2026, 10, 20, 8, 0);   // martes 10:00 en Madrid: BME abierta
  FakeDatos fd;
  Bolsa b(tmpdb("agente"), 3, [&t] { return t; });
  b.datos = &fd;
  AgenteBolsa ag(b, [] { return json{{"universo", {"SAN.MC", "TEF.MC", "ITX.MC"}}, {"capital", 10000}}; });
  json r = ag.ciclo();
  bool compra = false, tef = false;
  for (auto& a : r["acciones"]) {
    compra |= a == "compra SAN.MC";
    tef |= a.get<std::string>().find("TEF") != std::string::npos;
  }
  CHECK(compra);
  CHECK(!tef);
  long long cid = r["cartera"].get<long long>();
  json pos;
  for (auto& p : b.posiciones(cid))
    if (p["simbolo"] == "SAN.MC") pos = p;
  CHECK(pos.is_object());
  auto g = *b.db().one("SELECT * FROM agente_posiciones WHERE simbolo='SAN.MC'");
  json stop = b.orden(g["orden_stop"].get<long long>());
  CHECK_EQ(jstr(stop, "tipo"), std::string("stop"));
  CHECK_EQ(jstr(stop, "estado"), std::string("pendiente"));
  CHECK(jnum(stop["stop"]) < jnum(g["entrada"]) && jnum(g["entrada"]) < jnum(g["objetivo"]));
  // riesgo: perder hasta el stop cuesta como mucho ~1 % del patrimonio
  CHECK(jnum(pos["cantidad"]) * (jnum(g["entrada"]) - jnum(g["stop"])) <= 10000 * 0.011);
  json est = ag.estado();
  std::string t0 = jstr(est["diario"][0], "tipo");
  CHECK(t0 == "compra" || t0 == "stop");
  CHECK(!est["curva"].empty());
  bool con_stop = false;
  for (auto& p : est["resumen"]["posiciones"]) con_stop |= p["simbolo"] == "SAN.MC" && truthy(p.value("stop", json()));
  CHECK(con_stop);
  // el precio llega al objetivo: vende y sube el peso de los factores que acertaron
  fd.px["SAN.MC"] = jnum(g["objetivo"]) * 1.01;
  double peso0 = jnum(ag.pesos()["tendencia"]);
  json r2 = ag.ciclo();
  bool venta = false;
  for (auto& a : r2["acciones"]) venta |= a == "venta SAN.MC";
  CHECK(venta);
  CHECK(!b.db().one("SELECT * FROM agente_posiciones WHERE simbolo='SAN.MC'"));
  CHECK_EQ(jstr(b.orden(g["orden_stop"].get<long long>()), "estado"), std::string("cancelada"));
  CHECK(jnum(ag.pesos()["tendencia"]) > peso0);
  CHECK_EQ(ag.estado()["estadisticas"]["ganadoras"].get<int>(), 1);
}

TEST("bolsa: salta el stop del agente y lo registra; con la bolsa cerrada no compra") {
  double t = utc(2026, 10, 20, 8, 0);
  FakeDatos fd;
  {
    Bolsa b(tmpdb("stop"), 3, [&t] { return t; });
    b.datos = &fd;
    AgenteBolsa ag(b, [] { return json{{"universo", {"SAN.MC"}}}; });
    ag.ciclo();
    auto g = *b.db().one("SELECT * FROM agente_posiciones WHERE simbolo='SAN.MC'");
    fd.px["SAN.MC"] = jnum(g["stop"]) * 0.97;
    t += 120;
    ag.ciclo();
    CHECK_EQ(jstr(b.orden(g["orden_stop"].get<long long>()), "estado"), std::string("ejecutada"));
    bool salto = false;
    json est = ag.estado();
    for (auto& d : est["diario"]) salto |= d["tipo"] == "stop" && jstr(d, "texto").find("Saltó") != std::string::npos;
    CHECK(salto);
    CHECK_EQ(ag.estado()["estadisticas"]["cerradas"].get<int>(), 1);
  }
  {
    FakeDatos fd2;
    double sab = utc(2026, 10, 24, 12, 0);
    Bolsa b(tmpdb("cerrada"), 3, [sab] { return sab; });
    b.datos = &fd2;
    AgenteBolsa ag(b, [] { return json{{"universo", {"SAN.MC"}}}; });
    CHECK(ag.ciclo()["acciones"].empty());
  }
}

TEST("bolsa: cotizaciones reales contra las fuentes de verdad (NEXUS_ONLINE_TESTS=1)") {
  if (env("NEXUS_ONLINE_TESTS") != "1") return;   // necesita internet (GitHub Actions)
  DatosMercado d;
  Bolsa b(tmpdb("online"));
  b.datos = &d;
  json insts = b.instrumentos("");
  size_t reales = 0;
  std::string fallos;
  for (auto& inst : insts) {
    std::string s = jstr(inst, "simbolo");
    json q = b.precio(s);
    if (q["real"] != true) {
      fallos += s + " ";
      continue;
    }
    ++reales;
    double ref = jnum(b.instrumento(s)["precio_ref"]), px = jnum(q["precio"]);
    std::cerr << "    " << s << " " << px << " " << jstr(q, "divisa") << " " << jstr(q, "fuente") << "\n";
    CHECK(ref / 20 < px && px < ref * 20);
  }
  std::cerr << "    reales " << reales << "/" << insts.size() << "; sin dato: " << fallos << "\n    " << d.estado().dump() << "\n";
  CHECK(reales * 2 >= insts.size());
  double usd = d.fx("USD");
  CHECK(usd > 0.7 && usd < 1.6);
  json btc = d.quote("BTC-EUR");
  CHECK(btc.is_object() && jnum(btc["precio"]) > 0);
}
