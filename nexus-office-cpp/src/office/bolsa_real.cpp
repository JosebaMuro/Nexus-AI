// Banco de datos, backtests guardados, etapas hasta la inversion real y broker (ver bolsa_real.h).
#include "office/bolsa_real.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "office/bolsa_agente.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::bolsa {

namespace {

auto L = log::get("bolsa.real");

const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS bolsa_backtests (id INTEGER PRIMARY KEY, ts REAL, estrategia TEXT, simbolos TEXT, rango TEXT,
    resultado TEXT);
CREATE TABLE IF NOT EXISTS bolsa_broker (orden INTEGER PRIMARY KEY, broker TEXT, entorno TEXT, broker_id TEXT,
    estado TEXT, ts REAL, respuesta TEXT);
)";

void esquema(Bolsa& b) {
  std::lock_guard<std::recursive_mutex> lk(b.mutex());
  b.db().script(SCHEMA);
}

std::string gfmt(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.10g", v);
  return buf;
}

double rnd(double x, int d) {
  double m = std::pow(10.0, d);
  return std::round(x * m) / m;
}

std::vector<Row> consulta(Bolsa& b, const std::string& sql, const Params& p = {}) {
  try {
    return b.db().query(sql, p);
  } catch (const std::exception&) {   // tabla que aun no existe (el agente no ha arrancado nunca)
    return {};
  }
}

const json CRITERIOS = {
    {"datos", {{"min_sesiones", 120}, {"min_valores", 1}}},
    {"investigacion", {{"min_informes_30d", 1}}},
    {"backtest", {{"estrategia", "agente"}, {"min_sharpe", 0.5}, {"max_drawdown_pct", 25}, {"min_operaciones", 10},
                  {"fuera_muestra_positiva", true}}},
    {"simulacion", {{"min_dias", 30}, {"min_cerradas", 10}, {"min_rentabilidad_pct", 0.0}, {"max_drawdown_pct", 20}}}};

}  // namespace

json criterios() { return CRITERIOS; }

std::vector<std::string> lista_simbolos(const json& v) {
  std::vector<std::string> out;
  auto add = [&](std::string s) {
    s = upper(trim(s));
    if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  };
  if (v.is_array()) {
    for (auto& x : v)
      if (x.is_string()) add(x.get<std::string>());
  } else if (v.is_string()) {
    std::string cur;
    for (char c : v.get<std::string>()) {
      if (c == ',' || c == ' ' || c == ';' || c == '\n') add(cur), cur.clear();
      else cur += c;
    }
    add(cur);
  }
  return out;
}

// ---------------------------------------------------------------- banco de datos
json banco(Bolsa& b) {
  json lista = json::array();
  long total = 0;
  for (auto& r : b.db().query("SELECT simbolo, COUNT(*) n, MIN(fecha) desde, MAX(fecha) hasta FROM historico "
                              "GROUP BY simbolo ORDER BY n DESC")) {
    lista.push_back({{"simbolo", r["simbolo"]}, {"sesiones", r["n"]}, {"desde", r["desde"]}, {"hasta", r["hasta"]}});
    total += (long)jnum(r["n"]);
  }
  auto n_not = consulta(b, "SELECT COUNT(*) n FROM bolsa_noticias");
  auto n_inf = consulta(b, "SELECT COUNT(*) n FROM bolsa_informes");
  auto n_bt = consulta(b, "SELECT COUNT(*) n FROM bolsa_backtests");
  return {{"simbolos", lista},
          {"series", lista.size()},
          {"sesiones", total},
          {"noticias", n_not.empty() ? json(0) : n_not[0]["n"]},
          {"informes", n_inf.empty() ? json(0) : n_inf[0]["n"]},
          {"backtests", n_bt.empty() ? json(0) : n_bt[0]["n"]},
          {"en_linea", b.datos ? b.datos->estado().value("en_linea", false) : false},
          {"nota", "Cierres diarios reales guardados en data/bolsa/bolsa.db para trabajar sin conexión; también se "
                   "pueden importar en CSV."}};
}

json banco_descargar(Bolsa& b, const std::vector<std::string>& simbolos, const std::string& rango_in) {
  std::string rango = rango_in == "5y" || rango_in == "6mo" ? rango_in : "1y";
  if (simbolos.empty()) throw std::invalid_argument("indica al menos un símbolo");
  if (simbolos.size() > 60) throw std::invalid_argument("como mucho 60 símbolos por descarga");
  json res = json::array();
  for (auto& s : simbolos) {
    json r = {{"simbolo", s}};
    try {
      json sr = b.serie(s, rango);
      bool diaria = truthy(sr.value("real", json(false)));
      r["sesiones"] = diaria ? sr["puntos"].size() : 0;
      r["guardada"] = diaria && !sr.value("guardado", false);
      if (!diaria) r["error"] = "sin conexión: no hay serie diaria";
      else if (sr.value("guardado", false)) r["error"] = "sin conexión: se usa la serie ya guardada";
    } catch (const std::exception& e) {
      r["error"] = e.what();
    }
    res.push_back(r);
  }
  return {{"rango", rango}, {"resultado", res}, {"banco", banco(b)}};
}

std::vector<SerieBT> series_para(Bolsa& b, const std::vector<std::string>& simbolos, const std::string& rango,
                                 json* avisos) {
  std::vector<SerieBT> out;
  for (auto& s : simbolos) {
    try {
      json inst = b.instrumento(s);
      std::string sym = jstr(inst, "simbolo");
      json sr = b.serie(sym, rango);
      if (!truthy(sr.value("real", json(false)))) {
        if (avisos) avisos->push_back(sym + ": sin histórico diario (descárgalo al banco con conexión o importa un CSV)");
        continue;
      }
      out.push_back(serie_bt(sym, sr["puntos"], b.fx(jstr(inst, "divisa"))));
    } catch (const std::exception& e) {
      if (avisos) avisos->push_back(s + ": " + e.what());
    }
  }
  return out;
}

// ---------------------------------------------------------------- backtests
json backtest_mesa(Bolsa& b, const json& params) {
  esquema(b);
  json p = params.is_object() ? params : json::object();
  auto syms = lista_simbolos(p.value("simbolos", json()));
  if (syms.empty()) {
    for (auto& i : b.instrumentos("")) {
      if (syms.size() >= 12) break;
      syms.push_back(jstr(i, "simbolo"));
    }
  }
  if (syms.size() > 40) throw std::invalid_argument("como mucho 40 valores por backtest");
  std::string rango = jstr(p, "rango", "1y");
  if (rango != "6mo" && rango != "1y" && rango != "5y") throw std::invalid_argument("rango: 6mo, 1y o 5y");
  json avisos = json::array();
  auto series = series_para(b, syms, rango, &avisos);
  json res = backtest(series, p);
  for (auto& a : avisos) res["avisos"].push_back(a);
  res["rango"] = rango;
  {
    std::lock_guard<std::recursive_mutex> lk(b.mutex());
    b.db().execute("INSERT INTO bolsa_backtests (ts, estrategia, simbolos, rango, resultado) VALUES (?,?,?,?,?)",
                   {b.now(), res["estrategia"], join(res["simbolos"].get<std::vector<std::string>>(), ","), rango, jdump(res)});
    res["id"] = b.db().last_insert_id();
    b.db().execute("DELETE FROM bolsa_backtests WHERE id < (SELECT MAX(id) - 100 FROM bolsa_backtests)");
  }
  return res;
}

json backtests(Bolsa& b, int limite) {
  esquema(b);
  json out = json::array();
  for (auto& r : b.db().query("SELECT id, ts, estrategia, simbolos, rango, resultado FROM bolsa_backtests ORDER BY id DESC "
                              "LIMIT ?",
                              {limite})) {
    json res = jparse(jstr(r, "resultado"));
    json x = {{"id", r["id"]}, {"ts", r["ts"]}, {"estrategia", r["estrategia"]}, {"simbolos", r["simbolos"]}, {"rango", r["rango"]}};
    if (res.is_object()) {
      x["metricas"] = res["metricas"];
      x["referencia"] = res["referencia"];
      x["operaciones"] = res["operaciones"];
      if (res.contains("fuera_muestra")) x["fuera_muestra"] = res["fuera_muestra"]["metricas"];
    }
    out.push_back(x);
  }
  return out;
}

json backtest_guardado(Bolsa& b, long long id) {
  esquema(b);
  auto r = b.db().one("SELECT resultado FROM bolsa_backtests WHERE id=?", {id});
  if (!r) throw std::invalid_argument("no existe el backtest " + std::to_string(id));
  json res = jparse(jstr(*r, "resultado"));
  res["id"] = id;
  return res;
}

// ---------------------------------------------------------------- etapas
json etapas(Bolsa& b, const json& real_cfg) {
  esquema(b);
  json out = json::array();
  auto etapa = [&](const char* id, const char* nombre, bool hecha, json detalle, json falta, json proyectos) {
    out.push_back({{"id", id}, {"nombre", nombre}, {"estado", hecha ? "hecha" : "pendiente"}, {"detalle", detalle},
                   {"falta", falta}, {"proyectos", proyectos}});
    return hecha;
  };
  // 1. datos
  json bn = banco(b);
  long buenas = 0;
  for (auto& s : bn["simbolos"])
    if (jnum(s["sesiones"]) >= jnum(CRITERIOS["datos"]["min_sesiones"])) ++buenas;
  json falta = json::array();
  if (buenas < 1) falta.push_back("descargar al banco al menos un valor con 120 sesiones diarias");
  etapa("datos", "Banco de datos", falta.empty(),
        {{"series", bn["series"]}, {"series_120", buenas}, {"sesiones", bn["sesiones"]}, {"noticias", bn["noticias"]},
         {"en_linea", bn["en_linea"]}},
        falta, json::array({"OpenBB", "Qlib (capa de datos)", "FinRL (procesadores de datos)", "a-stock-data"}));
  // 2. investigacion
  auto inf = consulta(b, "SELECT COUNT(*) n FROM bolsa_informes WHERE ts > ?", {b.now() - 30 * 86400.0});
  long n_inf = inf.empty() ? 0 : (long)jnum(inf[0]["n"]);
  falta = json::array();
  if (n_inf < 1) falta.push_back("pedir a la mesa de analistas un informe de algún valor");
  etapa("investigacion", "Investigación (mesa de analistas IA)", falta.empty(), {{"informes_30d", n_inf}}, falta,
        json::array({"TradingAgents", "ai-hedge-fund", "FinRobot", "FinGPT"}));
  // 3. backtest de la estrategia del agente
  const json& cb = CRITERIOS["backtest"];
  auto bt = consulta(b, "SELECT id, ts, resultado FROM bolsa_backtests WHERE estrategia='agente' ORDER BY id DESC LIMIT 1");
  falta = json::array();
  json det_bt = json::object();
  if (bt.empty()) {
    falta.push_back("hacer un backtest con la estrategia del agente");
  } else {
    json res = jparse(jstr(bt[0], "resultado"));
    json m = res.is_object() ? res["metricas"] : json::object();
    long ops = res.is_object() ? (long)jnum(res["operaciones"]["operaciones"]) : 0;
    det_bt = {{"id", bt[0]["id"]}, {"ts", bt[0]["ts"]}, {"sharpe", m["sharpe"]}, {"max_drawdown_pct", m["max_drawdown_pct"]},
              {"rentabilidad_pct", m["rentabilidad_pct"]}, {"operaciones", ops}};
    if (jnum(m["sharpe"]) < jnum(cb["min_sharpe"])) falta.push_back("Sharpe de al menos 0,5 (ahora " + jdump(m["sharpe"]) + ")");
    if (jnum(m["max_drawdown_pct"]) > jnum(cb["max_drawdown_pct"]))
      falta.push_back("caída máxima de como mucho el 25 % (ahora " + jdump(m["max_drawdown_pct"]) + " %)");
    if (ops < jnum(cb["min_operaciones"])) falta.push_back("al menos 10 operaciones (ahora " + std::to_string(ops) + ")");
    if (res.is_object() && res.contains("fuera_muestra")) {
      double fm = jnum(res["fuera_muestra"]["metricas"]["rentabilidad_pct"]);
      det_bt["fuera_muestra_pct"] = fm;
      if (fm <= 0) falta.push_back("resultado positivo fuera de muestra (ahora " + jdump(res["fuera_muestra"]["metricas"]["rentabilidad_pct"]) + " %)");
    }
  }
  bool ok_bt = etapa("backtest", "Backtest con costes", falta.empty(), det_bt, falta,
                     json::array({"backtrader", "Qlib", "freqtrade", "vectorbt", "QuantStats", "PyPortfolioOpt"}));
  // 4. simulacion (paper trading del agente autonomo)
  const json& cs = CRITERIOS["simulacion"];
  auto cur = consulta(b, "SELECT ts, total_eur FROM agente_valor ORDER BY ts");
  auto est = consulta(b, "SELECT valor FROM agente_estado WHERE clave='estadisticas'");
  json st = est.empty() ? json() : jparse(jstr(est[0], "valor"));
  long cerradas = st.is_object() ? (long)jnum(st["cerradas"]) : 0;
  std::vector<double> tot;
  for (auto& r : cur) tot.push_back(jnum(r["total_eur"]));
  double dias = cur.size() > 1 ? (jnum(cur.back()["ts"]) - jnum(cur.front()["ts"])) / 86400 : 0;
  json mm = metricas(tot, 252);
  falta = json::array();
  if (dias < jnum(cs["min_dias"])) falta.push_back("30 días de simulación (lleva " + std::to_string((int)dias) + ")");
  if (cerradas < jnum(cs["min_cerradas"])) falta.push_back("10 operaciones cerradas (lleva " + std::to_string(cerradas) + ")");
  if (jnum(mm.value("rentabilidad_pct", json(0))) <= jnum(cs["min_rentabilidad_pct"])) falta.push_back("rentabilidad positiva");
  if (jnum(mm.value("max_drawdown_pct", json(0))) > jnum(cs["max_drawdown_pct"])) falta.push_back("caída máxima de como mucho el 20 %");
  bool ok_sim = etapa("simulacion", "Simulación con precios reales (paper trading)", falta.empty(),
                      {{"dias", rnd(dias, 1)}, {"cerradas", cerradas}, {"rentabilidad_pct", mm.value("rentabilidad_pct", json(0))},
                       {"max_drawdown_pct", mm.value("max_drawdown_pct", json(0))}},
                      falta, json::array({"freqtrade (dry-run)", "FinRL (paper trading)", "Lean (paper)"}));
  // 5. real asistido
  bool hab = real_cfg.is_object() && truthy(real_cfg.value("habilitado", json(false)));
  json cid = real_cfg.is_object() ? real_cfg.value("cartera", json(nullptr)) : json(nullptr);
  bool cartera_ok = truthy(cid) && b.db().one("SELECT id FROM carteras WHERE id=? AND modo='real'", {cid});
  falta = json::array();
  if (!ok_bt || !ok_sim) falta.push_back("superar el backtest y la simulación");
  if (!cartera_ok) falta.push_back("crear una cartera en modo real y elegirla");
  if (!hab) falta.push_back("habilitar las propuestas reales (lo decide la persona)");
  bool puede = ok_bt && ok_sim && hab && cartera_ok;
  out.push_back({{"id", "real"},
                 {"nombre", "Inversión real asistida"},
                 {"estado", puede ? "habilitada" : (ok_bt && ok_sim ? "lista" : "bloqueada")},
                 {"detalle", {{"habilitado", hab}, {"cartera", cid}, {"confirmacion", CONFIRMACION}}},
                 {"falta", falta},
                 {"proyectos", json::array({"Lean (brokers)", "FinRL (Alpaca)", "freqtrade (live)"})}});
  return {{"etapas", out},
          {"puede_proponer_real", puede},
          {"criterios", CRITERIOS},
          {"reglas", "El agente opera siempre en simulación. En real solo propone órdenes que no salen hasta que escribes " +
                         std::string(CONFIRMACION) + "; luego las envías al bróker o las ejecutas tú y las registras."},
          {"aviso", AVISO}};
}

json proponer_real(Bolsa& b, const json& real_cfg, const std::string& sym, const std::string& lado, double qty_sim,
                   double total_sim) {
  if (!real_cfg.is_object() || !truthy(real_cfg.value("habilitado", json(false)))) return nullptr;
  json e = etapas(b, real_cfg);
  if (!truthy(e["puede_proponer_real"])) return nullptr;
  json cid = real_cfg["cartera"];
  json res = b.resumen(cid);
  double total_real = jnum(res["total_eur"]);
  if (total_real <= 0 || total_sim <= 0 || qty_sim <= 0) return nullptr;
  double qty = qty_sim * total_real / total_sim;
  if (lado == "venta") {
    double held = 0;
    for (auto& p : res["posiciones"])
      if (jstr(p, "simbolo") == sym) held = jnum(p["cantidad"]);
    qty = std::min(qty, held);
  }
  qty = sym.find('-') != std::string::npos ? std::floor(qty * 1e4) / 1e4 : std::floor(qty);
  if (qty <= 0) return nullptr;
  json o = b.ordenar(sym, lado, qty, "mercado", nullptr, nullptr, "dia", cid, std::string(ORIGEN_AGENTE) + ":propuesta");
  L.info("Propuesta real #", jdump(o["id"]), " ", lado, " ", sym, ": espera la confirmacion de la persona");
  return o;
}

// ---------------------------------------------------------------- broker (Alpaca: paper por defecto)
namespace {

struct Alpaca {
  std::string base, clave, secreto;
  FetchHttp fetch;
  http::Response call(const std::string& metodo, const std::string& ruta, const json& cuerpo = nullptr) {
    http::Request r;
    r.method = metodo;
    r.url = base + ruta;
    r.headers = {{"APCA-API-KEY-ID", clave}, {"APCA-API-SECRET-KEY", secreto}, {"Accept", "application/json"}};
    if (!cuerpo.is_null()) {
      r.body = cuerpo.dump();
      r.content_type = "application/json";
    }
    r.timeout = 15;
    return fetch(r);
  }
};

std::string entorno(const json& cfg) { return jstr(cfg, "entorno", "papel") == "real" ? "real" : "papel"; }

Alpaca alpaca(const json& cfg, FetchHttp fetch) {
  if (jstr(cfg, "tipo") != "alpaca") throw std::invalid_argument("no hay ningún bróker conectado (bolsa.broker.tipo)");
  Alpaca a;
  a.base = entorno(cfg) == "real" ? "https://api.alpaca.markets" : "https://paper-api.alpaca.markets";
  a.clave = jstr(cfg, "clave");
  a.secreto = jstr(cfg, "secreto");
  if (a.clave.empty() || a.secreto.empty()) throw std::invalid_argument("faltan la clave y el secreto de Alpaca");
  a.fetch = fetch ? std::move(fetch) : FetchHttp(http::send);
  return a;
}

std::string error_http(const http::Response& r) {
  json j = r.json_body();
  std::string m = j.is_object() ? jstr(j, "message") : "";
  return r.status ? "HTTP " + std::to_string(r.status) + (m.empty() ? "" : ": " + m) : r.error.empty() ? "sin conexión" : r.error;
}

}  // namespace

json broker_estado(Bolsa& b, const json& cfg_in, bool consultar, FetchHttp fetch) {
  esquema(b);
  json cfg = cfg_in.is_object() ? cfg_in : json::object();
  std::string tipo = jstr(cfg, "tipo", "manual");
  json out = {{"tipo", tipo},
              {"entorno", entorno(cfg)},
              {"configurado", tipo == "alpaca" && !jstr(cfg, "clave").empty() && !jstr(cfg, "secreto").empty()},
              {"enviar_al_confirmar", cfg.value("enviar_al_confirmar", true)},
              {"tipos", json::array({"manual", "alpaca"})},
              {"nota", tipo == "alpaca" ? "Alpaca opera acciones de EE. UU. y cripto en USD. Empieza en papel; el entorno real "
                                          "solo si lo cambias tú, y cada orden sigue necesitando CONFIRMO."
                                        : "Manual: tras escribir CONFIRMO ejecutas la orden en tu bróker y registras el precio."}};
  json env = json::array();
  for (auto& r : b.db().query("SELECT orden, broker, entorno, broker_id, estado, ts FROM bolsa_broker ORDER BY ts DESC LIMIT 30"))
    env.push_back(r);
  out["ordenes"] = env;
  if (consultar && truthy(out["configurado"])) {
    try {
      auto a = alpaca(cfg, fetch);
      auto r = a.call("GET", "/v2/account");
      if (r.ok()) {
        json j = r.json_body();
        out["cuenta"] = {{"estado", j.value("status", json(""))}, {"divisa", j.value("currency", json("USD"))},
                         {"efectivo", j.value("cash", json(""))}, {"valor", j.value("portfolio_value", json(""))}};
      } else {
        out["error"] = error_http(r);
      }
    } catch (const std::exception& e) {
      out["error"] = e.what();
    }
  }
  return out;
}

json enviar_orden(Bolsa& b, long long oid, const json& cfg, FetchHttp fetch) {
  esquema(b);
  json o = b.orden(oid);
  if (jstr(o, "estado") != "confirmada")
    throw PermissionDenied("solo se envían al bróker órdenes reales confirmadas por la persona con " + std::string(CONFIRMACION));
  auto c = b.db().one("SELECT modo FROM carteras WHERE id=?", {o["cartera"]});
  if (!c || jstr(*c, "modo") != "real") throw PermissionDenied("la orden no es de una cartera real");
  if (b.db().one("SELECT orden FROM bolsa_broker WHERE orden=?", {oid}))
    throw std::invalid_argument("la orden " + std::to_string(oid) + " ya se envió al bróker");
  auto a = alpaca(cfg, fetch);
  json inst = b.instrumento(jstr(o, "simbolo"));
  std::string mercado = jstr(inst, "mercado"), sym = jstr(inst, "simbolo");
  if (mercado == "CRIPTO") {
    if (sym.size() < 5 || sym.substr(sym.size() - 4) != "-USD") throw std::invalid_argument("Alpaca solo opera cripto en USD (p. ej. BTC-USD)");
    sym = sym.substr(0, sym.size() - 4) + "/USD";
  } else if (jstr(inst, "divisa") != "USD" || (mercado != "NYSE" && mercado != "NASDAQ")) {
    throw std::invalid_argument("Alpaca solo opera acciones de EE. UU. y cripto en USD; " + sym + " cotiza en " + mercado);
  }
  static const std::map<std::string, std::string> TIPO = {
      {"mercado", "market"}, {"limitada", "limit"}, {"stop", "stop"}, {"stop_limitada", "stop_limit"}};
  json body = {{"symbol", sym},
               {"qty", gfmt(jnum(o["cantidad"]))},
               {"side", jstr(o, "lado") == "compra" ? "buy" : "sell"},
               {"type", TIPO.at(jstr(o, "tipo", "mercado"))},
               {"time_in_force", mercado == "CRIPTO" ? "gtc" : jstr(o, "validez") == "hasta_cancelar" ? "gtc" : "day"},
               {"client_order_id", "nexus-" + std::to_string(oid)}};
  if (truthy(o.value("limite", json()))) body["limit_price"] = gfmt(jnum(o["limite"]));
  if (truthy(o.value("stop", json()))) body["stop_price"] = gfmt(jnum(o["stop"]));
  auto r = a.call("POST", "/v2/orders", body);
  if (!r.ok()) throw std::runtime_error("Alpaca rechazó la orden: " + error_http(r));
  json j = r.json_body();
  {
    std::lock_guard<std::recursive_mutex> lk(b.mutex());
    b.db().execute("INSERT INTO bolsa_broker VALUES (?,?,?,?,?,?,?)",
                   {oid, "alpaca", entorno(cfg), jstr(j, "id"), jstr(j, "status", "new"), b.now(), r.body});
  }
  L.info("Orden real #", oid, " enviada a Alpaca (", entorno(cfg), "): ", jstr(j, "id"));
  json out = b.orden(oid);
  out["broker"] = {{"nombre", "alpaca"}, {"entorno", entorno(cfg)}, {"id", jstr(j, "id")}, {"estado", jstr(j, "status")}};
  return out;
}

json sincronizar_broker(Bolsa& b, const json& cfg, FetchHttp fetch) {
  esquema(b);
  json hechos = json::array();
  auto filas = b.db().query("SELECT * FROM bolsa_broker WHERE estado NOT IN ('registrada','canceled','expired','rejected')");
  if (filas.empty()) return {{"actualizadas", hechos}};
  auto a = alpaca(cfg, fetch);
  for (auto& f : filas) {
    long long oid = (long long)jnum(f["orden"]);
    auto r = a.call("GET", "/v2/orders/" + jstr(f, "broker_id"));
    if (!r.ok()) {
      hechos.push_back({{"orden", oid}, {"error", error_http(r)}});
      continue;
    }
    json j = r.json_body();
    std::string st = jstr(j, "status");
    std::string nuevo = st;
    if (st == "filled" && truthy(j.value("filled_avg_price", json()))) {
      try {
        json pj = j["filled_avg_price"];
        double px = pj.is_string() ? std::stod(pj.get<std::string>()) : jnum(pj);
        b.registrar_ejecucion(oid, px, 0.0, nullptr, nullptr);
        nuevo = "registrada";
      } catch (const std::exception& e) {
        hechos.push_back({{"orden", oid}, {"error", e.what()}});
      }
    } else if (st == "canceled" || st == "expired" || st == "rejected") {
      try {
        b.cancelar(oid);
      } catch (const std::exception&) {
      }
    }
    {
      std::lock_guard<std::recursive_mutex> lk(b.mutex());
      b.db().execute("UPDATE bolsa_broker SET estado=?, ts=?, respuesta=? WHERE orden=?", {nuevo, b.now(), r.body, oid});
    }
    hechos.push_back({{"orden", oid}, {"estado", nuevo}});
  }
  return {{"actualizadas", hechos}};
}

}  // namespace nx::bolsa
