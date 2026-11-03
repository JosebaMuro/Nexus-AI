// Mesa de analistas con IA (ver bolsa_mesa.h y docs/BOLSA_PROYECTOS.md).
#include "office/bolsa_mesa.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

#include "office/bolsa_agente.h"
#include "office/bolsa_cuant.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::bolsa {

namespace {

auto L = log::get("bolsa.mesa");

const char* SCHEMA = R"(
CREATE TABLE IF NOT EXISTS bolsa_noticias (simbolo TEXT, enlace TEXT, ts REAL, titulo TEXT, fuente TEXT,
    sentimiento REAL, PRIMARY KEY (simbolo, enlace));
CREATE TABLE IF NOT EXISTS bolsa_noticias_ok (simbolo TEXT PRIMARY KEY, ts REAL);
CREATE TABLE IF NOT EXISTS bolsa_informes (id INTEGER PRIMARY KEY, ts REAL, simbolo TEXT, decision TEXT, informe TEXT);
)";

const char* MARCO =
    "Trabajas en una mesa de bolsa de una oficina. Escribe en español, claro y breve. Usa SOLO los datos que se dan; "
    "no inventes cifras, noticias ni hechos. Esto no es asesoramiento financiero.\n\n";

double rnd(double x, int d) {
  double m = std::pow(10.0, d);
  return std::round(x * m) / m;
}
double clamp1(double v) { return std::max(-1.0, std::min(1.0, v)); }
std::string f2(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}
std::string pct1(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%+.1f %%", v);
  return b;
}

std::string entidades(std::string s) {
  if (s.rfind("<![CDATA[", 0) == 0 && s.size() >= 12) s = s.substr(9, s.size() - 12);
  static const std::vector<std::pair<std::string, std::string>> E = {
      {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&#x27;", "'"}, {"&nbsp;", " "}};
  for (auto& [a, b] : E)
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
  return trim(s);
}

std::string etiqueta(const std::string& item, const std::string& tag) {
  size_t a = item.find("<" + tag);
  if (a == std::string::npos) return "";
  a = item.find('>', a);
  size_t b = item.find("</" + tag + ">", a);
  if (a == std::string::npos || b == std::string::npos) return "";
  return entidades(item.substr(a + 1, b - a - 1));
}

double fecha_rss(const std::string& s) {   // "Tue, 29 Oct 2026 14:00:00 GMT" / "+0000"
  static const char* M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int d = 0, y = 0, h = 0, mi = 0, se = 0;
  char mon[8] = {0}, zona[8] = {0};
  const char* p = s.c_str();
  if (const char* c = std::strchr(p, ',')) p = c + 1;
  if (std::sscanf(p, " %d %3s %d %d:%d:%d %7s", &d, mon, &y, &h, &mi, &se, zona) < 6) return 0;
  int m = -1;
  for (int i = 0; i < 12; ++i)
    if (std::string(mon) == M[i]) m = i;
  if (m < 0) return 0;
  std::tm tm{};
  tm.tm_year = y - 1900, tm.tm_mon = m, tm.tm_mday = d, tm.tm_hour = h, tm.tm_min = mi, tm.tm_sec = se;
#ifdef _WIN32
  double t = (double)_mkgmtime(&tm);
#else
  double t = (double)timegm(&tm);
#endif
  int off = 0;
  if ((zona[0] == '+' || zona[0] == '-') && std::strlen(zona) == 5) {
    off = ((zona[1] - '0') * 10 + (zona[2] - '0')) * 3600 + ((zona[3] - '0') * 10 + (zona[4] - '0')) * 60;
    if (zona[0] == '+') off = -off;
  }
  return t + off;
}

std::string decision_de(double s) { return s > 0.2 ? "comprar" : s < -0.2 ? "vender" : "mantener"; }

}  // namespace

// ---------------------------------------------------------------- RSS y sentimiento
json leer_rss(const std::string& xml) {
  json out = json::array();
  size_t p = 0;
  while ((p = xml.find("<item", p)) != std::string::npos) {
    size_t f = xml.find("</item>", p);
    if (f == std::string::npos) break;
    std::string it = xml.substr(p, f - p);
    p = f + 7;
    std::string t = etiqueta(it, "title");
    if (t.empty()) continue;
    std::string fecha = etiqueta(it, "pubDate");
    out.push_back({{"titulo", t}, {"enlace", etiqueta(it, "link")}, {"fecha", fecha}, {"ts", fecha_rss(fecha)},
                   {"fuente", etiqueta(it, "source")}});
  }
  return out;
}

double sentimiento(const std::string& texto) {
  static const std::set<std::string> POS = {
      "sube", "suben", "subida", "alza", "alzas", "dispara", "disparan", "gana", "ganan", "ganancia", "ganancias",
      "beneficio", "beneficios", "récord", "record", "mejora", "mejoras", "mejoran", "crece", "crecen", "crecimiento",
      "supera", "superan", "rebota", "rebote", "dividendo", "recompra", "optimismo", "optimista", "fuerte", "firma",
      "adquiere", "aprueba", "eleva", "repunta", "máximos", "maximos", "positivo", "positiva",
      "beats", "beat", "surge", "surges", "soar", "soars", "rally", "rallies", "gain", "gains", "jump", "jumps",
      "record", "upgrade", "upgrades", "upgraded", "growth", "profit", "profits", "strong", "bullish", "outperform",
      "raises", "raise", "buyback", "dividend", "rise", "rises", "rising", "higher", "boost", "boosts", "approval", "wins"};
  static const std::set<std::string> NEG = {
      "cae", "caen", "caída", "caida", "baja", "bajan", "desploma", "desploman", "pierde", "pierden", "pérdida",
      "perdida", "pérdidas", "perdidas", "recorta", "recorte", "rebaja", "rebajan", "multa", "demanda", "investigación",
      "investigacion", "quiebra", "despidos", "despide", "huelga", "advierte", "alerta", "riesgo", "débil", "debil",
      "mínimos", "minimos", "negativo", "negativa", "hunde", "hunden", "retrocede", "fraude", "sanción", "sancion",
      "falls", "fall", "drop", "drops", "plunge", "plunges", "slump", "slumps", "loss", "losses", "miss", "misses",
      "missed", "downgrade", "downgrades", "downgraded", "lawsuit", "probe", "fine", "fined", "bankruptcy", "layoffs",
      "weak", "bearish", "underperform", "cuts", "cut", "warning", "warns", "lower", "decline", "declines", "sell-off",
      "selloff", "recall", "fraud", "tumble", "tumbles", "sinks", "sink"};
  static const std::set<std::string> NO = {"no", "not", "sin", "never", "nunca", "ni"};
  std::string t = lower(texto), w;
  int pos = 0, neg = 0;
  bool niega = false;
  auto fin = [&] {
    if (w.empty()) return;
    int s = POS.count(w) ? 1 : NEG.count(w) ? -1 : 0;
    if (niega) s = -s;
    if (s > 0) ++pos;
    if (s < 0) ++neg;
    niega = NO.count(w) > 0;
    w.clear();
  };
  for (unsigned char c : t) {
    if (std::isalnum(c) || c >= 0x80 || c == '-') w += (char)c;
    else fin();
  }
  fin();
  if (!pos && !neg) return 0;
  return rnd((double)(pos - neg) / (pos + neg) * std::min(1.0, (pos + neg) / 2.0), 3);
}

json extraer_json(const std::string& r) {
  size_t fin = r.rfind('}');
  while (fin != std::string::npos) {
    size_t ini = r.rfind('{', fin);
    while (ini != std::string::npos) {
      json j = jparse(r.substr(ini, fin - ini + 1));
      if (!j.is_discarded() && j.is_object() && !j.empty()) return j;
      if (ini == 0) break;
      ini = r.rfind('{', ini - 1);
    }
    if (fin == 0) break;
    fin = r.rfind('}', fin - 1);
  }
  return nullptr;
}

// ---------------------------------------------------------------- Mesa
Mesa::Mesa(Bolsa& mesa, Fetch fetch) : b(mesa), fetch_(fetch ? std::move(fetch) : Fetch(http::send)) {
  std::lock_guard<std::recursive_mutex> lk(b.mutex());
  b.db().script(SCHEMA);
}

json Mesa::noticias(const std::string& simbolo, bool refrescar) {
  json inst = b.instrumento(simbolo);
  std::string sym = jstr(inst, "simbolo");
  auto ok = b.db().one("SELECT ts FROM bolsa_noticias_ok WHERE simbolo=?", {sym});
  bool fresco = ok && b.now() - jnum((*ok)["ts"]) < ttl_noticias;
  json errores = json::array();
  if (refrescar || !fresco) {
    std::string base = sym.substr(0, sym.find('.'));
    std::string q = jstr(inst, "nombre", base);
    std::vector<std::pair<std::string, std::string>> fuentes = {
        {"Yahoo Finance", "https://feeds.finance.yahoo.com/rss/2.0/headline?" +
                              http::query_string({{"s", sym}, {"region", "US"}, {"lang", "en-US"}})},
        {"Google Noticias", "https://news.google.com/rss/search?" +
                                http::query_string({{"q", q + " bolsa"}, {"hl", "es"}, {"gl", "ES"}, {"ceid", "ES:es"}})}};
    int nuevas = 0;
    for (auto& [nombre, url] : fuentes) {
      http::Request req;
      req.url = url;
      req.headers = {{"User-Agent", "Mozilla/5.0 (Nexus-Office)"}};
      req.timeout = 8;
      http::Response r;
      try {
        r = fetch_(req);
      } catch (const std::exception& e) {
        r.error = e.what();
      }
      if (!r.ok()) {
        errores.push_back(nombre + ": " + (r.status ? "HTTP " + std::to_string(r.status) : r.error.empty() ? "sin conexión" : r.error));
        continue;
      }
      std::vector<Params> filas;
      for (auto& n : leer_rss(r.body)) {
        double ts = jnum(n["ts"]);
        std::string enlace = jstr(n, "enlace").empty() ? jstr(n, "titulo") : jstr(n, "enlace");
        filas.push_back({sym, enlace, ts > 0 ? ts : b.now(), jstr(n, "titulo"),
                         jstr(n, "fuente").empty() ? nombre : jstr(n, "fuente"), sentimiento(jstr(n, "titulo"))});
      }
      std::lock_guard<std::recursive_mutex> lk(b.mutex());
      b.db().executemany("INSERT OR IGNORE INTO bolsa_noticias VALUES (?,?,?,?,?,?)", filas);
      nuevas += (int)filas.size();
    }
    if (errores.size() < fuentes.size()) {
      std::lock_guard<std::recursive_mutex> lk(b.mutex());
      b.db().execute("INSERT OR REPLACE INTO bolsa_noticias_ok VALUES (?,?)", {sym, b.now()});
      b.db().execute("DELETE FROM bolsa_noticias WHERE simbolo=? AND ts < ?", {sym, b.now() - 120 * 86400.0});
    }
  }
  json lista = json::array();
  double suma = 0, peso = 0;
  for (auto& r : b.db().query("SELECT ts, titulo, enlace, fuente, sentimiento FROM bolsa_noticias WHERE simbolo=? "
                              "ORDER BY ts DESC LIMIT 30",
                              {sym})) {
    double edad_d = std::max(0.0, (b.now() - jnum(r["ts"])) / 86400.0);
    double w = std::exp(-edad_d / 7);   // lo reciente pesa mas (vida media ~5 dias)
    suma += w * jnum(r["sentimiento"]);
    peso += w;
    lista.push_back(r);
  }
  return {{"simbolo", sym},
          {"noticias", lista},
          {"sentimiento", peso > 0 ? rnd(suma / peso, 3) : 0.0},
          {"en_linea", errores.size() < 2},
          {"errores", errores}};
}

json Mesa::lecciones(const std::string& simbolo, int limite) {
  json out = json::array();
  try {
    for (auto& r : b.db().query("SELECT ts, tipo, texto FROM agente_diario WHERE simbolo=? AND tipo IN ('venta','stop') "
                                "ORDER BY id DESC LIMIT ?",
                                {simbolo, limite}))
      out.push_back(r);
  } catch (const std::exception&) {   // el agente aun no ha creado su diario
  }
  return out;
}

std::string Mesa::preguntar(const std::string& prompt, json& traza, const std::string& papel) {
  if (!llm || traza.value("sin_ia", false)) return "";
  std::string r;
  try {
    r = trim(llm(std::string(MARCO) + prompt));
  } catch (const std::exception& e) {
    L.debug("IA no disponible para la mesa: ", e.what());
  }
  if (r.empty()) traza["sin_ia"] = true;   // si la primera falla no se insiste con las demas
  else traza["papeles_ia"].push_back(papel);
  return r;
}

json Mesa::informe(const std::string& simbolo, const json& op) {
  json inst = b.instrumento(simbolo);
  std::string sym = jstr(inst, "simbolo");
  json pts = b.serie(sym, "1y")["puntos"];
  if (pts.size() < 60) pts = b.serie(sym, "6mo")["puntos"];
  if (pts.size() < 60) pts = b.serie(sym, "1d")["puntos"];
  json an = analizar(pts), sg = senales(pts);
  json q = b.precio(sym);
  json nt;
  try {
    nt = noticias(sym);
  } catch (const std::exception& e) {
    nt = {{"noticias", json::array()}, {"sentimiento", 0.0}, {"errores", json::array({e.what()})}};
  }
  double px = jnum(q["precio"]);
  double hi = px, lo = px;
  for (auto& p : pts) hi = std::max(hi, jnum(p["precio"])), lo = std::min(lo, jnum(p["precio"]));
  double rango = hi > lo ? (px - lo) / (hi - lo) * 100 : 50;
  double vol = an.is_object() ? jnum(an["vol_diaria"]) : 0.02;
  double atr_v = an.is_object() ? jnum(an["atr"]) : px * vol;
  double riesgo_pct = jnum(op.value("riesgo_pct", json(1.0)), 1.0), max_peso = jnum(op.value("max_peso_pct", json(20.0)), 20.0);
  int rondas = std::max(1, std::min(3, (int)jnum(op.value("rondas", json(1)), 1)));
  json traza = {{"sin_ia", false}, {"papeles_ia", json::array()}};

  // datos comunes que ven todos los papeles
  json titulares = json::array();
  for (auto& n : nt["noticias"]) {
    if (titulares.size() >= 8) break;
    titulares.push_back(jstr(n, "titulo") + " (" + jstr(n, "fuente") + ")");
  }
  json datos = {{"simbolo", sym},
                {"nombre", inst.value("nombre", json(sym))},
                {"mercado", inst.value("mercado", json(""))},
                {"divisa", inst.value("divisa", json(""))},
                {"precio", px},
                {"datos_reales", q.value("real", false)},
                {"rango_1a_pct", rnd(rango, 1)},
                {"maximo_periodo", hi},
                {"minimo_periodo", lo}};
  if (an.is_object())
    datos["tecnico"] = {{"sma20", rnd(jnum(an["sma20"]), 4)}, {"sma50", rnd(jnum(an["sma50"]), 4)}, {"rsi", an["rsi"]},
                        {"roc20_pct", rnd(jnum(an["roc20"]) * 100, 2)}, {"vol_diaria_pct", rnd(vol * 100, 2)},
                        {"atr", rnd(atr_v, 4)}};
  if (sg.is_object()) {
    json e = json::object();
    for (auto it = sg["estrategias"].begin(); it != sg["estrategias"].end(); ++it)
      e[it.key()] = {{"senal", it.value()["senal"]}, {"confianza", it.value()["confianza"]}};
    datos["senales"] = {{"combinada", sg["combinada"]}, {"lectura", sg["lectura"]}, {"estrategias", e},
                        {"macd", sg["indicadores"]["macd"]}, {"bollinger", sg["indicadores"]["bollinger"]}};
  }
  std::string D = datos.dump(1);

  // 1. analistas -------------------------------------------------
  double s_tec = sg.is_object() ? jnum(sg["combinada"]) : an.is_object() ? clamp1(jnum(an["factores"]["tendencia"]) * 0.5 + jnum(an["factores"]["momento"]) * 0.5) : 0;
  std::string t_tec;
  if (sg.is_object()) {
    auto& es = sg["estrategias"];
    t_tec = "Lectura " + jstr(sg, "lectura") + " (" + f2(s_tec) + "): tendencia " + f2(jnum(es["tendencia"]["senal"])) +
            ", reversión " + f2(jnum(es["reversion"]["senal"])) + ", momento " + f2(jnum(es["momento"]["senal"])) +
            ", volatilidad " + f2(jnum(es["volatilidad"]["senal"])) + ", estadística " + f2(jnum(es["estadistica"]["senal"])) + ".";
  } else {
    t_tec = "Historia corta: lectura técnica con pocas sesiones.";
  }
  if (an.is_object() && an["rsi"].is_number()) t_tec += " RSI " + f2(jnum(an["rsi"])) + ", 20 sesiones " + pct1(jnum(an["roc20"]) * 100) + ".";
  std::string r = preguntar("Eres el ANALISTA TÉCNICO. Analiza tendencia, momento, volatilidad y niveles de este valor en "
                            "menos de 120 palabras y termina con tu lectura (alcista, neutral o bajista).\n\nDatos:\n" + D,
                            traza, "tecnico");
  json a_tec = {{"senal", rnd(s_tec, 3)}, {"texto", r.empty() ? t_tec : r}, {"ia", !r.empty()}};

  double s_not = jnum(nt["sentimiento"]);
  std::string t_not = nt["noticias"].empty()
                          ? "Sin titulares guardados (sin conexión o sin noticias recientes)."
                          : std::to_string(nt["noticias"].size()) + " titulares; sentimiento " + f2(s_not) +
                                (s_not > 0.15 ? " (favorable)" : s_not < -0.15 ? " (desfavorable)" : " (neutro)") + ".";
  r = titulares.empty() ? "" : preguntar("Eres el ANALISTA DE NOTICIAS Y SENTIMIENTO. Resume en menos de 100 palabras qué "
                                         "dicen estos titulares sobre el valor y si el tono es favorable, neutro o desfavorable.\n\n"
                                         "Valor: " + jstr(datos, "nombre") + " (" + sym + ")\nTitulares:\n" + titulares.dump(1),
                                         traza, "noticias");
  json a_not = {{"senal", s_not}, {"texto", r.empty() ? t_not : r}, {"titulares", titulares}, {"ia", !r.empty()}};

  r = preguntar("Eres el ANALISTA FUNDAMENTAL. No tienes datos contables en vivo: con lo que sepas del negocio de esta "
                "empresa o activo (sector, modelo de negocio, riesgos conocidos), comenta en menos de 100 palabras su "
                "calidad y valoración, y AVISA de que tu conocimiento puede estar desfasado. Termina con un JSON "
                "{\"senal\": número entre -1 y 1}.\n\nValor: " + jstr(datos, "nombre") + " (" + sym + "), mercado " +
                jstr(datos, "mercado") + ", precio " + f2(px) + " " + jstr(datos, "divisa") + ", posición en su rango "
                "del último año: " + f2(rango) + " %.",
                traza, "fundamental");
  json fj = r.empty() ? json() : extraer_json(r);
  double s_fun = fj.is_object() && fj.contains("senal") ? clamp1(jnum(fj["senal"])) : 0.0;
  json a_fun = {{"senal", rnd(s_fun, 3)},
                {"texto", r.empty() ? "Sin datos contables en local ni IA instalada: no se valora el negocio. El precio está "
                                      "en el " + f2(rango) + " % de su rango del último año."
                                    : r},
                {"ia", !r.empty()},
                {"aviso", "conocimiento del modelo, puede estar desfasado"}};
  json analistas = {{"tecnico", a_tec}, {"noticias", a_not}, {"fundamental", a_fun}};

  // 2. debate alcista / bajista ----------------------------------
  json pro = json::array(), contra = json::array();
  if (an.is_object()) {
    (jnum(an["factores"]["tendencia"]) > 0 ? pro : contra)
        .push_back(std::string("El precio está ") + (px > jnum(an["sma50"]) ? "sobre" : "bajo") + " su media de 50 sesiones");
    (jnum(an["roc20"]) > 0 ? pro : contra).push_back("Momento de 20 sesiones " + pct1(jnum(an["roc20"]) * 100));
    if (an["rsi"].is_number()) {
      double rs = jnum(an["rsi"]);
      if (rs > 70) contra.push_back("RSI " + f2(rs) + ": sobrecompra");
      else if (rs < 30) pro.push_back("RSI " + f2(rs) + ": sobreventa, posible rebote");
    }
    if (vol > 0.03) contra.push_back("Volatilidad diaria alta (" + f2(vol * 100) + " %)");
  }
  if (s_not > 0.15) pro.push_back("Titulares recientes favorables");
  if (s_not < -0.15) contra.push_back("Titulares recientes desfavorables");
  if (rango > 90) contra.push_back("Cerca de máximos del periodo");
  if (rango < 10) pro.push_back("Cerca de mínimos del periodo (descuento)");
  json debate = json::array();
  std::string informes_txt = "Técnico: " + jstr(a_tec, "texto") + "\nNoticias: " + jstr(a_not, "texto") +
                             "\nFundamental: " + jstr(a_fun, "texto");
  std::string ultimo_bajista;
  for (int i = 0; i < rondas; ++i) {
    std::string al = preguntar("Eres el INVESTIGADOR ALCISTA. Con los informes de los analistas, defiende comprar este valor "
                               "en menos de 90 palabras" + std::string(ultimo_bajista.empty() ? "" : " y rebate al bajista") +
                               ".\n\nInformes:\n" + informes_txt +
                               (ultimo_bajista.empty() ? "" : "\n\nBajista:\n" + ultimo_bajista),
                               traza, "alcista");
    std::string ba = preguntar("Eres el INVESTIGADOR BAJISTA. Con los informes de los analistas, señala los riesgos y por qué "
                               "no comprar en menos de 90 palabras, rebatiendo al alcista.\n\nInformes:\n" + informes_txt +
                               "\n\nAlcista:\n" + al,
                               traza, "bajista");
    if (al.empty()) al = pro.empty() ? "No hay argumentos técnicos claros a favor." : "A favor: " + join(pro, "; ") + ".";
    if (ba.empty()) ba = contra.empty() ? "No hay señales de riesgo destacadas." : "En contra: " + join(contra, "; ") + ".";
    debate.push_back({{"ronda", i + 1}, {"alcista", al}, {"bajista", ba}});
    ultimo_bajista = ba;
  }

  // 3. operador ---------------------------------------------------
  double puntos = 0.6 * s_tec + 0.25 * s_not + 0.15 * s_fun;
  std::string d_reglas = decision_de(puntos);
  r = preguntar("Eres el OPERADOR. Con los informes y el debate, propone UNA acción para este valor.\n\nInformes:\n" +
                    informes_txt + "\n\nDebate:\n" + debate.dump(1) +
                    "\n\nResponde con una frase y un JSON {\"decision\": \"comprar\"|\"mantener\"|\"vender\", \"motivo\": \"...\"}",
                traza, "operador");
  json oj = r.empty() ? json() : extraer_json(r);
  std::string d_op = oj.is_object() ? lower(jstr(oj, "decision")) : "";
  if (d_op != "comprar" && d_op != "mantener" && d_op != "vender") d_op = d_reglas;
  json operador = {{"decision", d_op},
                   {"motivo", oj.is_object() && !jstr(oj, "motivo").empty() ? jstr(oj, "motivo")
                                                                            : "Puntuación conjunta " + f2(puntos) +
                                                                                  " (60 % técnico, 25 % noticias, 15 % fundamental)."},
                   {"puntuacion", rnd(puntos, 3)},
                   {"ia", oj.is_object()}};

  // 4. equipo de riesgo ------------------------------------------
  double base = px > 0 && atr_v > 0 ? std::min(max_peso, riesgo_pct * px / (2 * atr_v)) : max_peso / 2;
  json riesgo = {{"agresivo", {{"peso_pct", rnd(std::min(max_peso, base * 1.5), 1)},
                               {"motivo", "Acepta más volatilidad para no perder la oportunidad."}}},
                 {"neutral", {{"peso_pct", rnd(base, 1)},
                              {"motivo", "Arriesga el " + f2(riesgo_pct) + " % del patrimonio con un stop a 2 ATR."}}},
                 {"conservador", {{"peso_pct", rnd(base * 0.5, 1)},
                                  {"motivo", "Mitad de tamaño: prioriza no perder capital."}}}};
  r = preguntar("Eres el EQUIPO DE RIESGO (tres voces: agresiva, neutral y conservadora). El operador propone: " +
                    operador.dump() + ". Volatilidad diaria " + f2(vol * 100) + " %, ATR " + f2(atr_v) + ", peso máximo " +
                    f2(max_peso) + " %. Responde SOLO con un JSON {\"agresivo\": {\"peso_pct\": n, \"motivo\": \"...\"}, "
                    "\"neutral\": {...}, \"conservador\": {...}} con pesos en % del patrimonio.",
                traza, "riesgo");
  json rj = r.empty() ? json() : extraer_json(r);
  if (rj.is_object())
    for (auto* k : {"agresivo", "neutral", "conservador"})
      if (rj.contains(k) && rj[k].is_object() && rj[k].contains("peso_pct")) {
        double p = std::max(0.0, std::min(max_peso, jnum(rj[k]["peso_pct"])));
        riesgo[k] = {{"peso_pct", rnd(p, 1)}, {"motivo", jstr(rj[k], "motivo", jstr(riesgo[k], "motivo"))}, {"ia", true}};
      }

  // 5. gestor de cartera -----------------------------------------
  json lecc = lecciones(sym);
  std::string perfil = vol > 0.035 ? "conservador" : "neutral";
  json dec = {{"decision", d_op},
              {"confianza", rnd(std::min(1.0, std::abs(puntos) + (sg.is_object() ? jnum(sg["confianza"]) * 0.3 : 0)), 2)},
              {"peso_pct", d_op == "comprar" ? jnum(riesgo[perfil]["peso_pct"]) : 0.0},
              {"perfil_riesgo", perfil},
              {"motivo", jstr(operador, "motivo")}};
  r = preguntar("Eres el GESTOR DE CARTERA y tienes la última palabra. Revisa el informe completo y las lecciones de "
                "operaciones pasadas con este valor, y decide.\n\nInformes:\n" + informes_txt + "\n\nDebate:\n" +
                    debate.dump(1) + "\n\nOperador: " + operador.dump() + "\nRiesgo: " + riesgo.dump() +
                    "\nLecciones: " + (lecc.empty() ? std::string("ninguna") : lecc.dump()) +
                    "\n\nResponde con un JSON {\"decision\": \"comprar\"|\"mantener\"|\"vender\", \"confianza\": 0..1, "
                    "\"peso_pct\": n, \"motivo\": \"menos de 40 palabras\"}",
                traza, "gestor");
  json gj = r.empty() ? json() : extraer_json(r);
  if (gj.is_object()) {
    std::string d = lower(jstr(gj, "decision"));
    if (d == "comprar" || d == "mantener" || d == "vender") dec["decision"] = d;
    if (gj.contains("confianza")) dec["confianza"] = rnd(std::max(0.0, std::min(1.0, jnum(gj["confianza"]))), 2);
    if (gj.contains("peso_pct")) dec["peso_pct"] = rnd(std::max(0.0, std::min(max_peso, jnum(gj["peso_pct"]))), 1);
    if (!jstr(gj, "motivo").empty()) dec["motivo"] = jstr(gj, "motivo");
    if (dec["decision"] != "comprar") dec["peso_pct"] = 0.0;
    dec["ia"] = true;
  }

  json ia = ia_info ? ia_info() : json(nullptr);
  json out = {{"simbolo", sym},
              {"nombre", datos["nombre"]},
              {"ts", b.now()},
              {"precio", px},
              {"datos_reales", q.value("real", false)},
              {"analistas", analistas},
              {"debate", debate},
              {"operador", operador},
              {"riesgo", riesgo},
              {"decision", dec},
              {"lecciones", lecc},
              {"senales", sg},
              {"con_ia", !traza["papeles_ia"].empty()},
              {"papeles_ia", traza["papeles_ia"]},
              {"ia", ia},
              {"aviso", AVISO}};
  {
    std::lock_guard<std::recursive_mutex> lk(b.mutex());
    b.db().execute("INSERT INTO bolsa_informes (ts, simbolo, decision, informe) VALUES (?,?,?,?)",
                   {b.now(), sym, jstr(dec, "decision"), jdump(out)});
    b.db().execute("DELETE FROM bolsa_informes WHERE id < (SELECT MAX(id) - 500 FROM bolsa_informes)");
    out["id"] = b.db().last_insert_id();
  }
  return out;
}

json Mesa::informes(const std::string& simbolo, int limite) {
  auto rows = simbolo.empty()
                  ? b.db().query("SELECT id, ts, simbolo, decision, informe FROM bolsa_informes ORDER BY id DESC LIMIT ?", {limite})
                  : b.db().query("SELECT id, ts, simbolo, decision, informe FROM bolsa_informes WHERE simbolo=? ORDER BY id "
                                 "DESC LIMIT ?",
                                 {upper(simbolo), limite});
  json out = json::array();
  for (auto& r : rows) {
    json x = r;
    json inf = jparse(jstr(r, "informe"));
    x.erase("informe");
    if (inf.is_object()) {
      x["confianza"] = inf["decision"]["confianza"];
      x["peso_pct"] = inf["decision"]["peso_pct"];
      x["motivo"] = inf["decision"]["motivo"];
      x["con_ia"] = inf.value("con_ia", false);
      if (limite == 1) x["informe"] = inf;
    }
    out.push_back(x);
  }
  return out;
}

Mesa::LLM mesa_llm;
std::function<json()> mesa_ia_info;

namespace {
std::mutex g_mu;
std::map<Bolsa*, std::unique_ptr<Mesa>> g_mesas;
}  // namespace

Mesa& mesa() {
  Bolsa& b = get();
  std::lock_guard<std::mutex> lk(g_mu);
  auto& m = g_mesas[&b];
  if (!m) m = std::make_unique<Mesa>(b);
  m->llm = mesa_llm;
  m->ia_info = mesa_ia_info;
  return *m;
}

void reset_mesas() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_mesas.clear();
}

}  // namespace nx::bolsa
