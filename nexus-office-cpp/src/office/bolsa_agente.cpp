// Agente de bolsa autonomo (nexus/office/bolsa_agente.py): mismas reglas, mismas tablas y mismos JSON.
#include "office/bolsa_agente.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>

#include "core/config.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::bolsa {

const char* NOMBRE_CARTERA = "Agente autónomo (simulada)";
const char* ORIGEN_AGENTE = "agente-autonomo";

namespace {

auto L = log::get("bolsa.agente");
const std::vector<std::string> FACTORES = {"tendencia", "momento", "rsi", "volatilidad"};
const double UMBRAL_COMPRA = 0.9;
const double RSI_MAX_COMPRA = 80;          // no compra lo sobrecomprado
const double ESPERA_RECOMPRA_S = 6 * 3600;  // tras vender un valor no lo vuelve a comprar en 6 horas
const size_t MIN_PUNTOS = 30;

const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS agente_estado (clave TEXT PRIMARY KEY, valor TEXT);
CREATE TABLE IF NOT EXISTS agente_diario (id INTEGER PRIMARY KEY, ts REAL, tipo TEXT, simbolo TEXT, texto TEXT,
    datos TEXT);
CREATE TABLE IF NOT EXISTS agente_valor (ts REAL PRIMARY KEY, total_eur REAL, efectivo_eur REAL);
CREATE TABLE IF NOT EXISTS agente_posiciones (simbolo TEXT PRIMARY KEY, entrada REAL, stop REAL, objetivo REAL,
    atr REAL, orden_stop INTEGER, orden_entrada INTEGER, abierta REAL, factores TEXT);
)";

json pesos_iniciales() { return {{"tendencia", 1.0}, {"momento", 1.0}, {"rsi", 0.6}, {"volatilidad", 0.4}}; }

std::string fmt(const char* f, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}
std::string g4(double v) { return fmt("%.4g", v); }      // f"{v:.4g}"
std::string gg(double v) { return fmt("%g", v); }        // f"{v:g}"
std::string sg2(double v) { return fmt("%+.2f", v); }    // f"{v:+.2f}"
double clamp1(double v) { return std::max(-1.0, std::min(1.0, v)); }
double rnd(double x, int d) {
  double m = std::pow(10.0, d);
  return std::round(x * m) / m;
}
json opt(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }

}  // namespace

// ---------------------------------------------------------------- indicadores
std::optional<double> sma(const std::vector<double>& x, size_t n) {
  if (x.size() < n || !n) return std::nullopt;
  double s = 0;
  for (size_t i = x.size() - n; i < x.size(); ++i) s += x[i];
  return s / n;
}

std::optional<double> rsi(const std::vector<double>& x, size_t n) {
  if (x.size() <= n) return std::nullopt;
  double gains = 0, losses = 0;
  for (size_t i = x.size() - n; i < x.size(); ++i) {
    double d = x[i] - x[i - 1];
    gains += std::max(d, 0.0);
    losses += std::max(-d, 0.0);
  }
  if (losses == 0) return 100.0;
  return 100 - 100 / (1 + gains / losses);
}

std::optional<double> atr(const json& pts, size_t n) {
  if (pts.size() <= n) return std::nullopt;
  double s = 0;
  for (size_t i = pts.size() - n; i < pts.size(); ++i) {
    const json& p = pts[i];
    double prev = jnum(pts[i - 1]["precio"]), px = jnum(p["precio"]);
    double hi = p.contains("maximo") ? jnum(p["maximo"]) : px, lo = p.contains("minimo") ? jnum(p["minimo"]) : px;
    s += std::max({hi - lo, std::abs(hi - prev), std::abs(lo - prev)});
  }
  return s / n;
}

std::optional<double> volatilidad(const std::vector<double>& x, size_t n) {
  if (x.size() <= n) return std::nullopt;
  std::vector<double> rets;
  for (size_t i = x.size() - n; i < x.size(); ++i)
    if (x[i - 1] > 0 && x[i] > 0) rets.push_back(std::log(x[i] / x[i - 1]));
  if (rets.size() < 2) return std::nullopt;
  double m = 0;
  for (double r : rets) m += r;
  m /= rets.size();
  double v = 0;
  for (double r : rets) v += (r - m) * (r - m);
  return std::sqrt(v / (rets.size() - 1));
}

json analizar(const json& pts) {
  std::vector<double> x;
  for (auto& p : pts)
    if (truthy(p.value("precio", json()))) x.push_back(jnum(p["precio"]));
  if (x.size() < MIN_PUNTOS) return nullptr;
  double s20 = *sma(x, 20), s50 = *sma(x, std::min<size_t>(50, x.size()));
  auto r = rsi(x);
  auto a = atr(pts);
  auto v = volatilidad(x);
  double last = x.back();
  double roc = x.size() > 21 ? last / x[x.size() - 21] - 1 : 0.0;
  double vol = v && *v ? *v : 0.02;
  double f_tend = (last > s50 ? 1.0 : -1.0) * 0.5 + (s20 > s50 ? 0.5 : -0.5);
  double f_mom = clamp1(roc / (vol * std::sqrt(20.0) * 1.5));
  double f_rsi = !r ? 0.0 : (*r >= 45 && *r <= 68) ? 1.0 : (*r >= 78 || *r <= 25) ? -1.0 : *r < 45 ? 0.3 : -0.3;
  double f_vol = clamp1((0.025 - vol) / 0.02);   // premia lo menos volatil
  return {{"precio", last},
          {"sma20", s20},
          {"sma50", s50},
          {"rsi", opt(r)},
          {"atr", a && *a ? *a : last * vol},
          {"vol_diaria", vol},
          {"roc20", roc},
          {"factores", {{"tendencia", f_tend}, {"momento", f_mom}, {"rsi", f_rsi}, {"volatilidad", f_vol}}}};
}

double puntuar(const json& an, const json& pesos) {
  double s = 0;
  for (auto& k : FACTORES) s += jnum(pesos.value(k, json(0))) * jnum(an["factores"][k]);
  return s;
}

// ---------------------------------------------------------------- AgenteBolsa
AgenteBolsa::AgenteBolsa(Bolsa& mesa, Ajustes ajustes)
    : b(mesa), ajustes_(ajustes ? std::move(ajustes) : Ajustes([] { return json::object(); })) {
  std::lock_guard<std::recursive_mutex> lk(b.mutex());
  b.db().script(SCHEMA);
}

AgenteBolsa::~AgenteBolsa() {
  stop();
  if (th_.joinable()) th_.join();
}

json AgenteBolsa::get(const std::string& k, const json& def) {
  auto r = b.db().one("SELECT valor FROM agente_estado WHERE clave=?", {k});
  if (!r) return def;
  json v = jparse(jstr(*r, "valor"));
  return v.is_discarded() ? def : v;
}

void AgenteBolsa::put(const std::string& k, const json& v) {
  std::lock_guard<std::recursive_mutex> lk(b.mutex());
  b.db().execute("INSERT OR REPLACE INTO agente_estado VALUES (?,?)", {k, jdump(v)});
}

json AgenteBolsa::cfg() {
  json d = {{"activo", true},         {"intervalo_s", 60},     {"capital", 10000.0}, {"riesgo_pct", 1.0},
            {"max_posiciones", 8},    {"max_peso_pct", 20.0},  {"reserva_pct", 10.0}, {"universo", json::array()}};
  json a = ajustes_();
  if (a.is_object())
    for (auto it = a.begin(); it != a.end(); ++it)
      if (!it.value().is_null() && !(it.value().is_string() && it.value().get<std::string>().empty())) d[it.key()] = it.value();
  return d;
}

long long AgenteBolsa::cartera() {
  json cid = get("cartera");
  if (truthy(cid) && b.db().one("SELECT id FROM carteras WHERE id=? AND modo='simulado'", {cid}))
    return cid.get<long long>();
  json c = b.crear_cartera(NOMBRE_CARTERA, jnum(cfg()["capital"]), "broker-online", "simulado");
  put("cartera", c["id"]);
  diario("inicio", "", "Cartera simulada creada con " + fmt("%.2f", jnum(c["capital_inicial"])) + " € para operar sola.");
  return c["id"].get<long long>();
}

json AgenteBolsa::pesos() {
  json p = pesos_iniciales();
  json s = get("pesos");
  if (s.is_object())
    for (auto it = s.begin(); it != s.end(); ++it) p[it.key()] = it.value();
  return p;
}

std::vector<std::string> AgenteBolsa::universo() {
  std::vector<std::string> u;
  json cu = cfg()["universo"];
  if (cu.is_array())
    for (auto& s : cu)
      if (s.is_string()) u.push_back(upper(s.get<std::string>()));
  if (u.empty())
    for (auto& i : b.instrumentos("")) u.push_back(jstr(i, "simbolo"));
  return u;
}

void AgenteBolsa::diario(const std::string& tipo, const std::string& simbolo, const std::string& texto,
                         const json& datos) {
  {
    std::lock_guard<std::recursive_mutex> lk(b.mutex());
    b.db().transaction([&] {
      b.db().execute("INSERT INTO agente_diario (ts, tipo, simbolo, texto, datos) VALUES (?,?,?,?,?)",
                     {now(), tipo, simbolo, texto, jdump(datos.is_object() ? datos : json::object())});
      b.db().execute("DELETE FROM agente_diario WHERE id < (SELECT MAX(id) - 2000 FROM agente_diario)");
    });
  }
  L.info("Agente de bolsa [" + tipo + "] " + simbolo + " " + texto);
  json ev = {{"type", "bolsa_agente"}, {"tipo", tipo}, {"simbolo", simbolo}, {"texto", texto}};
  for (auto& fn : listeners) {
    try {
      fn(ev);
    } catch (...) {
    }
  }
}

json AgenteBolsa::recopilar(const std::string& sym) {
  json pts = b.serie(sym, "6mo")["puntos"];
  if (pts.size() < MIN_PUNTOS) pts = b.serie(sym, "1d")["puntos"];   // sin historico diario: ticks de la mesa
  if (pts.size() < MIN_PUNTOS) pts = historia_simulada(sym);         // ni eso: historia coherente con su volatilidad
  json an = analizar(pts);
  if (an.is_object()) {
    json q = b.precio(sym);
    an["precio"] = q["precio"];
    an["real"] = q.value("real", false);
    an["abierto"] = q["abierto"];
    an["fuente"] = q["fuente"];
  }
  return an;
}

json AgenteBolsa::historia_simulada(const std::string& sym, int n) {
  json inst = b.instrumento(sym);
  std::mt19937_64 rng(std::hash<std::string>{}(sym + "-" + std::to_string((long long)(now() / 86400))));
  double p = jnum(b.precio(sym)["precio"]), t = now();
  double dv = jnum(inst["vol"]) / std::sqrt(252.0);
  std::normal_distribution<double> g(0.0002, dv);
  std::vector<json> out;
  for (int i = 0; i < n; ++i) {
    out.push_back({{"ts", t - i * 86400.0}, {"precio", p}});
    p = p / std::exp(g(rng));
  }
  std::reverse(out.begin(), out.end());
  return json(out);
}

json AgenteBolsa::ciclo() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  long long cid = cartera();
  b.procesar();
  sincronizar(cid);
  json c = cfg();
  json ps = pesos();
  json analisis = json::object();
  for (auto& sym : universo()) {
    json an;
    try {
      an = recopilar(sym);
    } catch (const std::exception& e) {   // un valor sin datos no para al resto
      L.debug("sin datos de " + sym + ": " + e.what());
      continue;
    }
    if (an.is_object()) {
      an["puntuacion"] = rnd(puntuar(an, ps), 3);
      analisis[sym] = an;
    }
  }
  auto hechos = gestionar(cid, analisis);
  auto nuevos = abrir(cid, analisis, c);
  hechos.insert(hechos.end(), nuevos.begin(), nuevos.end());
  json res = b.resumen(cid);
  {
    std::lock_guard<std::recursive_mutex> lk2(b.mutex());
    b.db().execute("INSERT OR REPLACE INTO agente_valor VALUES (?,?,?)", {now(), res["total_eur"], res["efectivo_eur"]});
  }
  std::vector<json> rank;
  for (auto it = analisis.begin(); it != analisis.end(); ++it) {
    const json& a = it.value();
    rank.push_back({{"simbolo", it.key()},
                    {"puntuacion", a["puntuacion"]},
                    {"precio", a["precio"]},
                    {"rsi", a["rsi"].is_number() ? json(rnd(jnum(a["rsi"]), 1)) : json(nullptr)},
                    {"roc20_pct", rnd(jnum(a["roc20"]) * 100, 2)},
                    {"real", a.value("real", false)}});
  }
  std::stable_sort(rank.begin(), rank.end(),
                   [](const json& x, const json& y) { return jnum(x["puntuacion"]) > jnum(y["puntuacion"]); });
  if (rank.size() > 25) rank.resize(25);
  put("ranking", json(rank));
  ultimo_ciclo = now();
  return {{"cartera", cid}, {"acciones", hechos}, {"valores_analizados", analisis.size()}, {"total_eur", res["total_eur"]}};
}

void AgenteBolsa::sincronizar(long long cid) {
  std::set<std::string> held;
  for (auto& p : b.posiciones(cid)) held.insert(jstr(p, "simbolo"));
  for (auto& p : b.db().query("SELECT * FROM agente_posiciones")) {
    std::string sym = jstr(p, "simbolo");
    if (held.count(sym)) continue;
    json ent = truthy(p["orden_entrada"]) ? b.orden(p["orden_entrada"].get<long long>()) : json();
    if (ent.is_object()) {
      std::string st = jstr(ent, "estado");
      if (st == "pendiente") continue;   // la compra aun no se ha ejecutado (bolsa cerrada)
      if (st == "rechazada" || st == "caducada" || st == "cancelada") {
        borrar(sym);
        continue;
      }
    }
    auto op = b.db().one("SELECT * FROM operaciones WHERE cartera=? AND simbolo=? AND lado='venta' ORDER BY ts DESC LIMIT 1",
                         {cid, sym});
    if (op) {
      double g = jnum((*op)["ganancia_eur"]);
      aprender(p, g);
      diario("stop", sym,
             "Saltó el stop de protección a " + g4(jnum((*op)["precio"])) + ": resultado " + sg2(g) + " € con gastos.",
             {{"orden", (*op)["orden"]}});
    }
    borrar(sym);
  }
}

void AgenteBolsa::borrar(const std::string& sym) {
  json vend = get("vendidos", json::object());
  if (!vend.is_object()) vend = json::object();
  double t = now();
  vend[sym] = t;
  json keep = json::object();
  for (auto it = vend.begin(); it != vend.end(); ++it)
    if (t - jnum(it.value()) < ESPERA_RECOMPRA_S) keep[it.key()] = it.value();
  put("vendidos", keep);
  std::lock_guard<std::recursive_mutex> lk(b.mutex());
  b.db().execute("DELETE FROM agente_posiciones WHERE simbolo=?", {sym});
}

void AgenteBolsa::aprender(const json& p, double ganancia) {
  json f = jparse(p["factores"].is_string() ? p["factores"].get<std::string>() : "{}");
  if (!f.is_object() || f.empty()) return;
  json ps = pesos();
  double sign = ganancia > 0 ? 1.0 : -1.0;
  for (auto& k : FACTORES)
    ps[k] = rnd(std::max(0.05, std::min(2.5, jnum(ps[k]) + 0.08 * sign * jnum(f.value(k, json(0))))), 4);
  put("pesos", ps);
  json st = get("estadisticas");
  if (!st.is_object()) st = {{"cerradas", 0}, {"ganadoras", 0}, {"resultado_eur", 0.0}};
  st["cerradas"] = st.value("cerradas", 0) + 1;
  st["ganadoras"] = st.value("ganadoras", 0) + (ganancia > 0 ? 1 : 0);
  st["resultado_eur"] = rnd(jnum(st["resultado_eur"]) + ganancia, 2);
  put("estadisticas", st);
}

std::optional<std::string> AgenteBolsa::vender(long long cid, const std::string& sym, double qty,
                                               const std::string& motivo, const json& p) {
  if (truthy(p.value("orden_stop", json()))) {
    json o = b.orden(p["orden_stop"].get<long long>());
    if (jstr(o, "estado") == "pendiente") b.cancelar(o["id"].get<long long>());
  }
  json o;
  try {
    o = b.ordenar(sym, "venta", qty, "mercado", nullptr, nullptr, "dia", cid, ORIGEN_AGENTE);
  } catch (const std::invalid_argument& e) {
    diario("error", sym, std::string("No pudo vender: ") + e.what());
    return std::nullopt;
  }
  if (jstr(o, "estado") == "ejecutada") {
    auto op = b.db().one("SELECT ganancia_eur FROM operaciones WHERE orden=?", {o["id"]});
    double g = op ? jnum((*op)["ganancia_eur"]) : 0.0;
    aprender(p, g);
    borrar(sym);
    diario("venta", sym, motivo + ". Vende " + gg(qty) + " a " + g4(jnum(o["precio_ejec"])) + ": " + sg2(g) + " € con gastos.",
           {{"orden", o["id"]}});
  } else {
    diario("venta", sym, motivo + ". Orden de venta #" + std::to_string(o["id"].get<long long>()) + " en espera: " +
                             jstr(o, "nota"),
           {{"orden", o["id"]}});
  }
  return "venta " + sym;
}

std::vector<std::string> AgenteBolsa::gestionar(long long cid, const json& analisis) {
  std::vector<std::string> hechos;
  std::map<std::string, json> pos;
  for (auto& p : b.posiciones(cid)) pos[jstr(p, "simbolo")] = p;
  for (auto& p : b.db().query("SELECT * FROM agente_posiciones")) {
    std::string sym = jstr(p, "simbolo");
    if (!pos.count(sym)) continue;
    double qty = jnum(pos[sym]["cantidad"]);
    json an = analisis.contains(sym) ? analisis[sym] : json();
    double px = an.is_object() ? jnum(an["precio"]) : jnum(b.precio(sym)["precio"]);
    double objetivo = jnum(p["objetivo"]);
    if (objetivo && px >= objetivo) {
      if (auto r = vender(cid, sym, qty, "Objetivo alcanzado (" + g4(objetivo) + ")", p)) hechos.push_back(*r);
      continue;
    }
    if (an.is_object() && jnum(an["factores"]["tendencia"]) <= -1.0 && jnum(an["puntuacion"]) < 0) {
      if (auto r = vender(cid, sym, qty, "Tendencia rota (precio y media de 20 bajo la de 50)", p)) hechos.push_back(*r);
      continue;
    }
    double a = an.is_object() && truthy(an["atr"]) ? jnum(an["atr"]) : jnum(p["atr"]);
    double nuevo = px - 2 * a;
    if (nuevo > jnum(p["stop"]) * 1.002 && px > jnum(p["entrada"])) {   // trailing: el stop solo sube
      poner_stop(cid, sym, qty, nuevo, p);
      hechos.push_back("stop " + sym);
    } else {
      bool vivo = false;
      if (truthy(p["orden_stop"])) {
        std::string st = jstr(b.orden(p["orden_stop"].get<long long>()), "estado");
        vivo = st == "pendiente" || st == "ejecutada";
      }
      if (!vivo) poner_stop(cid, sym, qty, jnum(p["stop"]), p);   // stop caducado o perdido: se repone
    }
  }
  return hechos;
}

void AgenteBolsa::poner_stop(long long cid, const std::string& sym, double qty, double stop, const json& p) {
  if (truthy(p.value("orden_stop", json()))) {
    json o = b.orden(p["orden_stop"].get<long long>());
    if (jstr(o, "estado") == "pendiente") b.cancelar(o["id"].get<long long>());
  }
  int dec = stop >= 10 ? 2 : stop >= 1 ? 3 : 4;
  stop = rnd(stop, dec);
  json o;
  try {
    o = b.ordenar(sym, "venta", qty, "stop", nullptr, stop, "hasta_cancelar", cid, ORIGEN_AGENTE);
  } catch (const std::invalid_argument& e) {
    diario("error", sym, std::string("No pudo poner el stop: ") + e.what());
    return;
  }
  {
    std::lock_guard<std::recursive_mutex> lk(b.mutex());
    b.db().execute("UPDATE agente_posiciones SET stop=?, orden_stop=? WHERE simbolo=?", {stop, o["id"], sym});
  }
  double prev = jnum(p.value("stop", json(0)));
  if (prev && stop > prev)
    diario("stop", sym, "Sube el stop de " + g4(prev) + " a " + g4(stop) + " para asegurar beneficio.", {{"orden", o["id"]}});
}

std::vector<std::string> AgenteBolsa::abrir(long long cid, const json& analisis, const json& c) {
  json res = b.resumen(cid);
  double total = jnum(res["total_eur"]), libre = jnum(res["efectivo_eur"]);
  double reservado = jnum((*b.db().one("SELECT COALESCE(SUM(estimado_eur),0) e FROM ordenes WHERE cartera=? AND "
                                       "lado='compra' AND estado='pendiente'",
                                       {cid}))["e"]);
  libre -= reservado + total * jnum(c["reserva_pct"]) / 100;
  std::set<std::string> abiertas;
  for (auto& p : res["posiciones"]) abiertas.insert(jstr(p, "simbolo"));
  for (auto& r : b.db().query("SELECT simbolo FROM agente_posiciones")) abiertas.insert(jstr(r, "simbolo"));
  long huecos = (long)jnum(c["max_posiciones"]) - (long)abiertas.size();
  json vend = get("vendidos", json::object());
  std::set<std::string> recientes;
  if (vend.is_object())
    for (auto it = vend.begin(); it != vend.end(); ++it)
      if (now() - jnum(it.value()) < ESPERA_RECOMPRA_S) recientes.insert(it.key());
  std::vector<std::pair<std::string, json>> cands;
  for (auto it = analisis.begin(); it != analisis.end(); ++it) {
    const json& a = it.value();
    if (abiertas.count(it.key()) || recientes.count(it.key()) || !truthy(a["abierto"]) ||
        jnum(a["puntuacion"]) < UMBRAL_COMPRA || (a["rsi"].is_number() && jnum(a["rsi"]) > RSI_MAX_COMPRA))
      continue;
    cands.push_back({it.key(), a});
  }
  std::stable_sort(cands.begin(), cands.end(),
                   [](auto& x, auto& y) { return jnum(x.second["puntuacion"]) > jnum(y.second["puntuacion"]); });
  std::vector<std::string> hechos;
  for (size_t i = 0; i < cands.size() && (long)i < std::max(0L, huecos); ++i) {
    auto& [sym, an] = cands[i];
    json inst = b.instrumento(sym);
    double fx = b.fx(jstr(inst, "divisa"));
    double atr_v = jnum(an["atr"]);
    double px_eur = jnum(an["precio"]) / fx;
    double riesgo_eur = total * jnum(c["riesgo_pct"]) / 100;
    double dist_eur = std::max(2 * atr_v / fx, px_eur * 0.01);
    double qty = std::min({riesgo_eur / dist_eur, total * jnum(c["max_peso_pct"]) / 100 / px_eur, libre * 0.97 / px_eur});
    qty = jstr(inst, "mercado") == "CRIPTO" ? std::floor(qty * 1e4) / 1e4 : std::floor(qty);
    if (qty <= 0 || qty * px_eur < 150) continue;   // operaciones minusculas: la comision se come el beneficio
    json o;
    try {
      o = b.ordenar(sym, "compra", qty, "mercado", nullptr, nullptr, "dia", cid, ORIGEN_AGENTE);
    } catch (const std::invalid_argument& e) {
      diario("espera", sym, std::string("No compra: ") + e.what());
      continue;
    }
    double entrada = truthy(o.value("precio_ejec", json())) ? jnum(o["precio_ejec"]) : jnum(an["precio"]);
    double stop = entrada - 2 * atr_v, objetivo = entrada + 4 * atr_v;
    {
      std::lock_guard<std::recursive_mutex> lk(b.mutex());
      b.db().execute("INSERT OR REPLACE INTO agente_posiciones VALUES (?,?,?,?,?,?,?,?,?)",
                     {sym, entrada, stop, objetivo, atr_v, nullptr, o["id"], now(), jdump(an["factores"])});
    }
    libre -= qty * px_eur;
    std::string motivo = "Puntuación " + fmt("%.2f", jnum(an["puntuacion"]));
    if (an["rsi"].is_number())
      motivo += std::string(": precio ") + (jnum(an["factores"]["tendencia"]) > 0 ? "sobre" : "bajo") +
                " su media de 50, momento " + fmt("%+.1f", jnum(an["roc20"]) * 100) + " % en 20 sesiones, RSI " +
                fmt("%.0f", jnum(an["rsi"]));
    if (jstr(o, "estado") == "ejecutada") {
      diario("compra", sym,
             "Compra " + gg(qty) + " a " + g4(entrada) + ". " + motivo + ". Stop " + g4(stop) + ", objetivo " + g4(objetivo) + ".",
             {{"orden", o["id"]}});
      poner_stop(cid, sym, qty, stop, {{"stop", 0}});
    } else {
      diario("compra", sym,
             "Orden de compra #" + std::to_string(o["id"].get<long long>()) + " de " + gg(qty) + " en espera. " + motivo + ".",
             {{"orden", o["id"]}});
    }
    hechos.push_back("compra " + sym);
  }
  if (hechos.empty() && cands.empty() && !analisis.empty()) {
    std::string best;
    double bs = -1e300;
    bool best_open = false;
    for (auto it = analisis.begin(); it != analisis.end(); ++it)
      if (jnum(it.value()["puntuacion"]) > bs) {
        bs = jnum(it.value()["puntuacion"]);
        best = it.key();
        best_open = truthy(it.value()["abierto"]);
      }
    if (now() - jnum(get("ultima_espera", 0)) > 1800) {
      std::string txt = bs >= UMBRAL_COMPRA && !best_open
                            ? "Mercado cerrado: " + best + " tiene la mejor puntuación (" + fmt("%.2f", bs) +
                                  "); esperará a la apertura para decidir con precios del día."
                            : "Sin oportunidades claras: el mejor es " + best + " con " + fmt("%.2f", bs) +
                                  " (compra desde " + gg(UMBRAL_COMPRA) + ").";
      diario("espera", "", txt);
      put("ultima_espera", now());
    }
  }
  return hechos;
}

json AgenteBolsa::estado() {
  long long cid = cartera();
  json c = cfg();
  json res = b.resumen(cid);
  json dia = json::array();
  for (auto& d : b.db().query("SELECT * FROM agente_diario ORDER BY id DESC LIMIT 60")) {
    json r = d;
    json x = jparse(r["datos"].is_string() ? r["datos"].get<std::string>() : "{}");
    r["datos"] = x.is_discarded() || x.is_null() ? json::object() : x;
    dia.push_back(r);
  }
  auto cur = b.db().query("SELECT ts, total_eur FROM agente_valor ORDER BY ts DESC LIMIT 1500");
  json curva = json::array();
  for (auto it = cur.rbegin(); it != cur.rend(); ++it) curva.push_back(*it);
  std::map<std::string, json> gest;
  for (auto& p : b.db().query("SELECT simbolo, entrada, stop, objetivo, orden_stop FROM agente_posiciones"))
    gest[jstr(p, "simbolo")] = p;
  for (auto& p : res["posiciones"]) {
    auto it = gest.find(jstr(p, "simbolo"));
    if (it == gest.end()) continue;
    for (auto kv = it->second.begin(); kv != it->second.end(); ++kv)
      if (kv.key() != "simbolo") p[kv.key()] = kv.value();
  }
  json st = get("estadisticas");
  if (!st.is_object()) st = {{"cerradas", 0}, {"ganadoras", 0}, {"resultado_eur", 0.0}};
  json rk = get("ranking");
  return {{"activo", truthy(c["activo"]) && vivo()},
          {"configurado", truthy(c["activo"])},
          {"ajustes", c},
          {"cartera", cid},
          {"resumen", res},
          {"diario", dia},
          {"curva", curva},
          {"pesos", pesos()},
          {"estadisticas", st},
          {"ranking", rk.is_array() ? rk : json::array()},
          {"ultimo_ciclo", ultimo_ciclo},
          {"error", ultimo_error},
          {"datos", b.datos ? b.datos->estado() : json{{"en_linea", false}, {"fuentes", json::object()}}},
          {"aviso", AVISO}};
}

// ---------------------------------------------------------------- hilo
bool AgenteBolsa::wait(double secs) {
  std::unique_lock<std::mutex> lk(wmu_);
  return cv_.wait_for(lk, std::chrono::milliseconds((long long)(secs * 1000)), [&] { return stop_.load(); });
}

void AgenteBolsa::start() {
  if (vivo()) return;
  if (th_.joinable()) th_.join();
  stop_ = false;
  running_ = true;
  th_ = std::thread([this] { run(); });
}

void AgenteBolsa::stop() {
  {
    std::lock_guard<std::mutex> lk(wmu_);
    stop_ = true;
  }
  cv_.notify_all();
}

void AgenteBolsa::run() {
  if (!wait(5)) {   // deja arrancar el servidor
    while (!stop_) {
      if (!truthy(cfg()["activo"])) break;
      try {
        ciclo();
        ultimo_error.clear();
      } catch (const std::exception& e) {   // el agente nunca tumba el servidor
        ultimo_error = e.what();
        L.warning(std::string("Agente de bolsa: ciclo fallido: ") + e.what());
      }
      if (wait(std::max(15.0, jnum(cfg()["intervalo_s"], 60)))) break;
    }
  }
  running_ = false;
}

namespace {
std::mutex g_amu;
std::map<Bolsa*, std::unique_ptr<AgenteBolsa>> g_agentes;
}  // namespace

AgenteBolsa& agente() {
  Bolsa& mesa = get();
  std::lock_guard<std::mutex> lk(g_amu);
  auto& a = g_agentes[&mesa];
  if (!a) a = std::make_unique<AgenteBolsa>(mesa, [] {
    json j = get_config().get("bolsa.agente", json::object());
    return j.is_object() ? j : json::object();
  });
  return *a;
}

void reset_agentes() {
  std::lock_guard<std::mutex> lk(g_amu);
  g_agentes.clear();   // el destructor para y espera cada hilo
}

}  // namespace nx::bolsa
