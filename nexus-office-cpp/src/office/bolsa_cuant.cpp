// Laboratorio cuantitativo de la mesa de bolsa (ver bolsa_cuant.h y docs/BOLSA_PROYECTOS.md).
#include "office/bolsa_cuant.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <set>

#include "office/bolsa_agente.h"

namespace nx::bolsa {

namespace {

double clamp1(double v) { return std::max(-1.0, std::min(1.0, v)); }
double rnd(double x, int d) {
  if (!std::isfinite(x)) return 0;
  double m = std::pow(10.0, d);
  return std::round(x * m) / m + 0.0;   // sin "-0"
}
json jopt(const std::optional<double>& v, int d = 4) { return v ? json(rnd(*v, d)) : json(nullptr); }
double media(const std::vector<double>& v) { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
double desv(const std::vector<double>& v) {
  if (v.size() < 2) return 0;
  double m = media(v), s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return std::sqrt(s / (v.size() - 1));
}
std::vector<double> cola(const std::vector<double>& x, size_t n) {
  return std::vector<double>(x.end() - std::min(n, x.size()), x.end());
}
std::vector<double> rentas(const std::vector<double>& v) {
  std::vector<double> r;
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i - 1] > 0) r.push_back(v[i] / v[i - 1] - 1);
  return r;
}
double percentil(std::vector<double> v, double p) {   // interpolacion lineal, como numpy
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double k = (v.size() - 1) * p;
  size_t f = (size_t)std::floor(k), c = std::min(v.size() - 1, f + 1);
  return v[f] + (v[c] - v[f]) * (k - f);
}
long dia(double ts) { return (long)std::floor(ts / 86400.0); }
std::optional<double> param(const json& p, const char* k) {
  if (!p.is_object() || !p.contains(k) || p[k].is_null()) return std::nullopt;
  if (p[k].is_number()) return p[k].get<double>();
  if (p[k].is_string()) {
    try {
      return std::stod(p[k].get<std::string>());
    } catch (...) {
    }
  }
  return std::nullopt;
}
double pnum(const json& p, const char* k, double def) { return param(p, k).value_or(def); }

}  // namespace

// ---------------------------------------------------------------- series
SerieBT serie_bt(const std::string& simbolo, const json& puntos, double fx) {
  SerieBT s;
  s.simbolo = simbolo;
  s.fx = fx > 0 ? fx : 1.0;
  bool hl = true;
  for (auto& p : puntos) {
    if (!p.is_object() || !truthy(p.value("precio", json()))) continue;
    double px = jnum(p["precio"]);
    if (px <= 0) continue;
    s.ts.push_back(jnum(p["ts"]));
    s.precio.push_back(px);
    if (p.contains("maximo") && p.contains("minimo") && p["maximo"].is_number() && p["minimo"].is_number()) {
      s.maximo.push_back(std::max(px, jnum(p["maximo"])));
      s.minimo.push_back(std::min(px, jnum(p["minimo"])));
    } else {
      hl = false;
    }
  }
  if (!hl) s.maximo.clear(), s.minimo.clear();
  return s;
}

// ---------------------------------------------------------------- indicadores
std::optional<double> ema(const std::vector<double>& x, size_t n) {
  if (!n || x.size() < n) return std::nullopt;
  double k = 2.0 / (n + 1), e = 0;
  for (size_t i = 0; i < n; ++i) e += x[i];
  e /= n;
  for (size_t i = n; i < x.size(); ++i) e = x[i] * k + e * (1 - k);
  return e;
}

std::optional<double> hurst(const std::vector<double>& x, size_t max_lag) {
  if (x.size() < max_lag + 20) return std::nullopt;
  std::vector<double> lx;
  for (double v : x) {
    if (v <= 0) return std::nullopt;
    lx.push_back(std::log(v));
  }
  std::vector<double> ll, lt;
  for (size_t lag = 2; lag <= max_lag; ++lag) {
    std::vector<double> d;
    for (size_t i = lag; i < lx.size(); ++i) d.push_back(lx[i] - lx[i - lag]);
    double s = desv(d);
    if (s <= 0) continue;
    ll.push_back(std::log((double)lag));
    lt.push_back(std::log(s));
  }
  if (ll.size() < 3) return std::nullopt;
  double ml = media(ll), mt = media(lt), num = 0, den = 0;
  for (size_t i = 0; i < ll.size(); ++i) num += (ll[i] - ml) * (lt[i] - mt), den += (ll[i] - ml) * (ll[i] - ml);
  return den > 0 ? std::optional<double>(num / den) : std::nullopt;
}

json macd(const std::vector<double>& x) {
  if (x.size() < 35) return nullptr;
  std::vector<double> linea;   // MACD de cada sesion desde la 26
  for (size_t i = 26; i <= x.size(); ++i) {
    std::vector<double> sub(x.begin(), x.begin() + i);
    linea.push_back(*ema(sub, 12) - *ema(sub, 26));
  }
  auto s = ema(linea, 9);
  if (!s) return nullptr;
  return {{"macd", rnd(linea.back(), 4)}, {"senal", rnd(*s, 4)}, {"histograma", rnd(linea.back() - *s, 4)}};
}

json bollinger(const std::vector<double>& x, size_t n, double k) {
  if (x.size() < n) return nullptr;
  auto t = cola(x, n);
  double m = media(t), s = desv(t);
  double hi = m + k * s, lo = m - k * s;
  return {{"media", rnd(m, 4)}, {"alta", rnd(hi, 4)}, {"baja", rnd(lo, 4)},
          {"pct_b", hi > lo ? rnd((x.back() - lo) / (hi - lo), 3) : 0.5}};
}

json senales(const json& puntos) {
  std::vector<double> x;
  for (auto& p : puntos)
    if (p.is_object() && truthy(p.value("precio", json()))) x.push_back(jnum(p["precio"]));
  if (x.size() < 60) return nullptr;
  double px = x.back();
  double vol = volatilidad(x, 20).value_or(0.02);
  if (vol <= 0) vol = 0.02;
  json est = json::object();
  // tendencia: medias exponenciales de 8, 21 y 55 sesiones alineadas
  double e8 = *ema(x, 8), e21 = *ema(x, 21), e55 = *ema(x, std::min<size_t>(55, x.size()));
  double s_t = e8 > e21 && e21 > e55 ? 1.0 : e8 < e21 && e21 < e55 ? -1.0 : e8 > e55 ? 0.3 : -0.3;
  double c_t = std::min(1.0, std::abs(e8 - e55) / e55 / (vol * std::sqrt(20.0)));
  est["tendencia"] = {{"senal", s_t}, {"confianza", rnd(c_t, 3)},
                      {"detalle", {{"ema8", rnd(e8, 4)}, {"ema21", rnd(e21, 4)}, {"ema55", rnd(e55, 4)}}}};
  // reversion a la media: distancia a la media de 50 en desviaciones y bandas de Bollinger
  auto t50 = cola(x, 50);
  double m50 = media(t50), sd50 = desv(t50);
  double z = sd50 > 0 ? (px - m50) / sd50 : 0;
  json bb = bollinger(x);
  est["reversion"] = {{"senal", rnd(clamp1(-z / 2), 3)}, {"confianza", rnd(std::min(1.0, std::abs(z) / 3), 3)},
                      {"detalle", {{"z", rnd(z, 3)}, {"bollinger", bb}}}};
  // momento: 1, 3 y 6 meses ajustados por volatilidad
  double acc = 0, pes = 0;
  for (auto [n, w] : std::vector<std::pair<size_t, double>>{{21, 0.4}, {63, 0.3}, {126, 0.3}}) {
    if (x.size() <= n) continue;
    double r = px / x[x.size() - 1 - n] - 1;
    acc += w * clamp1(r / (vol * std::sqrt((double)n) * 1.5));
    pes += w;
  }
  double s_m = pes ? acc / pes : 0;
  est["momento"] = {{"senal", rnd(s_m, 3)}, {"confianza", rnd(std::min(1.0, std::abs(s_m) * 1.2), 3)},
                    {"detalle", {{"roc21_pct", rnd((px / x[x.size() - 22] - 1) * 100, 2)}}}};
  // regimen de volatilidad: la de 20 sesiones frente a la de 60
  double v60 = volatilidad(x, std::min<size_t>(59, x.size() - 1)).value_or(vol);
  double ratio = v60 > 0 ? vol / v60 : 1;
  est["volatilidad"] = {{"senal", rnd(clamp1((1 - ratio) * 2.5), 3)},
                        {"confianza", rnd(std::min(1.0, std::abs(1 - ratio) * 2), 3)},
                        {"detalle", {{"vol20_pct", rnd(vol * 100, 2)}, {"vol60_pct", rnd(v60 * 100, 2)}, {"ratio", rnd(ratio, 3)}}}};
  // estadistica: exponente de Hurst (>0,5 la serie persiste, <0,5 vuelve a la media)
  auto h = hurst(x);
  double s_h = 0, c_h = 0;
  if (h) {
    if (*h > 0.55) s_h = (s_t > 0 ? 1 : -1) * clamp1((*h - 0.5) * 4);
    else if (*h < 0.45) s_h = (z < 0 ? 1 : -1) * clamp1((0.5 - *h) * 4);
    c_h = std::min(1.0, std::abs(*h - 0.5) * 4);
  }
  est["estadistica"] = {{"senal", rnd(s_h, 3)}, {"confianza", rnd(c_h, 3)}, {"detalle", {{"hurst", jopt(h, 3)}}}};
  static const std::vector<std::pair<std::string, double>> W = {
      {"tendencia", 0.25}, {"reversion", 0.20}, {"momento", 0.25}, {"volatilidad", 0.15}, {"estadistica", 0.15}};
  double comb = 0, conf = 0;
  for (auto& [k, w] : W) {
    comb += w * jnum(est[k]["senal"]) * jnum(est[k]["confianza"]);
    conf += w * jnum(est[k]["confianza"]);
  }
  return {{"estrategias", est},
          {"combinada", rnd(comb, 3)},
          {"confianza", rnd(conf, 3)},
          {"lectura", comb > 0.2 ? "alcista" : comb < -0.2 ? "bajista" : "neutral"},
          {"indicadores",
           {{"precio", px}, {"rsi", jopt(rsi(x), 1)}, {"macd", macd(x)}, {"bollinger", bb}, {"vol_diaria_pct", rnd(vol * 100, 2)}}}};
}

// ---------------------------------------------------------------- metricas (QuantStats)
double periodos_anio(const std::vector<double>& ts) {
  if (ts.size() < 3) return 252;
  std::vector<double> d;
  for (size_t i = 1; i < ts.size(); ++i) d.push_back(ts[i] - ts[i - 1]);
  double med = percentil(d, 0.5) / 86400.0;
  if (med >= 25) return 12;
  if (med >= 5) return 52;
  if (med >= 0.9) return 252;
  return 252 * 6.5 * 3600 / std::max(60.0, med * 86400);   // intradia: sesiones de 6,5 h
}

json metricas(const std::vector<double>& v, double ppy) {
  json out = {{"periodos", v.size()}};
  if (v.size() < 2 || v.front() <= 0) return out;
  auto r = rentas(v);
  double total = v.back() / v.front() - 1;
  double anos = (v.size() - 1) / ppy;
  double cagr = v.back() > 0 && anos > 0 ? std::pow(v.back() / v.front(), 1 / anos) - 1 : -1;
  double m = media(r), s = desv(r);
  double dd2 = 0;
  for (double x : r) dd2 += std::min(x, 0.0) * std::min(x, 0.0);
  double dsd = r.empty() ? 0 : std::sqrt(dd2 / r.size());
  double pico = v.front(), maxdd = 0;
  size_t dur = 0, durmax = 0;
  for (double x : v) {
    if (x >= pico) pico = x, dur = 0;
    else ++dur;
    maxdd = std::max(maxdd, (pico - x) / pico);
    durmax = std::max(durmax, dur);
  }
  double var95 = -percentil(r, 0.05);
  std::vector<double> peores;
  for (double x : r)
    if (x <= -var95) peores.push_back(x);
  size_t pos = std::count_if(r.begin(), r.end(), [](double x) { return x > 0; });
  out["rentabilidad_pct"] = rnd(total * 100, 2);
  out["cagr_pct"] = rnd(cagr * 100, 2);
  out["volatilidad_pct"] = rnd(s * std::sqrt(ppy) * 100, 2);
  out["sharpe"] = s > 0 ? rnd(m / s * std::sqrt(ppy), 2) : 0.0;
  out["sortino"] = dsd > 0 ? rnd(m / dsd * std::sqrt(ppy), 2) : 0.0;
  out["max_drawdown_pct"] = rnd(maxdd * 100, 2);
  out["duracion_max_drawdown"] = durmax;
  out["calmar"] = maxdd > 0 ? rnd(cagr / maxdd, 2) : 0.0;
  out["var95_pct"] = rnd(var95 * 100, 2);
  out["cvar95_pct"] = rnd(-media(peores) * 100, 2);
  out["mejor_pct"] = r.empty() ? 0.0 : rnd(*std::max_element(r.begin(), r.end()) * 100, 2);
  out["peor_pct"] = r.empty() ? 0.0 : rnd(*std::min_element(r.begin(), r.end()) * 100, 2);
  out["positivos_pct"] = r.empty() ? 0.0 : rnd(100.0 * pos / r.size(), 1);
  out["valor_inicial"] = rnd(v.front(), 2);
  out["valor_final"] = rnd(v.back(), 2);
  return out;
}

json metricas_operaciones(const std::vector<double>& g) {
  std::vector<double> win, loss;
  for (double x : g) (x > 0 ? win : loss).push_back(x);
  double sw = std::accumulate(win.begin(), win.end(), 0.0), sl = std::accumulate(loss.begin(), loss.end(), 0.0);
  return {{"operaciones", g.size()},
          {"ganadoras", win.size()},
          {"aciertos_pct", g.empty() ? 0.0 : rnd(100.0 * win.size() / g.size(), 1)},
          {"factor_beneficio", sl < 0 ? json(rnd(sw / -sl, 2)) : json(nullptr)},
          {"media_ganancia_eur", rnd(media(win), 2)},
          {"media_perdida_eur", rnd(media(loss), 2)},
          {"esperanza_eur", rnd(media(g), 2)},
          {"resultado_eur", rnd(sw + sl, 2)},
          {"mejor_eur", g.empty() ? 0.0 : rnd(*std::max_element(g.begin(), g.end()), 2)},
          {"peor_eur", g.empty() ? 0.0 : rnd(*std::min_element(g.begin(), g.end()), 2)}};
}

// ---------------------------------------------------------------- backtest
namespace {

struct PosBT {
  double qty = 0, entrada = 0, stop = 0, objetivo = 0, atr = 0, coste_eur = 0;
  long desde = 0;
  json factores;
};

json ventana(const SerieBT& s, size_t fin, size_t n) {   // puntos [fin-n, fin) como los da la mesa
  json pts = json::array();
  for (size_t i = fin > n ? fin - n : 0; i < fin; ++i) {
    json p = {{"ts", s.ts[i]}, {"precio", s.precio[i]}};
    if (!s.maximo.empty()) p["maximo"] = s.maximo[i], p["minimo"] = s.minimo[i];
    pts.push_back(p);
  }
  return pts;
}

std::string fecha(long d) { return ymd(d); }

}  // namespace

json backtest(const std::vector<SerieBT>& series, const json& prm) {
  std::string estr = prm.is_object() ? jstr(prm, "estrategia", "agente") : "agente";
  static const std::set<std::string> ESTR = {"agente", "senales", "cruce_medias", "comprar_mantener"};
  if (!ESTR.count(estr)) throw std::invalid_argument("estrategia desconocida: " + estr);
  double capital = pnum(prm, "capital", 10000), riesgo = pnum(prm, "riesgo_pct", 1.0);
  long max_pos = (long)pnum(prm, "max_posiciones", 8);
  double max_peso = pnum(prm, "max_peso_pct", 20), reserva = pnum(prm, "reserva_pct", 10);
  double com_pct = pnum(prm, "comision_pct", 0.1), com_min = pnum(prm, "comision_min_eur", 1.0);
  double slip = pnum(prm, "deslizamiento_bps", 5) / 1e4, umbral = pnum(prm, "umbral", 0.9);
  double min_op = pnum(prm, "min_operacion_eur", 150), fuera = pnum(prm, "fuera_muestra_pct", 30);
  bool aprende = !prm.is_object() || prm.value("aprender", true);
  if (capital <= 0) throw std::invalid_argument("el capital debe ser positivo");
  json pesos = {{"tendencia", 1.0}, {"momento", 1.0}, {"rsi", 0.6}, {"volatilidad", 0.4}};
  if (prm.is_object() && prm.contains("pesos") && prm["pesos"].is_object())
    for (auto it = prm["pesos"].begin(); it != prm["pesos"].end(); ++it) pesos[it.key()] = it.value();
  const size_t W = estr == "senales" ? 130 : 60, MIN = estr == "senales" ? 60 : estr == "comprar_mantener" ? 1 : 50;

  std::vector<const SerieBT*> ss;
  for (auto& s : series)
    if (s.precio.size() >= MIN + 5) ss.push_back(&s);
  json avisos = json::array();
  if (ss.size() < series.size())
    avisos.push_back(std::to_string(series.size() - ss.size()) + " valor(es) sin historia suficiente (" +
                     std::to_string(MIN + 5) + " sesiones como mínimo)");
  if (ss.empty()) throw std::invalid_argument("sin series con historia suficiente para el backtest");
  std::set<long> dias;
  for (auto* s : ss)
    for (double t : s->ts) dias.insert(dia(t));
  std::vector<double> ppys;
  for (auto* s : ss) ppys.push_back(periodos_anio(s->ts));
  double ppy = percentil(ppys, 0.5);

  double caja = capital;
  std::map<std::string, PosBT> pos;
  std::map<std::string, size_t> idx;     // sesiones vistas de cada serie
  std::map<std::string, double> ultimo;  // ultimo precio en euros
  std::vector<double> curva, curva_ref, cts;
  json ops = json::array();
  std::vector<double> ganancias;
  std::map<std::string, double> ref_qty;
  double ref_caja = capital;
  bool ref_hecha = false;
  auto comision = [&](double nominal) { return std::max(com_min, nominal * com_pct / 100); };
  auto cerrar = [&](const std::string& sym, double px_div, double fx, long d, const std::string& motivo) {
    PosBT& p = pos[sym];
    double ejec = px_div * (1 - slip);
    double bruto = p.qty * ejec / fx, com = comision(bruto);
    caja += bruto - com;
    double g = bruto - com - p.coste_eur;
    ganancias.push_back(g);
    if (aprende && estr == "agente" && p.factores.is_object()) {   // como AgenteBolsa::aprender
      double sign = g > 0 ? 1.0 : -1.0;
      for (auto* k : {"tendencia", "momento", "rsi", "volatilidad"})
        pesos[k] = rnd(std::max(0.05, std::min(2.5, jnum(pesos[k]) + 0.08 * sign * jnum(p.factores.value(k, json(0))))), 4);
    }
    ops.push_back({{"simbolo", sym}, {"entrada", fecha(p.desde)}, {"salida", fecha(d)}, {"cantidad", p.qty},
                   {"precio_entrada", rnd(p.entrada, 4)}, {"precio_salida", rnd(ejec, 4)}, {"ganancia_eur", rnd(g, 2)},
                   {"motivo", motivo}});
    pos.erase(sym);
  };

  for (long d : dias) {
    // 1) avanza cada serie que tiene sesion hoy y gestiona sus posiciones con el maximo y minimo del dia
    std::map<std::string, json> hoy;   // analisis de las series con sesion hoy
    for (auto* s : ss) {
      size_t& i = idx[s->simbolo];
      bool sesion = false;
      while (i < s->ts.size() && dia(s->ts[i]) <= d) ++i, sesion = true;
      if (!sesion) continue;
      size_t k = i - 1;
      double px = s->precio[k];
      ultimo[s->simbolo] = px / s->fx;
      if (pos.count(s->simbolo) && estr != "comprar_mantener") {
        PosBT& p = pos[s->simbolo];
        double lo = s->minimo.empty() ? px : s->minimo[k], hi = s->maximo.empty() ? px : s->maximo[k];
        if (p.stop > 0 && lo <= p.stop) {
          cerrar(s->simbolo, hi < p.stop ? px : p.stop, s->fx, d, "stop");   // si abre por debajo, sale al cierre
          continue;
        }
        if (p.objetivo > 0 && hi >= p.objetivo) {
          cerrar(s->simbolo, p.objetivo, s->fx, d, "objetivo");
          continue;
        }
      }
      if (estr == "comprar_mantener") {
        hoy[s->simbolo] = {{"precio", px}, {"sym_fx", s->fx}};
        continue;
      }
      if (i < MIN) continue;
      json pts = ventana(*s, i, W);
      json an = analizar(pts);
      if (!an.is_object()) continue;
      an["sym_fx"] = s->fx;
      if (estr == "agente") {
        an["puntuacion"] = puntuar(an, pesos);
      } else if (estr == "senales") {
        json sg = senales(pts);
        if (!sg.is_object()) continue;
        an["puntuacion"] = sg["combinada"];
      } else if (estr == "cruce_medias") {
        an["puntuacion"] = jnum(an["sma20"]) > jnum(an["sma50"]) ? 1.0 : -1.0;
      }
      hoy[s->simbolo] = an;
    }
    // 2) salidas por señal y stop dinamico (trailing) al cierre
    for (auto& [sym, an] : hoy) {
      if (!pos.count(sym) || estr == "comprar_mantener") continue;
      PosBT& p = pos[sym];
      double px = jnum(an["precio"]), sc = jnum(an["puntuacion"]);
      bool sale = estr == "agente"     ? jnum(an["factores"]["tendencia"]) <= -1.0 && sc < 0
                  : estr == "senales" ? sc <= -0.3
                                      : sc < 0;
      if (sale) {
        cerrar(sym, px, jnum(an["sym_fx"]), d, estr == "cruce_medias" ? "cruce a la baja" : "señal de salida");
        continue;
      }
      double nuevo = px - 2 * jnum(an["atr"]);
      if (nuevo > p.stop * 1.002 && px > p.entrada) p.stop = nuevo;
    }
    // 3) valor del dia y entradas
    double invertido = 0;
    for (auto& [sym, p] : pos) invertido += p.qty * ultimo[sym];
    double total = caja + invertido;
    if (estr == "comprar_mantener") {
      if (!ref_hecha && !hoy.empty()) {
        double parte = total / ss.size();
        for (auto& [sym, an] : hoy) {
          double fx = jnum(an["sym_fx"]), px = jnum(an["precio"]) * (1 + slip) / fx;
          double qty = std::floor((parte - comision(parte)) / px);
          if (qty <= 0) continue;
          double coste = qty * px, com = comision(coste);
          caja -= coste + com;
          pos[sym] = {qty, px * fx, 0, 0, 0, coste + com, d, json()};
        }
        ref_hecha = !pos.empty();
      }
    } else {
      double libre = caja - total * reserva / 100;
      long huecos = max_pos - (long)pos.size();
      std::vector<std::pair<std::string, json>> cands;
      for (auto& [sym, an] : hoy) {
        if (pos.count(sym)) continue;
        double sc = jnum(an["puntuacion"]);
        bool entra = estr == "agente"    ? sc >= umbral && !(an["rsi"].is_number() && jnum(an["rsi"]) > 80)
                     : estr == "senales" ? sc >= 0.3
                                         : sc > 0;
        if (entra) cands.push_back({sym, an});
      }
      std::stable_sort(cands.begin(), cands.end(),
                       [](auto& a, auto& b) { return jnum(a.second["puntuacion"]) > jnum(b.second["puntuacion"]); });
      for (size_t c = 0; c < cands.size() && (long)c < huecos; ++c) {
        auto& [sym, an] = cands[c];
        double fx = jnum(an["sym_fx"]), px = jnum(an["precio"]), a = jnum(an["atr"]);
        double px_eur = px * (1 + slip) / fx;
        double dist = std::max(2 * a / fx, px_eur * 0.01);
        double qty = std::min({total * riesgo / 100 / dist, total * max_peso / 100 / px_eur, libre * 0.97 / px_eur});
        qty = sym.find('-') != std::string::npos ? std::floor(qty * 1e4) / 1e4 : std::floor(qty);
        if (qty <= 0 || qty * px_eur < min_op) continue;
        double coste = qty * px_eur, com = comision(coste);
        caja -= coste + com;
        libre -= coste + com;
        double ent = px * (1 + slip);
        pos[sym] = {qty, ent, ent - 2 * a, estr == "cruce_medias" ? 0.0 : ent + 4 * a, a, coste + com, d,
                    an.value("factores", json())};
      }
    }
    invertido = 0;
    for (auto& [sym, p] : pos) invertido += p.qty * ultimo[sym];
    // referencia: comprar y mantener a partes iguales desde la primera sesion con datos de todos
    if (ref_qty.empty() && ultimo.size() == ss.size()) {
      for (auto& [sym, px] : ultimo) ref_qty[sym] = capital / ss.size() / px;
      ref_caja = 0;
    }
    double ref = ref_caja;
    for (auto& [sym, q] : ref_qty) ref += q * ultimo[sym];
    curva.push_back(caja + invertido);
    curva_ref.push_back(ref_qty.empty() ? capital : ref);
    cts.push_back(d * 86400.0);
  }
  // cierra lo abierto al ultimo precio para medir el resultado de las operaciones
  long ultimo_dia = dias.empty() ? 0 : *dias.rbegin();
  std::vector<std::string> abiertas;
  for (auto& [sym, p] : pos) abiertas.push_back(sym);
  json pos_abiertas = json::array();
  for (auto& sym : abiertas) {
    PosBT p = pos[sym];
    pos_abiertas.push_back({{"simbolo", sym}, {"cantidad", p.qty}, {"entrada", rnd(p.entrada, 4)},
                            {"valor_eur", rnd(p.qty * ultimo[sym], 2)}});
  }

  size_t corte = (size_t)std::floor(curva.size() * (1 - std::max(0.0, std::min(90.0, fuera)) / 100));
  json res = {{"estrategia", estr},
              {"simbolos", json::array()},
              {"desde", fecha(*dias.begin())},
              {"hasta", fecha(ultimo_dia)},
              {"sesiones", curva.size()},
              {"metricas", metricas(curva, ppy)},
              {"referencia", metricas(curva_ref, ppy)},
              {"operaciones", metricas_operaciones(ganancias)},
              {"ultimas_operaciones", json::array()},
              {"posiciones_abiertas", pos_abiertas},
              {"pesos_finales", pesos},
              {"avisos", avisos},
              {"parametros",
               {{"capital", capital}, {"riesgo_pct", riesgo}, {"max_posiciones", max_pos}, {"max_peso_pct", max_peso},
                {"reserva_pct", reserva}, {"comision_pct", com_pct}, {"comision_min_eur", com_min},
                {"deslizamiento_bps", slip * 1e4}, {"umbral", umbral}, {"fuera_muestra_pct", fuera}}}};
  for (auto* s : ss) res["simbolos"].push_back(s->simbolo);
  if (fuera > 0 && corte > 5 && curva.size() - corte > 5) {
    std::vector<double> a(curva.begin() + corte, curva.end()), b(curva_ref.begin() + corte, curva_ref.end());
    res["fuera_muestra"] = {{"desde", fecha(dia(cts[corte]))}, {"metricas", metricas(a, ppy)}, {"referencia", metricas(b, ppy)}};
  }
  size_t paso = std::max<size_t>(1, curva.size() / 400);
  json cv = json::array();
  for (size_t i = 0; i < curva.size(); i += paso)
    cv.push_back({{"ts", cts[i]}, {"total", rnd(curva[i], 2)}, {"referencia", rnd(curva_ref[i], 2)}});
  if (!curva.empty() && (curva.size() - 1) % paso)
    cv.push_back({{"ts", cts.back()}, {"total", rnd(curva.back(), 2)}, {"referencia", rnd(curva_ref.back(), 2)}});
  res["curva"] = cv;
  for (size_t i = ops.size() > 100 ? ops.size() - 100 : 0; i < ops.size(); ++i) res["ultimas_operaciones"].push_back(ops[i]);
  if (ganancias.size() < 10) res["avisos"].push_back("menos de 10 operaciones: el resultado no es estadísticamente fiable");
  return res;
}

// ---------------------------------------------------------------- optimizacion de cartera (PyPortfolioOpt)
namespace {

using Vec = std::vector<double>;
using Mat = std::vector<Vec>;

Vec mul(const Mat& m, const Vec& w) {
  Vec o(w.size(), 0.0);
  for (size_t i = 0; i < w.size(); ++i)
    for (size_t j = 0; j < w.size(); ++j) o[i] += m[i][j] * w[j];
  return o;
}
double dot(const Vec& a, const Vec& b) { return std::inner_product(a.begin(), a.end(), b.begin(), 0.0); }

// proyeccion sobre {w >= 0, w <= tope, suma 1} (biseccion sobre el umbral)
Vec proyectar(const Vec& v, double tope) {
  double lo = *std::min_element(v.begin(), v.end()) - tope - 1, hi = *std::max_element(v.begin(), v.end()) + 1;
  Vec w(v.size());
  for (int it = 0; it < 100; ++it) {
    double tau = (lo + hi) / 2, s = 0;
    for (size_t i = 0; i < v.size(); ++i) s += w[i] = std::max(0.0, std::min(tope, v[i] - tau));
    (s > 1 ? lo : hi) = tau;
  }
  double s = std::accumulate(w.begin(), w.end(), 0.0);
  if (s > 0)
    for (auto& x : w) x /= s;
  return w;
}

// escala a suma 1 y recorta al tope repartiendo el exceso en proporcion (conserva las proporciones del resto)
Vec normalizar(Vec w, double tope) {
  for (auto& x : w) x = std::max(0.0, x);
  std::vector<bool> fijo(w.size(), false);
  for (size_t it = 0; it <= w.size(); ++it) {
    double libre = 1, s = 0;
    for (size_t i = 0; i < w.size(); ++i) fijo[i] ? libre -= tope : s += w[i];
    if (s <= 0) break;
    bool cambia = false;
    for (size_t i = 0; i < w.size(); ++i) {
      if (fijo[i]) continue;
      w[i] = w[i] / s * libre;
      if (w[i] > tope + 1e-12) fijo[i] = true, cambia = true;
    }
    for (size_t i = 0; i < w.size(); ++i)
      if (fijo[i]) w[i] = tope;
    if (!cambia) break;
  }
  return w;
}

Vec min_varianza(const Mat& c, double tope) {
  size_t n = c.size();
  double L = 0;
  for (auto& f : c) {
    double s = 0;
    for (double x : f) s += std::abs(x);
    L = std::max(L, s);
  }
  Vec w(n, 1.0 / n);
  double lr = L > 0 ? 0.5 / L : 1;
  for (int it = 0; it < 3000; ++it) {
    Vec g = mul(c, w);
    for (size_t i = 0; i < n; ++i) w[i] -= lr * 2 * g[i];
    w = proyectar(w, tope);
  }
  return w;
}

Vec max_sharpe(const Mat& c, const Vec& mu, double tope) {
  size_t n = c.size();
  Vec w(n, 1.0 / n), mejor = w;
  auto sharpe = [&](const Vec& x) {
    double v = dot(x, mul(c, x));
    return v > 0 ? dot(mu, x) / std::sqrt(v) : 0.0;
  };
  double best = sharpe(w);
  for (int it = 0; it < 4000; ++it) {
    Vec cw = mul(c, w);
    double v = dot(w, cw), s = std::sqrt(std::max(v, 1e-18)), r = dot(mu, w);
    Vec g(n);
    double gmax = 1e-18;
    for (size_t i = 0; i < n; ++i) g[i] = mu[i] / s - r * cw[i] / (s * s * s), gmax = std::max(gmax, std::abs(g[i]));
    double paso = 0.05 / (1 + it / 200.0);
    for (size_t i = 0; i < n; ++i) w[i] += paso * g[i] / gmax;
    w = proyectar(w, tope);
    double sh = sharpe(w);
    if (sh > best) best = sh, mejor = w;
  }
  return mejor;
}

Vec paridad_riesgo(const Mat& c, double tope) {
  size_t n = c.size();
  Vec w(n);
  for (size_t i = 0; i < n; ++i) w[i] = c[i][i] > 0 ? 1 / std::sqrt(c[i][i]) : 1;
  w = normalizar(w, 1.0);
  for (int it = 0; it < 1000; ++it) {
    Vec cw = mul(c, w);
    double v = dot(w, cw);
    if (v <= 0) break;
    for (size_t i = 0; i < n; ++i) {
      double rc = w[i] * cw[i] / v;
      if (rc > 0) w[i] *= std::sqrt((1.0 / n) / rc);
    }
    w = normalizar(w, 1.0);
  }
  return normalizar(w, tope);
}

// HRP de Lopez de Prado: agrupa por correlacion (enlace simple), ordena y reparte por biseccion recursiva
Vec hrp(const Mat& c) {
  size_t n = c.size();
  Mat dist(n, Vec(n, 0.0));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < n; ++j) {
      double den = std::sqrt(c[i][i] * c[j][j]);
      double rho = den > 0 ? std::max(-1.0, std::min(1.0, c[i][j] / den)) : 0;
      dist[i][j] = std::sqrt(std::max(0.0, 0.5 * (1 - rho)));
    }
  std::vector<std::vector<size_t>> grupos;
  for (size_t i = 0; i < n; ++i) grupos.push_back({i});
  while (grupos.size() > 1) {   // une los dos grupos mas cercanos; el orden de las hojas queda cuasi diagonal
    size_t a = 0, b = 1;
    double best = 1e300;
    for (size_t i = 0; i < grupos.size(); ++i)
      for (size_t j = i + 1; j < grupos.size(); ++j) {
        double dmin = 1e300;
        for (size_t x : grupos[i])
          for (size_t y : grupos[j]) dmin = std::min(dmin, dist[x][y]);
        if (dmin < best) best = dmin, a = i, b = j;
      }
    grupos[a].insert(grupos[a].end(), grupos[b].begin(), grupos[b].end());
    grupos.erase(grupos.begin() + b);
  }
  std::vector<size_t> orden = grupos[0];
  auto var_grupo = [&](const std::vector<size_t>& g) {
    Vec iv;
    for (size_t i : g) iv.push_back(c[i][i] > 0 ? 1 / c[i][i] : 0);
    double s = std::accumulate(iv.begin(), iv.end(), 0.0);
    if (s <= 0) return 0.0;
    double v = 0;
    for (size_t x = 0; x < g.size(); ++x)
      for (size_t y = 0; y < g.size(); ++y) v += iv[x] / s * iv[y] / s * c[g[x]][g[y]];
    return v;
  };
  Vec w(n, 1.0);
  std::function<void(std::vector<size_t>)> partir = [&](std::vector<size_t> g) {
    if (g.size() < 2) return;
    std::vector<size_t> izq(g.begin(), g.begin() + g.size() / 2), der(g.begin() + g.size() / 2, g.end());
    double vi = var_grupo(izq), vd = var_grupo(der);
    double alfa = vi + vd > 0 ? 1 - vi / (vi + vd) : 0.5;
    for (size_t i : izq) w[i] *= alfa;
    for (size_t i : der) w[i] *= 1 - alfa;
    partir(izq);
    partir(der);
  };
  partir(orden);
  return w;
}

}  // namespace

json optimizar(const std::vector<SerieBT>& series, const std::string& metodo, const json& prm) {
  static const std::set<std::string> MET = {"igual", "inversa_vol", "min_varianza", "paridad_riesgo", "max_sharpe", "hrp"};
  if (!MET.count(metodo)) throw std::invalid_argument("método desconocido: " + metodo);
  if (series.size() < 2) throw std::invalid_argument("hacen falta al menos dos valores");
  // sesiones comunes a todos los valores
  std::map<long, std::vector<double>> filas;
  for (size_t k = 0; k < series.size(); ++k)
    for (size_t i = 0; i < series[k].ts.size(); ++i) {
      auto& f = filas[dia(series[k].ts[i])];
      f.resize(series.size(), 0.0);
      f[k] = series[k].precio[i];
    }
  std::vector<std::vector<double>> px;
  for (auto& [d, f] : filas)
    if (std::all_of(f.begin(), f.end(), [](double x) { return x > 0; })) px.push_back(f);
  if (px.size() < 30) throw std::invalid_argument("menos de 30 sesiones comunes entre los valores");
  size_t n = series.size(), T = px.size() - 1;
  Mat r(T, Vec(n));
  for (size_t t = 0; t < T; ++t)
    for (size_t i = 0; i < n; ++i) r[t][i] = px[t + 1][i] / px[t][i] - 1;
  Vec mu(n, 0.0);
  for (auto& f : r)
    for (size_t i = 0; i < n; ++i) mu[i] += f[i] / T;
  Mat c(n, Vec(n, 0.0));
  for (auto& f : r)
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < n; ++j) c[i][j] += (f[i] - mu[i]) * (f[j] - mu[j]) / (T - 1);
  for (size_t i = 0; i < n; ++i)   // encogimiento suave hacia la diagonal (covarianza mas estable)
    for (size_t j = 0; j < n; ++j)
      if (i != j) c[i][j] *= 0.9;
  std::vector<double> tss;
  for (auto& [d, f] : filas) tss.push_back(d * 86400.0);
  double ppy = periodos_anio(series[0].ts);
  double tope = std::max(1.0 / n, std::min(1.0, pnum(prm, "max_peso_pct", 100) / 100));
  json avisos = json::array();
  Vec w;
  if (metodo == "igual") w = Vec(n, 1.0 / n);
  else if (metodo == "inversa_vol") {
    for (size_t i = 0; i < n; ++i) w.push_back(c[i][i] > 0 ? 1 / std::sqrt(c[i][i]) : 0);
    w = normalizar(w, tope);
  } else if (metodo == "min_varianza") w = min_varianza(c, tope);
  else if (metodo == "paridad_riesgo") w = paridad_riesgo(c, tope);
  else if (metodo == "hrp") w = normalizar(hrp(c), tope);
  else {
    if (std::all_of(mu.begin(), mu.end(), [](double x) { return x <= 0; })) {
      avisos.push_back("ningún valor tuvo rentabilidad media positiva: se usa la varianza mínima");
      w = min_varianza(c, tope);
    } else {
      w = max_sharpe(c, mu, tope);
    }
  }
  Vec cw = mul(c, w);
  double var = dot(w, cw), ret = dot(mu, w);
  json pesos = json::object(), contrib = json::object();
  for (size_t i = 0; i < n; ++i) {
    pesos[series[i].simbolo] = rnd(w[i] * 100, 2);
    contrib[series[i].simbolo] = var > 0 ? rnd(w[i] * cw[i] / var * 100, 2) : 0.0;
  }
  json out = {{"metodo", metodo},
              {"sesiones", px.size()},
              {"pesos_pct", pesos},
              {"contribucion_riesgo_pct", contrib},
              {"rentabilidad_anual_pct", rnd(ret * ppy * 100, 2)},
              {"volatilidad_anual_pct", rnd(std::sqrt(std::max(0.0, var * ppy)) * 100, 2)},
              {"sharpe", var > 0 ? rnd(ret / std::sqrt(var) * std::sqrt(ppy), 2) : 0.0},
              {"avisos", avisos},
              {"nota", "Pesos calculados con el pasado: no garantizan el futuro. Sin coste de operar ni impuestos."}};
  double cap = pnum(prm, "capital", 0);
  if (cap > 0) {   // asignacion en titulos enteros (como DiscreteAllocation de PyPortfolioOpt, voraz)
    Vec precio(n);
    for (size_t i = 0; i < n; ++i) precio[i] = series[i].precio.back() / series[i].fx;
    std::vector<double> q(n, 0);
    double caja = cap;
    for (size_t i = 0; i < n; ++i) {
      bool frac = series[i].simbolo.find('-') != std::string::npos;
      q[i] = cap * w[i] / precio[i];
      q[i] = frac ? std::floor(q[i] * 1e4) / 1e4 : std::floor(q[i]);
      caja -= q[i] * precio[i];
    }
    for (int it = 0; it < 1000; ++it) {   // el sobrante, al valor que mas se aleja de su peso y quepa
      long best = -1;
      double falta = 0;
      for (size_t i = 0; i < n; ++i) {
        double f = w[i] - q[i] * precio[i] / cap;
        if (precio[i] <= caja && f > falta) falta = f, best = (long)i;
      }
      if (best < 0) break;
      q[best] += 1;
      caja -= precio[best];
    }
    json asig = json::object();
    for (size_t i = 0; i < n; ++i)
      asig[series[i].simbolo] = {{"titulos", q[i]}, {"precio_eur", rnd(precio[i], 4)}, {"importe_eur", rnd(q[i] * precio[i], 2)}};
    out["asignacion"] = asig;
    out["sobrante_eur"] = rnd(caja, 2);
  }
  return out;
}

// ---------------------------------------------------------------- Monte Carlo
json montecarlo(const std::vector<double>& rs, int sims, int horizonte, double capital, uint64_t semilla) {
  if (rs.size() < 10) throw std::invalid_argument("hacen falta al menos 10 rentabilidades");
  sims = std::max(100, std::min(20000, sims));
  horizonte = std::max(5, std::min(2520, horizonte));
  std::mt19937_64 rng(semilla);
  std::uniform_int_distribution<size_t> pick(0, rs.size() - 1);
  std::vector<double> finales, dds;
  for (int s = 0; s < sims; ++s) {
    double v = capital, pico = capital, dd = 0;
    for (int t = 0; t < horizonte; ++t) {
      v *= 1 + rs[pick(rng)];
      pico = std::max(pico, v);
      dd = std::max(dd, (pico - v) / pico);
    }
    finales.push_back(v);
    dds.push_back(dd);
  }
  json pct = json::object();
  for (auto [k, p] : std::vector<std::pair<const char*, double>>{{"p5", .05}, {"p25", .25}, {"p50", .5}, {"p75", .75}, {"p95", .95}})
    pct[k] = rnd(percentil(finales, p), 2);
  auto prob = [&](const std::vector<double>& v, auto f) {
    return rnd(100.0 * std::count_if(v.begin(), v.end(), f) / v.size(), 1);
  };
  return {{"simulaciones", sims},
          {"horizonte", horizonte},
          {"capital", capital},
          {"valor_final", pct},
          {"prob_perdida_pct", prob(finales, [&](double x) { return x < capital; })},
          {"prob_perder_20_pct", prob(finales, [&](double x) { return x < capital * 0.8; })},
          {"drawdown_mediano_pct", rnd(percentil(dds, 0.5) * 100, 2)},
          {"drawdown_p95_pct", rnd(percentil(dds, 0.95) * 100, 2)},
          {"prob_drawdown_20_pct", prob(dds, [](double x) { return x >= 0.2; })},
          {"nota", "Remuestreo de las rentabilidades pasadas: supone que el futuro se parece al pasado."}};
}

// ---------------------------------------------------------------- protecciones (freqtrade)
json protecciones(const json& curva, const json& cerradas, const json& aj, double ahora) {
  double max_dd = pnum(aj, "max_drawdown_pct", 15), perd_dia = pnum(aj, "perdida_diaria_pct", 3);
  long racha = (long)pnum(aj, "racha_perdidas", 4);
  double pausa = pnum(aj, "pausa_racha_h", 24);
  json motivos = json::array();
  if (curva.is_array() && !curva.empty()) {
    double cur = jnum(curva.back()["total_eur"]);
    if (max_dd > 0) {
      double pico = 0;
      for (auto& c : curva)
        if (jnum(c["ts"]) >= ahora - 30 * 86400.0) pico = std::max(pico, jnum(c["total_eur"]));
      double dd = pico > 0 ? (pico - cur) / pico * 100 : 0;
      if (dd >= max_dd)
        motivos.push_back("la cartera cae un " + std::to_string((int)std::round(dd)) + " % desde su máximo de 30 días (freno en " +
                          std::to_string((int)max_dd) + " %)");
    }
    if (perd_dia > 0) {
      double ini = 0;
      for (auto& c : curva)
        if (jnum(c["ts"]) >= ahora - 86400.0) {
          ini = jnum(c["total_eur"]);
          break;
        }
      if (ini > 0 && (ini - cur) / ini * 100 >= perd_dia)
        motivos.push_back("pierde más de un " + std::to_string((int)perd_dia) + " % en las últimas 24 horas");
    }
  }
  if (racha > 0 && cerradas.is_array() && (long)cerradas.size() >= racha) {
    bool todas = true;
    for (size_t i = cerradas.size() - racha; i < cerradas.size(); ++i) todas &= jnum(cerradas[i]["ganancia_eur"]) < 0;
    if (todas && ahora - jnum(cerradas.back()["ts"]) < pausa * 3600)
      motivos.push_back(std::to_string(racha) + " operaciones perdedoras seguidas: pausa de " + std::to_string((int)pausa) + " h");
  }
  return {{"bloqueado", !motivos.empty()}, {"motivos", motivos}};
}

}  // namespace nx::bolsa
