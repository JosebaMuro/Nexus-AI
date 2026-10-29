// Mesa de bolsa simulada (nexus/office/bolsa.py). Mismo esquema, mismas reglas y mismos JSON que la edicion Python.
#include "office/bolsa.h"
#include "office/bolsa_agente.h"

#include <chrono>
#include <cstdio>
#include <cmath>
#include <set>

#include "core/config.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::bolsa {

namespace {

auto L = log::get("bolsa");
namespace ch = std::chrono;

struct Mercado {
  const char* id;
  const char* nombre;
  const char* zona;
  const char* abre;
  const char* cierra;
  const char* festivos;
  bool nacional = false, continuo = false;
};

const std::vector<Mercado> MERCADOS = {
    {"BME", "Bolsa de Madrid (BME)", "eu", "09:00", "17:30", "eu", true, false},
    {"XETRA", "Xetra (Fráncfort)", "eu", "09:00", "17:30", "eu"},
    {"EURONEXT", "Euronext (París, Ámsterdam)", "eu", "09:00", "17:30", "eu"},
    {"LSE", "Bolsa de Londres (LSE)", "uk", "08:00", "16:30", "uk"},
    {"NYSE", "NYSE (Nueva York)", "us", "09:30", "16:00", "us"},
    {"NASDAQ", "Nasdaq (Nueva York)", "us", "09:30", "16:00", "us"},
    {"CRIPTO", "Criptoactivos (24/7)", "utc", "00:00", "24:00", "", false, true},
};

const Mercado* find_market(const std::string& id) {
  for (auto& m : MERCADOS)
    if (id == m.id) return &m;
  return nullptr;
}
const Mercado& M(const std::string& id) {
  auto* m = find_market(id);
  if (!m) throw std::invalid_argument("mercado desconocido: " + id);
  return *m;
}

struct Inst {
  const char *sym, *name, *market, *cur, *type;
  double ref, vol, spread, adv, tax;
};
const std::vector<Inst> CATALOGO = {
    {"SAN.MC", "Banco Santander", "BME", "EUR", "acción", 6.0, 0.28, 3, 4.0e7, 0.2},
    {"BBVA.MC", "BBVA", "BME", "EUR", "acción", 10.5, 0.27, 3, 1.5e7, 0.2},
    {"ITX.MC", "Inditex", "BME", "EUR", "acción", 48.0, 0.24, 3, 2.5e6, 0.2},
    {"IBE.MC", "Iberdrola", "BME", "EUR", "acción", 14.0, 0.18, 3, 1.2e7, 0.2},
    {"TEF.MC", "Telefónica", "BME", "EUR", "acción", 4.2, 0.22, 4, 1.6e7, 0.2},
    {"REP.MC", "Repsol", "BME", "EUR", "acción", 13.0, 0.26, 4, 5.0e6, 0.2},
    {"SAP.DE", "SAP", "XETRA", "EUR", "acción", 230.0, 0.26, 3, 1.5e6, 0.0},
    {"SIE.DE", "Siemens", "XETRA", "EUR", "acción", 190.0, 0.24, 3, 1.2e6, 0.0},
    {"MC.PA", "LVMH", "EURONEXT", "EUR", "acción", 600.0, 0.27, 3, 5.0e5, 0.3},
    {"ASML.AS", "ASML", "EURONEXT", "EUR", "acción", 650.0, 0.38, 3, 6.0e5, 0.0},
    {"SHEL.L", "Shell", "LSE", "GBP", "acción", 27.0, 0.24, 4, 6.0e6, 0.0},
    {"AAPL", "Apple", "NASDAQ", "USD", "acción", 220.0, 0.27, 1, 5.0e7, 0.0},
    {"MSFT", "Microsoft", "NASDAQ", "USD", "acción", 420.0, 0.25, 1, 2.0e7, 0.0},
    {"NVDA", "NVIDIA", "NASDAQ", "USD", "acción", 130.0, 0.50, 1, 2.0e8, 0.0},
    {"AMZN", "Amazon", "NASDAQ", "USD", "acción", 190.0, 0.32, 1, 4.0e7, 0.0},
    {"KO", "Coca-Cola", "NYSE", "USD", "acción", 65.0, 0.15, 2, 1.2e7, 0.0},
    {"JNJ", "Johnson & Johnson", "NYSE", "USD", "acción", 155.0, 0.16, 2, 7.0e6, 0.0},
    {"VWCE.DE", "ETF Vanguard FTSE All-World (acumulación)", "XETRA", "EUR", "fondo cotizado (ETF)", 120.0, 0.15, 6,
     3.0e5, 0.0},
    {"IWDA.AS", "ETF iShares Core MSCI World (acumulación)", "EURONEXT", "EUR", "fondo cotizado (ETF)", 95.0, 0.15, 5,
     5.0e5, 0.0},
    {"IBGL.AS", "ETF iShares bonos del Estado zona euro 15-30 años", "EURONEXT", "EUR", "fondo cotizado (ETF)", 160.0,
     0.14, 10, 5.0e4, 0.0},
    {"BTC-EUR", "Bitcoin", "CRIPTO", "EUR", "criptoactivo", 60000.0, 0.60, 8, 2.0e4, 0.0},
    {"ETH-EUR", "Ether", "CRIPTO", "EUR", "criptoactivo", 2500.0, 0.75, 10, 3.0e5, 0.0},
};
const std::vector<std::pair<std::string, double>> FX_REF = {{"EUR", 1.0}, {"USD", 1.10}, {"GBP", 0.85}, {"CHF", 0.95}};
constexpr double FX_VOL = 0.07;

double fx_ref(const std::string& d) {
  for (auto& [k, v] : FX_REF)
    if (k == d) return v;
  return 0;
}

struct Tramo {
  double fijo, pct, minimo;
};
struct Tarifa {
  const char* id;
  const char* nombre;
  Tramo nacional, internacional, cripto;
  double cambio_pct;
};
const std::vector<Tarifa> TARIFAS = {
    {"broker-online", "Bróker online (tarifa típica)", {2.0, 0.0004, 2.0}, {4.0, 0.0005, 4.0}, {0.0, 0.005, 0.5}, 0.0025},
    {"banco", "Banco tradicional (tarifa típica)", {0.0, 0.0025, 8.0}, {0.0, 0.004, 20.0}, {0.0, 0.015, 2.0}, 0.006},
    {"neobroker", "Neobróker sin comisión (cobra en el cambio y la horquilla)", {1.0, 0.0, 1.0}, {1.0, 0.0, 1.0},
     {0.0, 0.01, 1.0}, 0.0015},
};
const Tarifa* find_tarifa(const std::string& id) {
  for (auto& t : TARIFAS)
    if (id == t.id) return &t;
  return nullptr;
}
constexpr double CANON_FIJO = 1.10, CANON_PCT = 0.000024;
constexpr int GTC_DAYS = 90;

const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS carteras (id INTEGER PRIMARY KEY, nombre TEXT, modo TEXT, tarifa TEXT,
    capital_inicial REAL, efectivo REAL, creada TEXT);
CREATE TABLE IF NOT EXISTS instrumentos (simbolo TEXT PRIMARY KEY, nombre TEXT, mercado TEXT, divisa TEXT, tipo TEXT,
    precio_ref REAL, vol REAL, spread_bps REAL, adv REAL, impuesto_pct REAL, propio INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS precios (simbolo TEXT PRIMARY KEY, precio REAL, ts REAL, cierre_ant REAL, apertura REAL);
CREATE TABLE IF NOT EXISTS ticks (simbolo TEXT, ts REAL, precio REAL);
CREATE INDEX IF NOT EXISTS ticks_s ON ticks(simbolo, ts);
CREATE TABLE IF NOT EXISTS historico (simbolo TEXT, fecha TEXT, cierre REAL, PRIMARY KEY (simbolo, fecha));
CREATE TABLE IF NOT EXISTS fx (divisa TEXT PRIMARY KEY, tasa REAL, ts REAL);
CREATE TABLE IF NOT EXISTS ordenes (id INTEGER PRIMARY KEY, cartera INTEGER, simbolo TEXT, lado TEXT, tipo TEXT,
    cantidad REAL, limite REAL, stop REAL, validez TEXT, estado TEXT, creada REAL, expira REAL, activada INTEGER DEFAULT 0,
    motivo TEXT DEFAULT '', precio_ejec REAL, ejecutada REAL, origen TEXT DEFAULT '', estimado_eur REAL DEFAULT 0);
CREATE TABLE IF NOT EXISTS operaciones (id INTEGER PRIMARY KEY, cartera INTEGER, orden INTEGER, simbolo TEXT,
    lado TEXT, cantidad REAL, precio REAL, fx REAL, efectivo_eur REAL, comision_eur REAL, impuestos_eur REAL,
    cambio_eur REAL, deslizamiento_eur REAL, total_eur REAL, ganancia_eur REAL, aviso TEXT DEFAULT '', ts REAL);
CREATE TABLE IF NOT EXISTS lotes (id INTEGER PRIMARY KEY, cartera INTEGER, simbolo TEXT, cantidad REAL,
    coste_unit_eur REAL, ts REAL);
)";

// ---------------------------------------------------------------- calendario
long days(int y, unsigned m, unsigned d) {
  return ch::sys_days(ch::year_month_day(ch::year(y), ch::month(m), ch::day(d))).time_since_epoch().count();
}
ch::year_month_day civil(long day) { return ch::year_month_day(ch::sys_days(ch::days(day))); }
int year_of(long day) { return (int)civil(day).year(); }
int pyweekday(long day) { return (int)ch::weekday(ch::sys_days(ch::days(day))).iso_encoding() - 1; }   // 0 = lunes
long day_of(double t) { return (long)std::floor(t / 86400.0); }

long nth_weekday(int y, unsigned m, int wd, int n) {
  if (n > 0) {
    long d = days(y, m, 1);
    d += ((wd - pyweekday(d)) % 7 + 7) % 7;
    return d + 7L * (n - 1);
  }
  long d = (m < 12 ? days(y, m + 1, 1) : days(y + 1, 1, 1)) - 1;
  return d - ((pyweekday(d) - wd) % 7 + 7) % 7;
}

long observed(long d) {
  int w = pyweekday(d);
  return w == 5 ? d - 1 : w == 6 ? d + 1 : d;
}

double hm(const std::string& s) {
  auto p = split(s, ":");
  return std::stoi(p[0]) * 3600.0 + std::stoi(p[1]) * 60.0;
}

double local_t(const std::string& market, double t) { return t + utc_offset(M(market).zona, t); }
long local_day(const std::string& market, double t) { return day_of(local_t(market, t)); }

// (apertura, cierre) en UTC de la sesion del dia local, o nada si ese dia no abre
std::optional<std::pair<double, double>> session(const std::string& market, long day) {
  auto& m = M(market);
  if (m.continuo) return std::make_pair(day * 86400.0, (day + 1) * 86400.0);
  if (pyweekday(day) >= 5) return std::nullopt;
  auto hol = holidays(m.festivos, year_of(day));
  if (std::find(hol.begin(), hol.end(), day) != hol.end()) return std::nullopt;
  double noon = day * 86400.0 + 12 * 3600;
  double base = day * 86400.0 - utc_offset(m.zona, noon);
  return std::make_pair(base + hm(m.abre), base + hm(m.cierra));
}

std::pair<double, double> next_session(const std::string& market, double t) {
  long day = local_day(market, t);
  for (int i = 0; i < 15; ++i, ++day) {
    auto s = session(market, day);
    if (s && t < s->second) return *s;
  }
  throw std::runtime_error("sin sesiones en " + market);
}

std::pair<double, int> trading_time(const std::string& market, double t0, double t1) {
  if (t1 <= t0) return {0.0, 0};
  if (M(market).continuo) return {t1 - t0, 0};
  t0 = std::max(t0, t1 - 370 * 86400.0);
  double secs = 0;
  int opens = 0;
  long day = local_day(market, t0) - 1, end = local_day(market, t1) + 1;
  for (; day <= end; ++day) {
    auto s = session(market, day);
    if (!s) continue;
    double lo = std::max(s->first, t0), hi = std::min(s->second, t1);
    if (hi > lo) secs += hi - lo;
    if (t0 < s->first && s->first <= t1) ++opens;
  }
  return {secs, opens};
}

double r2(double x) { return round_to(x + 0.0, 2); }
double rnd(double x, int d) { return round_to(x, d); }

const char* DIAS[] = {"lun", "mar", "mié", "jue", "vie", "sáb", "dom"};

std::string two(long v) {
  std::string s = std::to_string(v);
  return s.size() < 2 ? "0" + s : s;
}

double fget(const json& r, const char* k) { return jnum(r[k]); }

std::string fnum_fixed(double v, int d) {   // f"{v:.2f}"
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", d, v);
  return buf;
}

}  // namespace

const char* AVISO =
    "Simulación: nunca se usa dinero ni se envían órdenes reales. Con conexión, los precios son cotizaciones reales "
    "(Yahoo Finance, con el retraso de cada bolsa); sin conexión siguen simulados desde el último precio real. "
    "No es asesoramiento financiero. Invertir conlleva riesgo de perder dinero; antes de operar con dinero real "
    "consulta a un asesor registrado en la CNMV.";
const char* CONFIRMACION = "CONFIRMO";
const std::vector<std::string> ORDER_TYPES = {"mercado", "limitada", "stop", "stop_limitada"};

long easter(int y) {
  int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
  int h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
  int m = (a + 11 * h + 22 * l) / 451, month = (h + l - 7 * m + 114) / 31;
  return days(y, month, (h + l - 7 * m + 114) % 31 + 1);
}

std::vector<long> holidays(const std::string& kind, int y) {
  long e = easter(y);
  std::set<long> out;
  if (kind == "eu") {
    out = {days(y, 1, 1), e - 2, e + 1, days(y, 5, 1), days(y, 12, 24), days(y, 12, 25), days(y, 12, 26), days(y, 12, 31)};
  } else if (kind == "uk") {
    out = {e - 2, e + 1, nth_weekday(y, 5, 0, 1), nth_weekday(y, 5, 0, -1), nth_weekday(y, 8, 0, -1)};
    long ny = days(y, 1, 1);
    int w = pyweekday(ny);
    out.insert(ny + (w == 5 ? 2 : w == 6 ? 1 : 0));
    long xmas = days(y, 12, 25), box = days(y, 12, 26);
    if (pyweekday(xmas) == 5) {
      out.insert(days(y, 12, 27));
      out.insert(days(y, 12, 28));
    } else if (pyweekday(xmas) == 6) {
      out.insert(box);
      out.insert(days(y, 12, 27));
    } else {
      out.insert(xmas);
      out.insert(pyweekday(box) == 5 ? box + 2 : box);
    }
  } else if (kind == "us") {
    out = {nth_weekday(y, 1, 0, 3),   nth_weekday(y, 2, 0, 3),        e - 2, nth_weekday(y, 5, 0, -1),
           observed(days(y, 6, 19)), observed(days(y, 7, 4)),        nth_weekday(y, 9, 0, 1),
           nth_weekday(y, 11, 3, 4), observed(days(y, 12, 25))};
    long ny = days(y, 1, 1);
    if (pyweekday(ny) != 5) out.insert(observed(ny));
  }
  return {out.begin(), out.end()};
}

double utc_offset(const std::string& zona, double t) {
  int y = year_of(day_of(t));
  if (zona == "eu" || zona == "uk") {
    double start = nth_weekday(y, 3, 6, -1) * 86400.0 + 3600, end = nth_weekday(y, 10, 6, -1) * 86400.0 + 3600;
    bool summer = start <= t && t < end;
    return 3600.0 * (zona == "eu" ? (summer ? 2 : 1) : (summer ? 1 : 0));
  }
  if (zona == "us") {
    double start = nth_weekday(y, 3, 6, 2) * 86400.0 + 7 * 3600, end = nth_weekday(y, 11, 6, 1) * 86400.0 + 6 * 3600;
    return 3600.0 * (start <= t && t < end ? -4 : -5);
  }
  return 0;
}

bool is_open(const std::string& market, double t) {
  auto s = session(market, local_day(market, t));
  return s && s->first <= t && t < s->second;
}

std::string fmt_local(const std::string& market, double t) {
  double lt = local_t(market, t);
  long d = day_of(lt);
  auto c = civil(d);
  long sec = (long)std::floor(lt - d * 86400.0);
  return std::string(DIAS[pyweekday(d)]) + " " + two((unsigned)c.day()) + "/" + two((unsigned)c.month()) + " " +
         two(sec / 3600) + ":" + two(sec / 60 % 60);
}

std::string ymd(long day) {
  auto c = civil(day);
  return std::to_string((int)c.year()) + "-" + two((unsigned)c.month()) + "-" + two((unsigned)c.day());
}

std::string iso_utc(double t) {
  long d = day_of(t);
  long sec = (long)std::floor(t - d * 86400.0);
  return ymd(d) + "T" + two(sec / 3600) + ":" + two(sec / 60 % 60) + ":" + two(sec % 60) + "+00:00";
}

json tarifas_json() {
  json o = json::object();
  for (auto& t : TARIFAS) o[t.id] = t.nombre;
  return o;
}

double num(const json& x) {
  if (x.is_number()) return x.get<double>();
  if (x.is_boolean()) return x.get<bool>() ? 1 : 0;
  std::string s = x.is_string() ? x.get<std::string>() : jdump(x);
  s = replace_all(replace_all(trim(s), "€", ""), " ", "");
  if (contains(s, ",") && contains(s, ".")) {
    if (s.rfind(',') > s.rfind('.'))
      s = replace_all(replace_all(s, ".", ""), ",", ".");
    else
      s = replace_all(s, ",", "");
  }
  s = replace_all(s, ",", ".");
  size_t used = 0;
  double v;
  try {
    v = std::stod(s, &used);
  } catch (...) {
    throw std::invalid_argument("could not convert string to float: '" + s + "'");
  }
  if (used != s.size()) throw std::invalid_argument("could not convert string to float: '" + s + "'");
  return v;
}

// ---------------------------------------------------------------- Bolsa
Bolsa::Bolsa(const fs::path& path, std::optional<uint64_t> seed, std::function<double()> clock)
    : db_(path.empty() ? paths::sub({"bolsa"}) / "bolsa.db" : path),
      rng_(seed ? *seed : std::random_device{}()),
      clock_(clock ? std::move(clock) : [] { return nx::now(); }) {
  db_.script(SCHEMA);
  db_.transaction([&] {
    for (auto& i : CATALOGO)
      db_.execute("INSERT OR IGNORE INTO instrumentos (simbolo, nombre, mercado, divisa, tipo, precio_ref, vol, "
                  "spread_bps, adv, impuesto_pct) VALUES (?,?,?,?,?,?,?,?,?,?)",
                  {i.sym, i.name, i.market, i.cur, i.type, i.ref, i.vol, i.spread, i.adv, i.tax / 100});
  });
}

double Bolsa::gauss(double mu, double sigma) {
  std::normal_distribution<double> n(mu, sigma);
  return n(rng_);
}

json Bolsa::mercados() {
  double t = now();
  json out = json::array();
  for (auto& m : MERCADOS) {
    auto s = next_session(m.id, t);
    bool op = is_open(m.id, t);
    json o = {{"id", m.id},
              {"nombre", m.nombre},
              {"abierto", op},
              {"hora_local", fmt_local(m.id, t)},
              {"horario", m.continuo ? std::string("24 horas, todos los días") : std::string(m.abre) + "-" + m.cierra}};
    o[op ? "cierra" : "abre"] = fmt_local(m.id, op ? s.second : s.first);
    out.push_back(o);
  }
  return out;
}

json Bolsa::instrumentos(const std::string& texto) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto rows = db_.query("SELECT * FROM instrumentos ORDER BY mercado, simbolo");
  json out = json::array();
  std::string t = lower(texto);
  for (auto& r : rows) {
    if (!texto.empty() && !(contains(lower(jstr(r, "simbolo")), t) || contains(lower(jstr(r, "nombre")), t) ||
                            contains(lower(jstr(r, "tipo")), t) || t == lower(jstr(r, "mercado"))))
      continue;
    out.push_back(r);
  }
  return out;
}

json Bolsa::instrumento(const std::string& simbolo) {
  std::string s = upper(trim(simbolo));
  auto r = db_.one("SELECT * FROM instrumentos WHERE simbolo=?", {s});
  if (!r) {
    std::vector<std::string> near;
    for (auto& x : instrumentos(simbolo)) {
      if (near.size() >= 8) break;
      near.push_back(jstr(x, "simbolo"));
    }
    throw std::invalid_argument("no conozco el instrumento " + simbolo +
                                (near.empty() ? ". Añádelo en la mesa de bolsa con su precio de referencia o importa sus cierres."
                                              : "; ¿quizá " + join(near, ", ") + "?"));
  }
  return *r;
}

json Bolsa::anadir_instrumento(const std::string& simbolo, const std::string& nombre, const std::string& mercado_in,
                               const json& precio_in, const std::string& divisa_in, const std::string& tipo,
                               const json& vol_in, const json& spread_bps, const json& adv, const json& impuesto_pct) {
  std::string mercado = upper(mercado_in);
  auto* m = find_market(mercado);
  if (!m) {
    std::vector<std::string> ids;
    for (auto& x : MERCADOS) ids.push_back(x.id);
    throw std::invalid_argument("mercado desconocido: " + mercado + ". Disponibles: " + join(ids, ", "));
  }
  std::string zona = m->zona;
  std::string divisa = upper(!divisa_in.empty() ? divisa_in
                                                : (zona == "eu" || zona == "utc") ? "EUR"
                                                : mercado == "LSE"              ? "GBP"
                                                                                : "USD");
  if (fx_ref(divisa) == 0) throw std::invalid_argument("divisa no soportada: " + divisa + ". Disponibles: EUR, USD, GBP, CHF");
  double precio = num(precio_in), vol = num(vol_in);
  if (precio <= 0 || !(0 < vol && vol < 5))
    throw std::invalid_argument("el precio debe ser positivo y la volatilidad anual entre 0 y 5 (0,25 = 25 %)");
  std::string sym = upper(trim(simbolo));
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    db_.transaction([&] {
      db_.execute("INSERT OR REPLACE INTO instrumentos VALUES (?,?,?,?,?,?,?,?,?,?,1)",
                  {sym, nombre, mercado, divisa, tipo, precio, vol, num(spread_bps), num(adv), num(impuesto_pct) / 100});
      db_.execute("DELETE FROM precios WHERE simbolo=?", {sym});
    });
  }
  return instrumento(sym);
}

double Bolsa::step(const json& inst, double price, double t0, double t1) {
  std::string mk = jstr(inst, "mercado");
  auto [secs, opens] = trading_time(mk, t0, t1);
  if (!secs && !opens) return price;
  auto& m = M(mk);
  double year_secs = m.continuo ? 365 * 86400.0 : 252 * (hm(m.cierra) - hm(m.abre));
  double vol = fget(inst, "vol"), mu = m.continuo ? 0.0 : 0.05;
  double tau = secs / year_secs;
  double logret = (mu - vol * vol / 2) * tau + vol * std::sqrt(tau) * gauss(0, 1);
  for (int i = 0; i < std::min(opens, 60); ++i) logret += gauss(0, vol / std::sqrt(252.0) * 0.35);
  return std::max(price * std::exp(std::max(-0.5, std::min(0.5, logret))), 1e-6);
}

json Bolsa::precio(const std::string& simbolo) {
  json inst = instrumento(simbolo);
  std::string sym = jstr(inst, "simbolo"), mk = jstr(inst, "mercado");
  double t = now();
  json real = real_quote(sym);
  bool is_real = real.is_object();
  json st;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    db_.transaction([&] {
      auto cur = db_.one("SELECT * FROM precios WHERE simbolo=?", {sym});
      if (is_real) {
        double rp = jnum(real["precio"]), rc = jnum(real["cierre_anterior"]);
        json ap = cur ? (*cur)["apertura"] : json();
        st = {{"precio", rp}, {"ts", t}, {"cierre_ant", rc ? rc : rp}, {"apertura", truthy(ap) ? ap : json(rp)}};
      } else if (!cur) {
        auto h = db_.one("SELECT fecha, cierre FROM historico WHERE simbolo=? ORDER BY fecha DESC LIMIT 1", {sym});
        double p0 = h ? fget(*h, "cierre") : fget(inst, "precio_ref");
        st = {{"precio", p0}, {"ts", t}, {"cierre_ant", p0}, {"apertura", p0}};
      } else {
        st = *cur;
        double t0 = fget(st, "ts");
        double p = step(inst, fget(st, "precio"), t0, t);
        bool new_day = M(mk).continuo ? local_day(mk, t0) != local_day(mk, t) : trading_time(mk, t0, t).second > 0;
        if (new_day) {
          st["cierre_ant"] = st["precio"];
          st["apertura"] = p;
        }
        st["precio"] = p;
        st["ts"] = t;
      }
      db_.execute("INSERT OR REPLACE INTO precios VALUES (?,?,?,?,?)",
                  {sym, st["precio"], st["ts"], st["cierre_ant"], st["apertura"]});
      auto last = db_.one("SELECT ts FROM ticks WHERE simbolo=? ORDER BY ts DESC LIMIT 1", {sym});
      if (!last || fget(st, "ts") - fget(*last, "ts") >= (is_real ? 10 : 30)) {
        db_.execute("INSERT INTO ticks VALUES (?,?,?)", {sym, st["ts"], st["precio"]});
        db_.execute("DELETE FROM ticks WHERE simbolo=? AND ts < (SELECT ts FROM ticks WHERE simbolo=? "
                    "ORDER BY ts DESC LIMIT 1 OFFSET 499)",
                    {sym, sym});
      }
    });
  }
  double p = fget(st, "precio");
  double half = p * fget(inst, "spread_bps") / 2e4;
  int dec = p >= 10 ? 2 : p >= 1 ? 3 : 4;
  return {{"simbolo", sym},
          {"nombre", inst["nombre"]},
          {"mercado", mk},
          {"divisa", inst["divisa"]},
          {"tipo", inst["tipo"]},
          {"precio", rnd(p, dec)},
          {"compra", rnd(p - half, dec)},
          {"venta", rnd(p + half, dec)},
          {"variacion_pct", rnd((p / fget(st, "cierre_ant") - 1) * 100, 2)},
          {"cierre_anterior", rnd(fget(st, "cierre_ant"), dec)},
          {"abierto", is_open(mk, t)},
          {"hora", iso_utc(t)},
          {"fuente", is_real ? real["fuente"] : json("simulado")},
          {"retraso", is_real ? json(jstr(real, "retraso")) : json("")},
          {"real", is_real}};
}

json Bolsa::real_quote(const std::string& sym) {
  if (!datos) return nullptr;
  json q;
  try {
    if (!datos->tiene_mercado(sym)) datos->set_mercado(sym, jstr(instrumento(sym), "mercado"));
    q = datos->quote(sym);
  } catch (const std::exception& e) {   // sin conexion: sigue la simulacion
    L.debug("cotizacion real de " + sym + " no disponible: " + e.what());
    return nullptr;
  }
  if (!q.is_object() || !truthy(q.value("precio", json()))) return nullptr;
  std::string d = jstr(q, "divisa");
  if (d == "GBp" || d == "GBX") {   // Londres cotiza en peniques: el catalogo va en libras
    q["precio"] = jnum(q["precio"]) / 100;
    q["cierre_anterior"] = jnum(q.value("cierre_anterior", json(0))) / 100;
    if (q.contains("puntos") && q["puntos"].is_array())
      for (auto& p : q["puntos"]) p["precio"] = jnum(p["precio"]) / 100;
  }
  return q;
}

json Bolsa::serie(const std::string& simbolo, const std::string& rango) {
  json inst = instrumento(simbolo);
  std::string sym = jstr(inst, "simbolo");
  bool diario = rango == "6mo" || rango == "1y" || rango == "5y";
  json pts = json::array();
  if (datos) {
    try {
      pts = datos->serie(sym, rango);
    } catch (const std::exception&) {
      pts = json::array();
    }
    if (!pts.is_array()) pts = json::array();
    double ref = fget(inst, "precio_ref");
    if (!pts.empty() && jstr(inst, "divisa") == "GBP" && jnum(pts.back()["precio"]) > 50 * (ref ? ref : 1))
      for (auto& p : pts) p["precio"] = jnum(p["precio"]) / 100;
  }
  if (!pts.empty()) {
    if (diario) guardar_historico(sym, pts);
    return {{"simbolo", sym}, {"rango", rango}, {"real", true}, {"puntos", pts}};
  }
  if (diario) {
    auto rows = db_.query("SELECT fecha, cierre FROM historico WHERE simbolo=? ORDER BY fecha", {sym});
    for (auto& r : rows) {
      std::string f = jstr(r, "fecha");
      int y = 0;
      unsigned m = 0, d = 0;
      if (std::sscanf(f.c_str(), "%d-%u-%u", &y, &m, &d) != 3) continue;
      pts.push_back({{"ts", days(y, m, d) * 86400.0}, {"precio", r["cierre"]}});
    }
    if (!pts.empty()) return {{"simbolo", sym}, {"rango", rango}, {"real", true}, {"puntos", pts}, {"guardado", true}};
  }
  json out = json::array();
  for (auto& r : ticks(sym, 400)) out.push_back({{"ts", r["ts"]}, {"precio", r["precio"]}});
  return {{"simbolo", sym}, {"rango", rango}, {"real", false}, {"puntos", out}};
}

int Bolsa::guardar_historico(const std::string& sym, const json& pts) {
  std::vector<Params> rows;
  for (auto& p : pts) rows.push_back({sym, ymd(day_of(jnum(p["ts"]))), p["precio"]});
  std::lock_guard<std::recursive_mutex> lk(mu_);
  db_.executemany("INSERT OR REPLACE INTO historico VALUES (?,?,?)", rows);
  return (int)rows.size();
}

json Bolsa::ticks(const std::string& simbolo, int n) {
  std::string sym = jstr(instrumento(simbolo), "simbolo");
  auto rows = db_.query("SELECT ts, precio FROM ticks WHERE simbolo=? ORDER BY ts DESC LIMIT ?", {sym, n});
  json out = json::array();
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back(*it);
  return out;
}

double Bolsa::fx(const std::string& divisa) {
  if (divisa == "EUR") return 1.0;
  double t = now();
  double real = 0;
  if (datos) {
    try {
      real = datos->fx(divisa);
    } catch (const std::exception&) {
      real = 0;
    }
  }
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto r = db_.one("SELECT * FROM fx WHERE divisa=?", {divisa});
  double rate = r ? fget(*r, "tasa") : fx_ref(divisa);
  if (real > 0) {
    rate = real;
  } else if (r) {
    double tau = std::max(0.0, t - fget(*r, "ts")) / (365 * 86400.0);
    rate *= std::exp(FX_VOL * std::sqrt(std::min(tau, 1.0)) * gauss(0, 1));
  }
  db_.execute("INSERT OR REPLACE INTO fx VALUES (?,?,?)", {divisa, rate, t});
  return rate;
}

json Bolsa::importar_csv(const std::string& simbolo, const std::string& texto) {
  json inst = instrumento(simbolo);
  std::string sym = jstr(inst, "simbolo");
  std::vector<std::string> lines;
  for (auto& ln : splitlines(texto))
    if (!trim(ln).empty()) lines.push_back(ln);
  auto any_has = [&](const char* c) {
    for (size_t i = 0; i < lines.size() && i < 5; ++i)
      if (contains(lines[i], c)) return true;
    return false;
  };
  std::string sep = any_has(";") ? ";" : any_has("\t") ? "\t" : ",";
  int ci = -1;
  if (!lines.empty()) {
    auto header = split(lines[0], sep);
    for (size_t i = 0; i < header.size(); ++i) {
      std::string h = lower(trim(header[i]));
      if (h == "close" || h == "cierre" || h == "adj close" || h == "último" || h == "ultimo" || h == "precio") {
        ci = (int)i;
        break;
      }
    }
  }
  static const Rx DATE(R"((\d{4})-(\d{2})-(\d{2})|(\d{1,2})[/.-](\d{1,2})[/.-](\d{4}))");
  std::vector<std::pair<std::string, double>> rows;
  for (auto& ln : lines) {
    std::vector<std::string> parts;
    for (auto& p : split(ln, sep)) parts.push_back(trim(trim(p), "\""));
    auto m = DATE.match(parts[0]);
    if (!m || parts.size() < 2) continue;
    std::string fecha = !m->str(1).empty() ? m->str(1) + "-" + m->str(2) + "-" + m->str(3)
                                           : m->str(6) + "-" + two(std::stoi(m->str(5))) + "-" + two(std::stoi(m->str(4)));
    double close;
    try {
      close = num(ci >= 0 && ci < (int)parts.size() ? parts[ci] : parts[1]);
    } catch (...) {
      continue;
    }
    if (close > 0) rows.emplace_back(fecha, close);
  }
  if (rows.size() < 2) throw std::invalid_argument("no encuentro al menos dos filas «fecha;cierre» en el CSV");
  std::stable_sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
  std::vector<double> rets;
  for (size_t i = 1; i < rows.size(); ++i) rets.push_back(std::log(rows[i].second / rows[i - 1].second));
  std::optional<double> vol;
  if (rets.size() >= 10) {
    double mean = 0, ss = 0;
    for (double r : rets) mean += r;
    mean /= rets.size();
    for (double r : rets) ss += (r - mean) * (r - mean);
    vol = std::sqrt(ss / (rets.size() - 1) * 252);
  }
  double last = rows.back().second, prev = rows[rows.size() - 2].second;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    db_.transaction([&] {
      for (auto& [f, c] : rows) db_.execute("INSERT OR REPLACE INTO historico VALUES (?,?,?)", {sym, f, c});
      if (vol && 0.01 < *vol && *vol < 5) db_.execute("UPDATE instrumentos SET vol=? WHERE simbolo=?", {*vol, sym});
      db_.execute("INSERT OR REPLACE INTO precios VALUES (?,?,?,?,?)", {sym, last, now(), prev, last});
    });
  }
  return {{"simbolo", sym},
          {"filas", rows.size()},
          {"desde", rows.front().first},
          {"hasta", rows.back().first},
          {"ultimo_cierre", last},
          {"volatilidad_anual", vol ? json(rnd(*vol, 4)) : json(nullptr)}};
}

json Bolsa::carteras() {
  auto rows = db_.query("SELECT * FROM carteras ORDER BY id");
  if (rows.empty()) {
    crear_cartera();
    rows = db_.query("SELECT * FROM carteras ORDER BY id");
  }
  json out = json::array();
  for (auto& r : rows) out.push_back(r);
  return out;
}

json Bolsa::crear_cartera(const std::string& nombre, const json& capital_in, const std::string& tarifa,
                          const std::string& modo) {
  double capital = num(capital_in);
  if (!find_tarifa(tarifa))
    throw std::invalid_argument("tarifa desconocida: " + tarifa + ". Disponibles: broker-online, banco, neobroker");
  if (modo != "simulado" && modo != "real") throw std::invalid_argument("modo debe ser «simulado» o «real»");
  if (capital < 0 || capital > 1e9) throw std::invalid_argument("capital fuera de rango");
  long long id;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    db_.execute("INSERT INTO carteras (nombre, modo, tarifa, capital_inicial, efectivo, creada) VALUES (?,?,?,?,?,?)",
                {nombre, modo, tarifa, capital, capital, iso_utc(std::floor(now()))});
    id = db_.last_insert_id();
  }
  return cartera_info(id);
}

json Bolsa::cartera_info(const json& cid) {
  json rows = carteras();
  if (cid.is_null() || !truthy(cid)) return rows[0];
  long long id = (long long)num(cid);
  auto r = db_.one("SELECT * FROM carteras WHERE id=?", {id});
  if (!r) throw std::invalid_argument("no existe la cartera " + (cid.is_string() ? cid.get<std::string>() : jdump(cid)));
  return *r;
}

json Bolsa::reiniciar(const json& cid, const json& capital) {
  json c = cartera_info(cid);
  if (jstr(c, "modo") != "simulado") throw std::invalid_argument("solo se reinician carteras simuladas");
  double cap = capital.is_null() ? fget(c, "capital_inicial") : num(capital);
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    db_.transaction([&] {
      for (auto* tbl : {"ordenes", "operaciones", "lotes"})
        db_.execute(std::string("DELETE FROM ") + tbl + " WHERE cartera=?", {c["id"]});
      db_.execute("UPDATE carteras SET capital_inicial=?, efectivo=? WHERE id=?", {cap, cap, c["id"]});
    });
  }
  return cartera_info(c["id"]);
}

json Bolsa::posiciones(const json& cid) {
  json c = cartera_info(cid);
  json out = json::array();
  for (auto& r : db_.query("SELECT simbolo, SUM(cantidad) q, SUM(cantidad*coste_unit_eur) coste FROM lotes "
                           "WHERE cartera=? AND cantidad > 1e-12 GROUP BY simbolo ORDER BY simbolo",
                           {c["id"]})) {
    json q = precio(jstr(r, "simbolo"));
    double f = fx(jstr(q, "divisa"));
    double qty = fget(r, "q"), coste = fget(r, "coste");
    double valor = qty * fget(q, "compra") / f;
    out.push_back({{"simbolo", r["simbolo"]},
                   {"nombre", q["nombre"]},
                   {"cantidad", rnd(qty, 8)},
                   {"coste_medio_eur", rnd(coste / qty, 4)},
                   {"coste_eur", r2(coste)},
                   {"precio", q["precio"]},
                   {"divisa", q["divisa"]},
                   {"valor_eur", r2(valor)},
                   {"plusvalia_eur", r2(valor - coste)},
                   {"plusvalia_pct", coste ? rnd((valor / coste - 1) * 100, 2) : 0.0},
                   {"variacion_dia_pct", q["variacion_pct"]}});
  }
  return out;
}

json Bolsa::resumen(const json& cid) {
  procesar();
  json c = cartera_info(cid);
  json pos = posiciones(c["id"]);
  double invertido = 0;
  for (auto& p : pos) invertido += fget(p, "valor_eur");
  double total = fget(c, "efectivo") + invertido;
  auto real = *db_.one("SELECT COALESCE(SUM(ganancia_eur),0) g, COALESCE(SUM(comision_eur+impuestos_eur+cambio_eur),0) "
                       "gastos FROM operaciones WHERE cartera=?",
                       {c["id"]});
  for (auto& p : pos) p["peso_pct"] = total ? rnd(fget(p, "valor_eur") / total * 100, 2) : 0.0;
  double cap = fget(c, "capital_inicial");
  return {{"cartera", c["id"]},
          {"nombre", c["nombre"]},
          {"modo", c["modo"]},
          {"tarifa", c["tarifa"]},
          {"efectivo_eur", r2(fget(c, "efectivo"))},
          {"invertido_eur", r2(invertido)},
          {"total_eur", r2(total)},
          {"capital_inicial_eur", r2(cap)},
          {"rentabilidad_pct", cap ? rnd((total / cap - 1) * 100, 2) : 0.0},
          {"ganancia_realizada_eur", r2(fget(real, "g"))},
          {"gastos_eur", r2(fget(real, "gastos"))},
          {"posiciones", pos},
          {"ordenes_pendientes", ordenes(c["id"], true).size()},
          {"aviso", AVISO}};
}

json Bolsa::costes(const json& cid, const std::string& simbolo, const std::string& lado, double cantidad, double px) {
  json c = cartera_info(cid), inst = instrumento(simbolo);
  const Tarifa& tar = *find_tarifa(jstr(c, "tarifa"));
  std::string divisa = jstr(inst, "divisa"), mk = jstr(inst, "mercado");
  double f = fx(divisa);
  double efectivo = cantidad * px / f;
  std::string tramo = mk == "CRIPTO" ? "cripto" : M(mk).nacional ? "nacional" : "internacional";
  const Tramo& t = tramo == "cripto" ? tar.cripto : tramo == "nacional" ? tar.nacional : tar.internacional;
  double com = std::max(t.minimo, t.fijo + t.pct * efectivo);
  if (mk == "BME") com += CANON_FIJO + CANON_PCT * efectivo;
  double imp = lado == "compra" ? efectivo * fget(inst, "impuesto_pct") : 0.0;
  double cambio = divisa != "EUR" ? efectivo * tar.cambio_pct : 0.0;
  return {{"efectivo_eur", r2(efectivo)}, {"comision_eur", r2(com)}, {"impuestos_eur", r2(imp)},
          {"cambio_eur", r2(cambio)},     {"fx", f},                 {"tramo", tramo}};
}

std::pair<double, double> Bolsa::fill_price(const json& inst, const json& quote, const std::string& lado,
                                            double cantidad) {
  double base = lado == "compra" ? fget(quote, "venta") : fget(quote, "compra");
  double daily = fget(inst, "vol") / std::sqrt(252.0);
  double impact = 0.1 * daily * std::sqrt(std::max(cantidad, 0.0) / std::max(fget(inst, "adv"), 1.0));
  double noise = std::abs(gauss(0, fget(inst, "spread_bps") / 1e4 / 4));
  double frac = std::min(impact + noise, 0.05);
  double px = lado == "compra" ? base * (1 + frac) : base * (1 - frac);
  return {px, std::abs(px - fget(quote, "precio")) * cantidad};
}

double Bolsa::held(long long cid, const std::string& sym) {
  return jnum(db_.scalar("SELECT COALESCE(SUM(cantidad),0) q FROM lotes WHERE cartera=? AND simbolo=?", {cid, sym}));
}

double Bolsa::reserved(long long cid, const std::string& sym) {
  return jnum(db_.scalar("SELECT COALESCE(SUM(cantidad),0) q FROM ordenes WHERE cartera=? AND simbolo=? AND "
                         "lado='venta' AND estado IN ('pendiente','por_confirmar','confirmada')",
                         {cid, sym}));
}

json Bolsa::ordenar(const std::string& simbolo, std::string lado, const json& cantidad_in, std::string tipo,
                    const json& limite_in, const json& stop_in, const std::string& validez, const json& cartera,
                    const std::string& origen) {
  json c = cartera_info(cartera), inst = instrumento(simbolo);
  long long cid = c["id"].get<long long>();
  std::string sym = jstr(inst, "simbolo"), mk = jstr(inst, "mercado");
  lado = lower(lado);
  if (lado == "comprar" || lado == "buy") lado = "compra";
  if (lado == "vender" || lado == "sell") lado = "venta";
  tipo = replace_all(replace_all(lower(tipo), " ", "_"), "-", "_");
  if (lado != "compra" && lado != "venta") throw std::invalid_argument("lado debe ser «compra» o «venta»");
  if (std::find(ORDER_TYPES.begin(), ORDER_TYPES.end(), tipo) == ORDER_TYPES.end())
    throw std::invalid_argument("tipo de orden desconocido: " + tipo + ". Disponibles: " + join(ORDER_TYPES, ", "));
  if (validez != "dia" && validez != "hasta_cancelar")
    throw std::invalid_argument("validez debe ser «dia» o «hasta_cancelar»");
  double cantidad = num(cantidad_in);
  bool frac = mk == "CRIPTO";
  if (cantidad <= 0 || (!frac && std::abs(cantidad - std::round(cantidad)) > 1e-9))
    throw std::invalid_argument(std::string("la cantidad debe ser positiva") + (frac ? "" : " y entera (títulos completos)"));
  auto empty = [](const json& v) { return v.is_null() || (v.is_string() && v.get<std::string>().empty()); };
  json limite = empty(limite_in) ? json(nullptr) : json(num(limite_in));
  json stop = empty(stop_in) ? json(nullptr) : json(num(stop_in));
  if ((tipo == "limitada" || tipo == "stop_limitada") && !truthy(limite))
    throw std::invalid_argument("una orden limitada necesita precio límite");
  if ((tipo == "stop" || tipo == "stop_limitada") && !truthy(stop))
    throw std::invalid_argument("una orden stop necesita precio de activación");
  if (lado == "venta") {
    double h = held(cid, sym) - reserved(cid, sym);
    if (cantidad > h + 1e-9)
      throw std::invalid_argument("no tienes suficientes " + sym + " para vender (" + fnum(h, 6) +
                                  " disponibles); en esta mesa no se permiten ventas en corto");
  }
  double estimado = 0;
  if (lado == "compra") {
    json q = precio(sym);
    double px = std::max({fget(q, "venta"), truthy(limite) ? limite.get<double>() : 0.0,
                          truthy(stop) ? stop.get<double>() : 0.0});
    json g = costes(cid, sym, lado, cantidad, px);
    estimado = r2(fget(g, "efectivo_eur") * 1.01 + fget(g, "comision_eur") + fget(g, "impuestos_eur") + fget(g, "cambio_eur"));
    double res = jnum(db_.scalar("SELECT COALESCE(SUM(estimado_eur),0) e FROM ordenes WHERE cartera=? AND "
                                 "lado='compra' AND estado IN ('pendiente','por_confirmar','confirmada')",
                                 {cid}));
    double libre = fget(c, "efectivo") - res;
    if (estimado > libre + 1e-6)
      throw std::invalid_argument("efectivo insuficiente: la orden necesita unos " + fnum_fixed(estimado, 2) + " € y hay " +
                                  fnum_fixed(libre, 2) + " € libres" +
                                  (res ? " (" + fnum_fixed(res, 2) + " € reservados en otras órdenes)" : ""));
  }
  double t = now();
  auto s = next_session(mk, t);
  double expira = validez == "dia" ? s.second : t + GTC_DAYS * 86400.0;
  std::string estado = jstr(c, "modo") == "real" ? "por_confirmar" : "pendiente";
  long long oid;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    db_.execute("INSERT INTO ordenes (cartera, simbolo, lado, tipo, cantidad, limite, stop, validez, estado, creada, "
                "expira, origen, estimado_eur) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {cid, sym, lado, tipo, cantidad, limite, stop, validez, estado, t, expira, origen, estimado});
    oid = db_.last_insert_id();
  }
  if (estado == "pendiente") procesar(oid);
  json o = orden(oid);
  if (estado == "por_confirmar") {
    o["nota"] = std::string("Cartera REAL: la orden NO se ha enviado a ningún bróker. Revísala y, si la quieres, "
                            "confírmala tú escribiendo ") +
                CONFIRMACION + " en la app o la terminal; después la ejecutas en tu bróker y registras la ejecución.";
  } else if (jstr(o, "estado") == "pendiente") {
    o["nota"] = !is_open(mk, t) ? std::string(M(mk).nombre) + " está cerrado: se ejecutará cuando abra (" +
                                      fmt_local(mk, s.first) + " hora local)"
                                : std::string("Orden en el libro: espera a que el precio la active");
  }
  o["aviso"] = AVISO;
  return o;
}

json Bolsa::orden(long long oid) {
  auto r = db_.one("SELECT * FROM ordenes WHERE id=?", {oid});
  if (!r) throw std::invalid_argument("no existe la orden " + std::to_string(oid));
  json o = *r;
  for (auto* k : {"creada", "expira", "ejecutada"})
    if (truthy(o[k])) o[k] = iso_utc(std::floor(jnum(o[k])));
  auto op = db_.one("SELECT * FROM operaciones WHERE orden=?", {o["id"]});
  if (op) o["operacion"] = *op;
  return o;
}

json Bolsa::ordenes(const json& cartera, bool abiertas) {
  json c = cartera_info(cartera);
  std::string sql = std::string("SELECT id FROM ordenes WHERE cartera=?") +
                    (abiertas ? " AND estado IN ('pendiente','por_confirmar','confirmada')" : "") +
                    " ORDER BY id DESC LIMIT 200";
  json out = json::array();
  for (auto& r : db_.query(sql, {c["id"]})) out.push_back(orden(r["id"].get<long long>()));
  return out;
}

json Bolsa::cancelar(long long oid) {
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto o = db_.one("SELECT * FROM ordenes WHERE id=?", {oid});
    if (!o) throw std::invalid_argument("no existe la orden " + std::to_string(oid));
    std::string st = jstr(*o, "estado");
    if (st != "pendiente" && st != "por_confirmar" && st != "confirmada")
      throw std::invalid_argument("la orden " + std::to_string(oid) + " ya está " + st);
    db_.execute("UPDATE ordenes SET estado='cancelada', motivo='cancelada por el usuario' WHERE id=?", {oid});
  }
  return orden(oid);
}

std::vector<long long> Bolsa::procesar(long long only) {
  std::vector<long long> done;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::string sql = std::string("SELECT o.* FROM ordenes o JOIN carteras c ON c.id=o.cartera WHERE o.estado='pendiente' "
                                "AND c.modo='simulado'") +
                    (only ? " AND o.id=?" : "") + " ORDER BY o.id";
  Params p;
  if (only) p.push_back(only);
  for (auto& o : db_.query(sql, p))
    if (process_one(o)) done.push_back(o["id"].get<long long>());
  return done;
}

bool Bolsa::process_one(json o) {
  json inst = instrumento(jstr(o, "simbolo"));
  std::string mk = jstr(inst, "mercado");
  double t = now();
  long long oid = o["id"].get<long long>();
  if (!is_open(mk, t)) {
    if (t >= fget(o, "expira")) set(oid, {{"estado", "caducada"}, {"motivo", "fin de la validez sin ejecutarse"}});
    return false;
  }
  json q = precio(jstr(o, "simbolo"));
  if (t >= fget(o, "expira")) {
    set(oid, {{"estado", "caducada"}, {"motivo", "fin de la validez sin ejecutarse"}});
    return false;
  }
  bool buy = jstr(o, "lado") == "compra";
  double ref = buy ? fget(q, "venta") : fget(q, "compra");
  std::string tipo = jstr(o, "tipo");
  double qty = fget(o, "cantidad");
  if ((tipo == "stop" || tipo == "stop_limitada") && !truthy(o["activada"])) {
    double stp = fget(o, "stop"), p = fget(q, "precio");
    if ((buy && p >= stp) || (!buy && p <= stp)) {
      set(oid, {{"activada", 1}});
      o["activada"] = 1;
    } else {
      return false;
    }
  }
  auto [px, slip] = fill_price(inst, q, jstr(o, "lado"), qty);
  if (tipo == "limitada" || tipo == "stop_limitada") {
    double lim = fget(o, "limite");
    if ((buy && ref > lim) || (!buy && ref < lim)) return false;
    px = buy ? std::min(px, lim) : std::max(px, lim);
    slip = std::abs(px - fget(q, "precio")) * qty;
  }
  return execute(o, px, slip / fx(jstr(inst, "divisa")));
}

void Bolsa::set(long long oid, const json& kw) {
  std::vector<std::string> cols;
  Params p;
  for (auto& [k, v] : kw.items()) {
    cols.push_back(k + "=?");
    p.push_back(v);
  }
  p.push_back(oid);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  db_.execute("UPDATE ordenes SET " + join(cols, ", ") + " WHERE id=?", p);
}

bool Bolsa::execute(const json& o, double px, double slip_eur, const json& gastos) {
  json c = cartera_info(o["cartera"]);
  long long cid = c["id"].get<long long>(), oid = o["id"].get<long long>();
  std::string sym = jstr(o, "simbolo"), lado = jstr(o, "lado");
  double qty = fget(o, "cantidad");
  bool buy = lado == "compra";
  json g = gastos.is_null() ? costes(cid, sym, lado, qty, px) : gastos;
  double fees = fget(g, "comision_eur") + fget(g, "impuestos_eur") + fget(g, "cambio_eur");
  double total = buy ? fget(g, "efectivo_eur") + fees : fget(g, "efectivo_eur") - fees;
  double efectivo = fget(c, "efectivo");
  std::string aviso;
  json gain = nullptr;
  double t = now();
  const double two_months = 61 * 86400.0;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (buy && total > efectivo + 1e-6) {
    db_.execute("UPDATE ordenes SET estado='rechazada', motivo=? WHERE id=?",
                {"efectivo insuficiente: hacen falta " + fnum_fixed(total, 2) + " € y hay " + fnum_fixed(efectivo, 2) + " €", oid});
    return false;
  }
  db_.transaction([&] {
    if (buy) {
      db_.execute("INSERT INTO lotes (cartera, simbolo, cantidad, coste_unit_eur, ts) VALUES (?,?,?,?,?)",
                  {cid, sym, qty, total / qty, t});
      if (db_.one("SELECT 1 FROM operaciones WHERE cartera=? AND simbolo=? AND lado='venta' AND ganancia_eur < 0 AND ts >= ?",
                  {cid, sym, t - two_months}))
        aviso = "Recompra antes de dos meses tras vender con pérdidas: esa pérdida no se puede compensar en el IRPF "
                "hasta que vendas estos títulos (art. 33.5 LIRPF).";
    } else {
      double left = qty, cost = 0;
      for (auto& lot : db_.query("SELECT * FROM lotes WHERE cartera=? AND simbolo=? AND cantidad > 1e-12 ORDER BY ts, id",
                                 {cid, sym})) {
        double take = std::min(left, fget(lot, "cantidad"));
        cost += take * fget(lot, "coste_unit_eur");
        db_.execute("UPDATE lotes SET cantidad=? WHERE id=?", {fget(lot, "cantidad") - take, lot["id"]});
        left -= take;
        if (left <= 1e-12) break;
      }
      if (left > 1e-9) throw std::invalid_argument("no hay suficientes " + sym + " para vender");
      db_.execute("DELETE FROM lotes WHERE cartera=? AND cantidad <= 1e-12", {cid});
      gain = r2(total - cost);
      if (gain.get<double>() < 0 &&
          db_.one("SELECT 1 FROM lotes WHERE cartera=? AND simbolo=? AND cantidad > 1e-12 AND ts >= ?", {cid, sym, t - two_months}))
        aviso = "Pérdida con compras de este valor en los dos meses anteriores: no podrás compensarla en el IRPF hasta "
                "vender esos títulos (art. 33.5 LIRPF).";
    }
    db_.execute("UPDATE carteras SET efectivo=? WHERE id=?", {r2(buy ? efectivo - total : efectivo + total), cid});
    db_.execute("INSERT INTO operaciones (cartera, orden, simbolo, lado, cantidad, precio, fx, efectivo_eur, comision_eur, "
                "impuestos_eur, cambio_eur, deslizamiento_eur, total_eur, ganancia_eur, aviso, ts) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {cid, oid, sym, lado, qty, px, g["fx"], g["efectivo_eur"], g["comision_eur"], g["impuestos_eur"],
                 g["cambio_eur"], r2(slip_eur), r2(total), gain, aviso, t});
    db_.execute("UPDATE ordenes SET estado='ejecutada', precio_ejec=?, ejecutada=? WHERE id=?", {px, t, oid});
  });
  L.info("Orden ", oid, " ejecutada: ", lado, " ", fnum(qty, 8), " ", sym, " a ", fnum_fixed(px, 4));
  return true;
}

json Bolsa::operaciones(const json& cartera, int limite) {
  json c = cartera_info(cartera);
  json out = json::array();
  for (auto& r : db_.query("SELECT * FROM operaciones WHERE cartera=? ORDER BY ts DESC, id DESC LIMIT ?", {c["id"], limite})) {
    json x = r;
    x["fecha"] = iso_utc(std::floor(fget(r, "ts")));
    out.push_back(x);
  }
  return out;
}

json Bolsa::informe_fiscal(const json& cartera, const json& anio) {
  json c = cartera_info(cartera);
  int y = truthy(anio) ? (int)num(anio) : year_of(day_of(now()));
  double a = days(y, 1, 1) * 86400.0, b = days(y + 1, 1, 1) * 86400.0;
  json rows = json::array();
  double neto = 0;
  for (auto& r : db_.query("SELECT simbolo, SUM(ganancia_eur) g, COUNT(*) n, SUM(CASE WHEN aviso!='' THEN 1 ELSE 0 END) av "
                           "FROM operaciones WHERE cartera=? AND lado='venta' AND ts>=? AND ts<? GROUP BY simbolo",
                           {c["id"], a, b})) {
    neto += jnum(r["g"]);
    rows.push_back(r);
  }
  return {{"cartera", c["id"]},
          {"modo", c["modo"]},
          {"anio", y},
          {"por_valor", rows},
          {"neto_eur", r2(neto)},
          {"nota", "Orientativo: en la Renta las ganancias y pérdidas por transmisión de acciones, ETF y criptoactivos van "
                   "a la base del ahorro. Los tramos y la normativa foral cambian: confírmalo con tu asesor fiscal. En "
                   "simulado no hay nada que declarar."},
          {"aviso", AVISO}};
}

json Bolsa::confirmar(long long oid, const std::string& confirmacion) {
  if (upper(trim(confirmacion)) != CONFIRMACION)
    throw PermissionDenied(std::string("para confirmar una orden real escribe exactamente ") + CONFIRMACION);
  auto o = db_.one("SELECT * FROM ordenes WHERE id=?", {oid});
  if (!o || jstr(*o, "estado") != "por_confirmar")
    throw std::invalid_argument("la orden " + std::to_string(oid) + " no está por confirmar");
  set(oid, {{"estado", "confirmada"},
            {"motivo", "confirmada por el usuario; ejecútala en tu bróker y registra la ejecución"}});
  return orden(oid);
}

json Bolsa::registrar_ejecucion(long long oid, const json& precio_in, const json& comision_eur, const json& impuestos_eur,
                                const json& fx_in) {
  auto o = db_.one("SELECT * FROM ordenes WHERE id=?", {oid});
  if (!o || jstr(*o, "estado") != "confirmada")
    throw std::invalid_argument("la orden " + std::to_string(oid) + " no está confirmada");
  json inst = instrumento(jstr(*o, "simbolo"));
  double rate = truthy(fx_in) ? num(fx_in) : fx(jstr(inst, "divisa"));
  double px = num(precio_in);
  if (px <= 0 || rate <= 0) throw std::invalid_argument("precio y tipo de cambio deben ser positivos");
  double efectivo = fget(*o, "cantidad") * px / rate;
  double imp = impuestos_eur.is_null() || (impuestos_eur.is_string() && impuestos_eur.get<std::string>().empty())
                   ? (jstr(*o, "lado") == "compra" ? efectivo * fget(inst, "impuesto_pct") : 0.0)
                   : num(impuestos_eur);
  json g = {{"efectivo_eur", r2(efectivo)}, {"comision_eur", r2(num(comision_eur))}, {"impuestos_eur", r2(imp)},
            {"cambio_eur", 0.0},            {"fx", rate}};
  if (!execute(*o, px, 0.0, g)) throw std::invalid_argument(jstr(orden(oid), "motivo"));
  return orden(oid);
}

namespace {
std::mutex g_mu;
std::map<std::string, std::unique_ptr<Bolsa>> g_inst;
}  // namespace

// cotizaciones reales salvo que la configuracion diga bolsa.datos: simulados (o NEXUS_BOLSA_OFFLINE=1)
Proveedor* proveedor_configurado() {
  auto& cfg = get_config();
  std::string modo = cfg.str("bolsa.datos", "reales");
  if (modo == "simulados" || env("NEXUS_BOLSA_OFFLINE") == "1") return nullptr;
  auto& prov = proveedor();
  if (auto* dm = dynamic_cast<DatosMercado*>(&prov)) {
    double r = cfg.num("bolsa.refresco_s", 20);
    dm->ttl = r > 0 ? r : 20;
  }
  return &prov;
}

Bolsa& get() {
  std::string path = (paths::sub({"bolsa"}) / "bolsa.db").string();
  std::lock_guard<std::mutex> lk(g_mu);
  auto& p = g_inst[path];
  if (!p) {
    p = std::make_unique<Bolsa>(path);
    p->datos = proveedor_configurado();
  }
  return *p;
}

void reset_instances() {
  reset_agentes();   // los agentes usan la mesa: se paran antes de cerrarla
  std::lock_guard<std::mutex> lk(g_mu);
  g_inst.clear();
}

}  // namespace nx::bolsa
