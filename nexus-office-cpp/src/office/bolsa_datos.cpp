// Cotizaciones reales (nexus/office/bolsa_datos.py): mismas fuentes, mismo orden, mismos JSON y mismo backoff.
#include "office/bolsa_datos.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <optional>

#include "util/crypto.h"
#include "util/csv.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::bolsa {

namespace {

auto L = log::get("bolsa.datos");
const char* UA_WEB =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36 Nexus-Office";
const std::vector<std::string> YAHOO = {"https://query1.finance.yahoo.com", "https://query2.finance.yahoo.com"};
const std::vector<std::pair<std::string, std::string>> STOOQ_SUFFIX = {
    {".MC", ".es"}, {".DE", ".de"}, {".PA", ".fr"}, {".AS", ".nl"}, {".L", ".uk"}};
const std::vector<std::pair<std::string, std::string>> GOOGLE_EXCHANGE = {
    {".MC", "BME"}, {".DE", "ETR"}, {".PA", "EPA"}, {".AS", "AMS"}, {".L", "LON"}, {".MI", "BIT"}, {".SW", "SWX"}};
const std::vector<std::pair<std::string, std::string>> CNBC_SUFFIX = {
    {".MC", "-ES"}, {".DE", "-DE"}, {".PA", "-FR"}, {".AS", "-NL"}, {".L", "-GB"}, {".MI", "-IT"}, {".SW", "-CH"}};
const char* CNBC = "https://quote.cnbc.com/quote-html-webservice/restQuote/symbolType/symbol";
const std::map<std::string, std::string> KRAKEN_PAIR = {
    {"BTC-EUR", "XBTEUR"}, {"ETH-EUR", "ETHEUR"}, {"SOL-EUR", "SOLEUR"}, {"ADA-EUR", "ADAEUR"}};
const std::map<std::string, std::pair<std::string, std::string>> RANGOS = {
    {"1d", {"1d", "1m"}},   {"5d", {"5d", "5m"}}, {"1mo", {"1mo", "30m"}},
    {"6mo", {"6mo", "1d"}}, {"1y", {"1y", "1d"}}, {"5y", {"5y", "1wk"}}};

// error de "el simbolo no esta en esa fuente": no cuenta como fallo de la fuente
struct Lookup : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool is_crypto(const std::string& sym) {
  static const Rx R("^[A-Z0-9]+-(EUR|USD)$");
  return R.test(sym);
}

double to_d(const std::string& s) {
  size_t used = 0;
  double v = std::stod(s, &used);
  if (used != s.size()) throw std::invalid_argument(s);
  return v;
}

}  // namespace

bool Fuente::disponible() const { return now() >= hasta; }

void Fuente::fallo(const std::string& err) {
  ++fallos;
  hasta = now() + std::min(900.0, 30.0 * std::pow(2.0, fallos - 1));
  if (fallos == 1 || fallos == 4) L.info("Cotizaciones: " + nombre + " no responde (" + err + "); se reintenta mas tarde");
}

json yahoo_points(const json& res) {
  json out = json::array();
  const json& ts = res.contains("timestamp") && res["timestamp"].is_array() ? res["timestamp"] : json::array();
  json q = json::object();
  if (res.contains("indicators") && res["indicators"].contains("quote") && res["indicators"]["quote"].is_array() &&
      !res["indicators"]["quote"].empty())
    q = res["indicators"]["quote"][0];
  auto at = [&](const char* k, size_t i) -> json {
    if (!q.contains(k) || !q[k].is_array() || i >= q[k].size()) return nullptr;
    return q[k][i];
  };
  for (size_t i = 0; i < ts.size(); ++i) {
    json c = at("close", i);
    if (!c.is_number()) continue;
    json row = {{"ts", jnum(ts[i])}, {"precio", c.get<double>()}};
    for (auto [k, name] : {std::pair{"open", "apertura"}, {"high", "maximo"}, {"low", "minimo"}, {"volume", "volumen"}}) {
      json v = at(k, i);
      if (v.is_number()) row[name] = v.get<double>();
    }
    out.push_back(std::move(row));
  }
  return out;
}

DatosMercado::DatosMercado(Fetch fetch, double ttl_, double timeout)
    : ttl(ttl_), fetch_(fetch ? std::move(fetch) : Fetch([](const http::Request& r) { return http::send(r); })),
      timeout_(timeout) {
  for (auto n : {"yahoo", "cnbc", "google", "stooq", "coinbase", "kraken", "bce"}) fuentes[n] = Fuente{n};
}

bool DatosMercado::disponible(const std::string& f) {
  std::lock_guard<std::mutex> lk(mu_);
  return fuentes[f].disponible();
}

json DatosMercado::estado() {
  double t = now();
  std::lock_guard<std::mutex> lk(mu_);
  json fs = json::object();
  for (auto& [n, f] : fuentes)
    fs[n] = {{"disponible", f.disponible()}, {"fallos", f.fallos}, {"reintento_s", std::max(0L, std::lround(f.hasta - t))}};
  json http_ = json::object();
  for (auto& [k, v] : ultimo_http_) http_[k] = v;
  return {{"fuentes", fs},
          {"en_linea", t - ultimo_ok_ < 300},
          {"ultima_fuente", ultima_fuente_},
          {"cotizaciones", (int)quotes_.size()},
          {"http", http_}};
}

void DatosMercado::set_mercado(const std::string& sym, const std::string& mercado) {
  std::lock_guard<std::mutex> lk(mu_);
  mercado_de_[upper(sym)] = mercado;
}

bool DatosMercado::tiene_mercado(const std::string& sym) {
  std::lock_guard<std::mutex> lk(mu_);
  return mercado_de_.count(upper(sym)) > 0;
}

http::Response DatosMercado::get(const std::string& fuente, const std::string& url,
                                 const std::map<std::string, std::string>& params, const http::Headers& extra) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fuentes[fuente].disponible()) throw std::runtime_error(fuente + " apartada");
  }
  http::Request req;
  req.url = params.empty() ? url : url + "?" + http::query_string(params);
  req.headers = {{"User-Agent", UA_WEB}};
  for (auto& h : extra) req.headers.push_back(h);
  req.timeout = timeout_;
  http::Response r = fetch_(req);
  std::lock_guard<std::mutex> lk(mu_);
  auto& f = fuentes[fuente];
  if (r.status) ultimo_http_[fuente] = r.status;
  if (r.status == 404) {   // el simbolo no existe en esa fuente: la fuente esta bien
    f.ok();
    throw Lookup(fuente + ": simbolo desconocido");
  }
  if (!r.ok()) {
    std::string err = r.status ? "HTTP " + std::to_string(r.status) : (r.error.empty() ? "sin respuesta" : r.error);
    f.fallo(err);
    throw std::runtime_error(err);
  }
  f.ok();
  return r;
}

json DatosMercado::yahoo_chart(const std::string& sym, const std::string& rango, const std::string& intervalo) {
  std::string err = "yahoo";
  for (auto& host : YAHOO) {
    try {
      auto r = get("yahoo", host + "/v8/finance/chart/" + sym,
                   {{"range", rango}, {"interval", intervalo}, {"includePrePost", "false"}});
      json j = r.json_body();
      json res = j.is_object() && j.contains("chart") && j["chart"].is_object() ? j["chart"].value("result", json())
                                                                                  : json();
      if (!res.is_array() || res.empty()) throw Lookup("yahoo: sin datos para " + sym);
      return res[0];
    } catch (const Lookup&) {
      throw;
    } catch (const std::exception& e) {   // probar el otro host
      err = e.what();
    }
  }
  throw std::runtime_error(err);
}

json DatosMercado::quote_yahoo(const std::string& sym) {
  json res = yahoo_chart(sym, "1d", "1m");
  json m = res.value("meta", json::object());
  json pts = yahoo_points(res);
  json px = m.value("regularMarketPrice", json());
  if (!px.is_number() && !pts.empty()) px = pts.back()["precio"];
  if (!px.is_number()) throw Lookup("yahoo: sin precio para " + sym);
  json prev = m.value("previousClose", json());
  if (!truthy(prev)) prev = m.value("chartPreviousClose", json());
  if (!truthy(prev)) prev = px;
  json tsm = m.value("regularMarketTime", json());
  json tail = json::array();
  for (size_t i = pts.size() > 400 ? pts.size() - 400 : 0; i < pts.size(); ++i) tail.push_back(pts[i]);
  return {{"precio", px.get<double>()},
          {"cierre_anterior", jnum(prev)},
          {"ts", truthy(tsm) ? jnum(tsm) : now()},
          {"divisa", jstr(m, "currency")},
          {"fuente", "Yahoo Finance"},
          {"retraso", ends_with(sym, "-EUR") || ends_with(sym, "-USD") ? "tiempo real" : "≈15 min"},
          {"puntos", tail}};
}

json DatosMercado::quote_stooq(const std::string& sym) {
  std::string s = lower(sym);
  bool done = false;
  for (auto& [suf, st] : STOOQ_SUFFIX)
    if (ends_with(upper(sym), suf)) {
      s = lower(sym.substr(0, sym.size() - suf.size())) + st;
      done = true;
      break;
    }
  if (!done && sym.find('.') == std::string::npos && sym.find('-') == std::string::npos) s = lower(sym) + ".us";
  auto r = get("stooq", "https://stooq.com/q/l/", {{"s", s}, {"f", "sd2t2ohlcvp"}, {"h", ""}, {"e", "csv"}});
  auto rows = csv::read(r.body);
  if (rows.size() < 2) throw Lookup("stooq: sin datos para " + sym);
  std::map<std::string, std::string> row;
  for (size_t i = 0; i < rows[0].size() && i < rows[1].size(); ++i) row[rows[0][i]] = rows[1][i];
  if (!row.count("Close") || row["Close"].empty() || row["Close"] == "N/D") throw Lookup("stooq: sin datos para " + sym);
  double px = to_d(row["Close"]);
  std::string ps = !row["Prev"].empty() ? row["Prev"] : row["Open"];
  double prev = px;
  try {
    if (!ps.empty()) prev = to_d(ps);
  } catch (...) {
    prev = px;
  }
  return {{"precio", px}, {"cierre_anterior", prev}, {"ts", now()},      {"divisa", ""},
          {"fuente", "Stooq"}, {"retraso", "≈15 min"}, {"puntos", json::array()}};
}

// CNBC (acciones y ETF de EE. UU. y Europa): responde tambien desde servidores donde Yahoo limita
namespace {

std::string cnbc_symbol(const std::string& sym) {
  std::string u = upper(sym);
  for (auto& [suf, c] : CNBC_SUFFIX)
    if (ends_with(u, suf)) return u.substr(0, u.size() - suf.size()) + c;
  return u;
}

// numero de CNBC («7,512.5», «-0.35%»); nullopt si falta
std::optional<double> cnbc_num(const json& v) {
  std::string t = v.is_string() ? v.get<std::string>() : v.is_number() ? jdump(v) : "";
  t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == ',' || c == '%'; }), t.end());
  t = trim(t);
  if (t.empty()) return std::nullopt;
  try {
    return to_d(t);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

json DatosMercado::quote_cnbc(const std::string& sym) {
  auto r = get("cnbc", CNBC,
               {{"symbols", cnbc_symbol(sym)}, {"requestMethod", "itv"}, {"noform", "1"}, {"partnerId", "2"},
                {"fund", "1"}, {"exthrs", "1"}, {"output", "json"}});
  json j = r.json_body();
  json q = json::object();
  if (j.is_object() && j.value("FormattedQuoteResult", json()).is_object()) {
    json fq = j["FormattedQuoteResult"].value("FormattedQuote", json());
    if (fq.is_array() && !fq.empty()) q = fq[0];
    else if (fq.is_object()) q = fq;
  }
  double px = cnbc_num(q.value("last", json())).value_or(0);
  if (!(px > 0)) throw Lookup("cnbc: sin precio para " + sym);
  double prev = cnbc_num(q.value("previous_day_closing", json())).value_or(0);
  if (!prev) {   // sin cierre anterior: se deduce de la variacion
    auto ch = cnbc_num(q.value("change", json())), pct = cnbc_num(q.value("change_pct", json()));
    prev = ch ? px - *ch : pct && *pct != -100 ? px / (1 + *pct / 100) : px;
  }
  return {{"precio", px},
          {"cierre_anterior", prev},
          {"ts", now()},
          {"divisa", jstr(q, "currencyCode")},
          {"fuente", "CNBC"},
          {"retraso", jstr(q, "realTime") == "true" ? "tiempo real" : "≈15 min"},
          {"puntos", json::array()}};
}

// velas de CNBC: respaldo de las de Yahoo (responde donde Yahoo limita)
json DatosMercado::cnbc_points(const std::string& sym, const std::string& rango) {
  static const std::map<std::string, std::string> CNBC_RANGO = {{"1d", "1D"}, {"5d", "5D"}, {"1mo", "1M"},
                                                                {"6mo", "6M"}, {"1y", "1Y"}, {"5y", "5Y"}};
  auto it = CNBC_RANGO.find(rango);
  auto r = get("cnbc", "https://ts-api.cnbc.com/harmony/app/charts/" + (it == CNBC_RANGO.end() ? "1D" : it->second) + ".json",
               {{"symbol", cnbc_symbol(sym)}});
  json j = r.json_body(), out = json::array();
  json bd = j.is_object() ? j.value("barData", json::object()) : json::object();
  json bars = bd.is_object() ? bd.value("priceBars", json::array()) : json::array();
  if (!bars.is_array()) return out;
  for (auto& b : bars) {
    if (!b.is_object()) continue;
    auto c = cnbc_num(b.value("close", json())), t = cnbc_num(b.value("tradeTimeinMills", json()));
    if (!c || !*c || !t || !*t) continue;
    json row = {{"ts", *t / 1000}, {"precio", *c}};
    for (auto [k, name] : {std::pair{"open", "apertura"}, {"high", "maximo"}, {"low", "minimo"}, {"volume", "volumen"}})
      if (auto v = cnbc_num(b.value(k, json()))) row[name] = *v;
    out.push_back(row);
  }
  return out;
}

// velas de cripto de Coinbase o Kraken (respaldo de Yahoo, que limita mucho desde servidores)
json DatosMercado::crypto_points(const std::string& sym, const std::string& rango) {
  struct Velas {
    int gran, interval;   // granularidad de Coinbase en s (0 = no sirve), intervalo de Kraken en min
    double span;          // segundos que cubre
  };
  static const std::map<std::string, Velas> CRIPTO_VELAS = {
      {"1d", {300, 5, 86400}},           {"5d", {3600, 30, 5 * 86400}},     {"1mo", {21600, 240, 30 * 86400}},
      {"6mo", {86400, 1440, 182 * 86400}}, {"1y", {86400, 1440, 365 * 86400}}, {"5y", {0, 10080, 5 * 365 * 86400.0}}};
  auto it = CRIPTO_VELAS.find(rango);
  const Velas& v = it == CRIPTO_VELAS.end() ? CRIPTO_VELAS.at("1d") : it->second;
  double desde = now() - v.span;
  auto row = [](double t, json o, json h, json lo, json c, json vol) -> json {
    return {{"ts", t}, {"precio", cnbc_num(c).value_or(0)}, {"apertura", cnbc_num(o).value_or(0)},
            {"maximo", cnbc_num(h).value_or(0)}, {"minimo", cnbc_num(lo).value_or(0)}, {"volumen", cnbc_num(vol).value_or(0)}};
  };
  json out = json::array();
  if (v.gran && v.span / v.gran <= 300) {   // Coinbase da como mucho 300 velas: [ts, min, max, apertura, cierre, vol]
    try {
      json j = get("coinbase", "https://api.exchange.coinbase.com/products/" + sym + "/candles",
                   {{"granularity", std::to_string(v.gran)}}).json_body();
      std::vector<json> rows;
      if (j.is_array())
        for (auto& c : j)
          if (c.is_array() && c.size() >= 6 && c[0].is_number() && c[0].get<double>() >= desde) rows.push_back(c);
      std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a[0].get<double>() < b[0].get<double>(); });
      for (auto& c : rows) out.push_back(row(c[0].get<double>(), c[3], c[2], c[1], c[4], c[5]));
      if (!out.empty()) return out;
    } catch (const std::exception&) {   // Kraken
    }
  }
  auto pair = KRAKEN_PAIR.find(sym);
  if (pair == KRAKEN_PAIR.end()) return out;
  json res = get("kraken", "https://api.kraken.com/0/public/OHLC",
                 {{"pair", pair->second}, {"interval", std::to_string(v.interval)}}).json_body().value("result", json::object());
  if (!res.is_object()) return out;
  for (auto& [k, rows] : res.items()) {
    if (k == "last" || !rows.is_array()) continue;
    for (auto& c : rows)   // [ts, apertura, max, min, cierre, vwap, volumen, n]
      if (c.is_array() && c.size() >= 7 && c[0].is_number() && c[0].get<double>() >= desde)
        out.push_back(row(c[0].get<double>(), c[1], c[2], c[3], c[4], c[6]));
    break;
  }
  return out;
}

std::string DatosMercado::google_symbol(const std::string& sym) {
  std::string u = upper(sym);
  for (auto& [suf, ex] : GOOGLE_EXCHANGE)
    if (ends_with(u, suf)) return u.substr(0, u.size() - suf.size()) + ":" + ex;
  if (is_crypto(u)) return u;
  std::string mk;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = mercado_de_.find(u);
    if (it != mercado_de_.end()) mk = it->second;
  }
  return u + ":" + (mk == "NYSE" ? "NYSE" : "NASDAQ");
}

json DatosMercado::quote_google(const std::string& sym) {
  static const Rx PX(R"re(data-last-price="([0-9.]+)")re");
  static const Rx PREV(R"re(Previous close</div>.*?<div[^>]*>[^0-9]*([0-9][0-9,]*\.?[0-9]*))re", Rx::S);
  static const Rx CUR(R"re(data-currency-code="([A-Z]{3})")re");
  std::string gs = google_symbol(sym);
  auto r = get("google", "https://www.google.com/finance/quote/" + gs, {{"hl", "en"}},
               {{"Cookie", "CONSENT=YES+cb; SOCS=CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyaBg"}});
  std::string body = r.body;
  auto m = PX.search(body);
  if (!m && gs.find(":NASDAQ") != std::string::npos) {   // puede ser de la NYSE
    std::string alt = gs.substr(0, gs.find(":NASDAQ")) + ":NYSE";
    body = get("google", "https://www.google.com/finance/quote/" + alt, {{"hl", "en"}}, {{"Cookie", "CONSENT=YES+cb"}}).body;
    m = PX.search(body);
  }
  if (!m) throw Lookup("google: sin precio para " + sym);
  double px = to_d(m->str(1));
  double prevf = px;
  if (auto p = PREV.search(body)) {
    std::string v = p->str(1);
    v.erase(std::remove(v.begin(), v.end(), ','), v.end());
    try {
      prevf = to_d(v);
    } catch (...) {
      prevf = px;
    }
  }
  auto c = CUR.search(body);
  return {{"precio", px},
          {"cierre_anterior", prevf},
          {"ts", now()},
          {"divisa", c ? c->str(1) : ""},
          {"fuente", "Google Finance"},
          {"retraso", "≈15 min"},
          {"puntos", json::array()}};
}

json DatosMercado::quote_crypto(const std::string& sym) {
  try {
    auto r = get("coinbase", "https://api.coinbase.com/v2/prices/" + sym + "/spot");
    json j = r.json_body();
    double px = jnum(j["data"]["amount"].is_string() ? json(to_d(j["data"]["amount"].get<std::string>()))
                                                     : j["data"]["amount"]);
    if (!(px > 0)) throw Lookup("coinbase: sin precio");
    return {{"precio", px}, {"cierre_anterior", px}, {"ts", now()}, {"divisa", sym.substr(sym.rfind('-') + 1)},
            {"fuente", "Coinbase"}, {"retraso", "tiempo real"}, {"puntos", json::array()}};
  } catch (const std::exception&) {
    auto it = KRAKEN_PAIR.find(upper(sym));
    if (it == KRAKEN_PAIR.end()) throw;
    auto r = get("kraken", "https://api.kraken.com/0/public/Ticker", {{"pair", it->second}});
    json res = r.json_body().value("result", json::object());
    if (!res.is_object() || res.empty()) throw Lookup("kraken: sin datos");
    json d = res.begin().value();
    return {{"precio", to_d(d["c"][0].get<std::string>())}, {"cierre_anterior", to_d(d["o"].get<std::string>())},
            {"ts", now()}, {"divisa", "EUR"}, {"fuente", "Kraken"}, {"retraso", "tiempo real"},
            {"puntos", json::array()}};
  }
}

json DatosMercado::quote(const std::string& sym_in, double max_age) {
  std::string sym = upper(sym_in);
  double age = max_age < 0 ? ttl : max_age;
  json hit;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = quotes_.find(sym);
    if (it != quotes_.end()) {
      if (now() - it->second.first < age) return it->second.second;
      hit = it->second.second;
    }
  }
  std::vector<json (DatosMercado::*)(const std::string&)> chain = {&DatosMercado::quote_yahoo};
  if (is_crypto(sym))
    chain.push_back(&DatosMercado::quote_crypto);
  else
    chain.insert(chain.end(), {&DatosMercado::quote_cnbc, &DatosMercado::quote_google, &DatosMercado::quote_stooq});
  for (auto fn : chain) {
    json q;
    try {
      q = (this->*fn)(sym);
    } catch (const std::exception&) {   // siguiente fuente
      continue;
    }
    q["simbolo"] = sym;
    std::lock_guard<std::mutex> lk(mu_);
    quotes_[sym] = {now(), q};
    ultima_fuente_ = jstr(q, "fuente");
    ultimo_ok_ = now();
    return q;
  }
  return hit;
}

json DatosMercado::quotes(const std::vector<std::string>& syms, double max_age) {
  std::vector<std::future<json>> fut;
  for (auto& s : syms) fut.push_back(std::async(std::launch::async, [this, s, max_age] { return quote(s, max_age); }));
  json out = json::object();
  for (size_t i = 0; i < syms.size(); ++i) {
    json q = fut[i].get();
    if (q.is_object()) out[upper(syms[i])] = q;
  }
  return out;
}

json DatosMercado::serie(const std::string& sym_in, const std::string& rango_in) {
  std::string sym = upper(sym_in);
  std::string rango = RANGOS.count(rango_in) ? rango_in : "1d";
  double ttl_s = rango == "1d" || rango == "5d" ? 60 : 6 * 3600;
  std::string key = sym + "|" + rango;
  json hit;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = series_.find(key);
    if (it != series_.end()) {
      if (now() - it->second.first < ttl_s) return it->second.second;
      hit = it->second.second;
    }
  }
  json pts = json::array();
  try {
    auto& [r, i] = RANGOS.at(rango);
    pts = yahoo_points(yahoo_chart(sym, r, i));
  } catch (const std::exception&) {
    pts = json::array();
  }
  static const Rx CRIPTO(R"(^[A-Z0-9]+-(EUR|USD)$)");
  if (pts.empty()) {
    try {   // respaldo: velas de Coinbase/Kraken para cripto y de CNBC para el resto (responden donde Yahoo limita)
      pts = CRIPTO.match(sym) ? crypto_points(sym, rango) : cnbc_points(sym, rango);
    } catch (const std::exception&) {
      pts = json::array();
    }
  }
  if (pts.empty()) return hit.is_array() ? hit : json::array();
  std::lock_guard<std::mutex> lk(mu_);
  series_[key] = {now(), pts};
  return pts;
}

double DatosMercado::fx(const std::string& divisa_in) {
  std::string divisa = upper(divisa_in);
  if (divisa == "EUR") return 1.0;
  double hit = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = fx_.find(divisa);
    if (it != fx_.end()) {
      if (now() - it->second.first < 300) return it->second.second;
      hit = it->second.second;
    }
  }
  double rate = 0;
  try {
    rate = jnum(yahoo_chart("EUR" + divisa + "=X", "1d", "5m")["meta"]["regularMarketPrice"]);
  } catch (const std::exception&) {
    try {
      auto r = get("bce", "https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml");
      Rx re("currency=['\"]" + re_escape(divisa) + "['\"]\\s+rate=['\"]([0-9.]+)");
      auto m = re.search(r.body);
      rate = m ? to_d(m->str(1)) : 0;
    } catch (const std::exception&) {
      rate = 0;
    }
  }
  if (rate > 0) {
    std::lock_guard<std::mutex> lk(mu_);
    fx_[divisa] = {now(), rate};
    return rate;
  }
  return hit;
}

Proveedor& proveedor() {
  static DatosMercado P;
  return P;
}

}  // namespace nx::bolsa
