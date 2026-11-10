// Estilo de cada usuario final (ver estilo.h).
#include "memory/estilo.h"

#include <cmath>
#include <set>

#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::mem {

static auto L = log::get("estilo");

const std::vector<std::string>& StyleLearner::dims() {
  static const std::vector<std::string> D = {"longitud", "formalidad", "formato", "tecnico", "emojis", "tono"};
  return D;
}

namespace {

struct Pattern {
  const char* dim;
  double delta;
  const char* label;
  Rx rx;
};

const std::vector<Pattern>& explicit_patterns() {
  static const std::vector<Pattern> P = [] {
    std::vector<Pattern> v;
    auto add = [&](const char* dim, double d, const char* label, const char* pat) { v.push_back({dim, d, label, Rx(pat, Rx::I)}); };
    // longitud
    add("longitud", -0.35, "pide respuestas mas cortas",
        R"(\b(m[aá]s (corto|breve|conciso)|en pocas palabras|sin rollos?|menos texto|demasiado largo|shorter|too long|tl;?dr)\b)");
    add("longitud", 0.35, "pide mas detalle",
        R"(\b(m[aá]s detall\w*|ampl[ií]a\w*|desarr[oó]ll\w*|expl[ií]ca\w* mejor|m[aá]s extens\w*|con m[aá]s profundidad|more detail|elaborate)\b)");
    // formalidad
    add("formalidad", 0.6, "pide trato de usted", R"(\b(de usted|tr[aá]tame de usted|h[aá]blame de usted|m[aá]s formal|registro formal)\b)");
    add("formalidad", -0.6, "pide tuteo", R"(\b(tut[eé]ame|h[aá]blame de t[uú]|tr[aá]tame de t[uú]|no me hables de usted|menos formal)\b)");
    // formato
    add("formato", 0.35, "pide listas o tablas",
        R"(\b(en (una )?tabla|en (una )?lista|en puntos|por puntos|en vi[ñn]etas|as a table|bullet points)\b)");
    add("formato", -0.35, "pide prosa", R"(\b(sin (listas|vi[ñn]etas|tablas)|en (un )?p[aá]rrafo|en prosa|texto corrido)\b)");
    // nivel tecnico
    add("tecnico", -0.35, "pide lenguaje sencillo",
        R"(\b(sin tecnicismos|m[aá]s (sencillo|f[aá]cil|simple)|no (lo )?entiendo|como si (tuviera|fuera) (5|cinco|un ni[ñn]o)|en cristiano|para (un )?principiantes?|simpler)\b)");
    add("tecnico", 0.35, "pide nivel tecnico", R"(\b(m[aá]s t[eé]cnico|en detalle t[eé]cnico|soy (ingenier|program|t[eé]cnic)\w*|nivel experto|more technical)\b)");
    // emojis
    add("emojis", -0.7, "pide sin emojis", R"(\b(sin emojis?|no uses emojis?|quita los emojis?|nada de emojis?|no emojis?)\b)");
    add("emojis", 0.4, "pide emojis", R"(\b(con emojis?|usa emojis?|pon(le)? (alg[uú]n )?emojis?)\b)");
    // tono
    add("tono", -0.35, "pide ir al grano", R"(\b(al grano|s[eé] directo|sin rodeos|sin florituras|sin cortes[ií]as|no te enrolles|menos peloteo)\b)");
    add("tono", 0.35, "pide un tono mas cercano", R"(\b(m[aá]s (cercano|amable|simp[aá]tico|c[aá]lido)|menos seco|m[aá]s humano)\b)");
    return v;
  }();
  return P;
}

double jaccard(const std::string& a, const std::string& b) {
  auto ta = tokens(a), tb = tokens(b);
  std::set<std::string> sa(ta.begin(), ta.end()), sb(tb.begin(), tb.end());
  if (sa.size() < 3 || sb.size() < 3) return 0;
  size_t inter = 0;
  for (auto& x : sa) inter += sb.count(x);
  return (double)inter / (double)(sa.size() + sb.size() - inter);
}

bool has_emoji(const std::string& s) {
  for (size_t i = 0; i + 3 < s.size(); ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0xF0 && (unsigned char)s[i + 1] == 0x9F) return true;   // U+1F300..U+1FAFF
  }
  return false;
}

}  // namespace

std::vector<StyleSignal> StyleLearner::detect(const std::string& text, const std::string& prev) {
  std::vector<StyleSignal> out;
  std::set<std::string> seen;
  for (auto& p : explicit_patterns())
    if (!seen.count(p.dim) && p.rx.test(text)) {
      out.push_back({"explicita", p.dim, p.delta, p.label});
      seen.insert(p.dim);
    }
  // implicitas, con peso pequeno: como escribe el propio usuario
  static const Rx USTED(R"(\b(usted|ustedes|le agradecer[ií]a|podr[ií]a usted|disculpe)\b)", Rx::I);
  if (!seen.count("formalidad") && USTED.test(text)) out.push_back({"implicita", "formalidad", 0.08, "escribe de usted"});
  if (!seen.count("emojis") && has_emoji(text)) out.push_back({"implicita", "emojis", 0.05, "usa emojis"});
  if (!seen.count("longitud") && ulen(text) < 40 && !prev.empty() && ulen(prev) < 40)
    out.push_back({"implicita", "longitud", -0.02, "escribe mensajes cortos"});
  if (!prev.empty() && jaccard(text, prev) >= 0.6) out.push_back({"reformula", "", 0, "repite la pregunta con otras palabras"});
  return out;
}

void StyleLearner::apply(const std::string& user, const std::string& session_id, const std::string& agent,
                         const StyleSignal& s, const std::string& text) {
  db_.execute("INSERT INTO style_signals(ts, user_id, session_id, agent, kind, dim, delta, text) VALUES(?,?,?,?,?,?,?,?)",
              {now(), user, session_id, agent, s.kind, s.dim, s.delta, uleft(text, 200)});
  if (s.dim.empty()) return;
  auto row = db_.one("SELECT value, confidence, signals, locked FROM user_style WHERE user_id=? AND dim=?", {user, s.dim});
  double v = row ? jnum((*row)["value"]) : 0.0, c = row ? jnum((*row)["confidence"]) : 0.0;
  long long n = row ? (long long)jnum((*row)["signals"]) : 0;
  if (row && jnum((*row)["locked"]) != 0) return;   // el usuario lo fijo a mano
  // una correccion en sentido contrario pesa mas (el usuario cambia de opinion); en el mismo sentido satura
  bool same = (v >= 0) == (s.delta >= 0);
  double step = same ? s.delta * (1.0 - 0.5 * std::fabs(v)) : s.delta * 1.3;
  v = std::max(-1.0, std::min(1.0, v + step));
  c = std::min(1.0, c + std::fabs(s.delta));
  db_.execute(
      "INSERT INTO user_style(user_id, dim, value, confidence, signals, locked, updated) VALUES(?,?,?,?,?,0,?) "
      "ON CONFLICT(user_id, dim) DO UPDATE SET value=excluded.value, confidence=excluded.confidence, "
      "signals=excluded.signals, updated=excluded.updated",
      {user, s.dim, round_to(v, 3), round_to(c, 3), n + 1, now()});
}

json StyleLearner::observe(const std::string& user, const std::string& session_id, const std::string& agent,
                           const std::string& text, const std::string& prev) {
  json out = json::array();
  for (auto& s : detect(text, prev)) {
    try {
      apply(user, session_id, agent, s, text);
      out.push_back({{"kind", s.kind}, {"dim", s.dim}, {"delta", s.delta}, {"label", s.label}});
    } catch (const std::exception& e) {
      L.warning("No se pudo aplicar la senal de estilo: ", e.what());
    }
  }
  return out;
}

void StyleLearner::on_feedback(const std::string& user, const std::string& answer, int score) {
  int sc = score > 0 ? 1 : -1;
  bool long_answer = ulen(answer) > 700;
  static const Rx LIST(R"((^|\n)\s*([-*•]|\d+[.)])\s+\S|\|\s*-{3,})");
  bool structured = LIST.test(answer);
  apply(user, "", "", {"valoracion", "longitud", 0.12 * sc * (long_answer ? 1 : -1), "valoracion"}, "");
  apply(user, "", "", {"valoracion", "formato", 0.1 * sc * (structured ? 1 : -1), "valoracion"}, "");
}

void StyleLearner::record_tools(const std::string& user, const std::vector<std::string>& tools, const std::string& text) {
  std::vector<std::string> seq;
  for (auto& t : tools)
    if (seq.empty() || seq.back() != t) seq.push_back(t);
  if (seq.size() < 2) return;
  if (seq.size() > 5) seq.resize(5);
  db_.execute(
      "INSERT INTO user_routines(user_id, seq, count, last, example) VALUES(?,?,1,?,?) "
      "ON CONFLICT(user_id, seq) DO UPDATE SET count=count+1, last=excluded.last, example=excluded.example",
      {user, join(seq, " > "), now(), uleft(text, 160)});
}

json StyleLearner::routines(const std::string& user, int min_count) {
  json out = json::array();
  for (auto& r : db_.query("SELECT seq, count, last, example FROM user_routines WHERE user_id=? AND count>=? "
                           "ORDER BY count DESC, last DESC LIMIT 5",
                           {user, min_count}))
    out.push_back(r);
  return out;
}

std::string StyleLearner::prompt(const std::string& user) {
  static const std::map<std::string, std::pair<std::string, std::string>> TXT = {
      {"longitud", {"Responde breve y al grano (1-3 frases salvo que pida mas).", "Responde con detalle y desarrollo."}},
      {"formalidad", {"Tutea al usuario, con registro cercano.", "Trata al usuario de usted, con registro formal."}},
      {"formato", {"Prefiere prosa en parrafos, sin listas.", "Prefiere listas, pasos numerados y tablas cuando haya varios elementos."}},
      {"tecnico", {"Evita tecnicismos: explica con palabras sencillas y ejemplos.", "Puedes usar terminologia tecnica sin explicarla."}},
      {"emojis", {"No uses emojis.", "Puedes usar algun emoji."}},
      {"tono", {"Tono directo y sobrio, sin cortesias de relleno.", "Tono calido y cercano."}}};
  std::vector<std::string> lines;
  for (auto& r : db_.query("SELECT dim, value, confidence, locked FROM user_style WHERE user_id=?", {user})) {
    double v = jnum(r["value"]), c = jnum(r["confidence"]);
    bool locked = jnum(r["locked"]) != 0;
    if (std::fabs(v) < 0.25 || (c < 0.3 && !locked)) continue;
    auto it = TXT.find(jstr(r, "dim"));
    if (it != TXT.end()) lines.push_back("- " + (v < 0 ? it->second.first : it->second.second));
  }
  for (auto& r : routines(user, 3)) {
    lines.push_back("- Suele trabajar asi: " + jstr(r, "seq") + " (" + std::to_string((long long)jnum(r["count"])) +
                    " veces). Si encaja, ofrece hacerlo de esa manera.");
    if (lines.size() >= 9) break;
  }
  if (lines.empty()) return "";
  return "Estilo de este usuario (aprendido de sus interacciones; respetalo salvo que pida otra cosa):\n" + join(lines, "\n");
}

json StyleLearner::profile(const std::string& user) {
  json d = json::object();
  for (auto& name : dims()) d[name] = {{"value", 0.0}, {"confidence", 0.0}, {"signals", 0}, {"locked", false}};
  for (auto& r : db_.query("SELECT * FROM user_style WHERE user_id=?", {user}))
    d[jstr(r, "dim")] = {{"value", jnum(r["value"])},
                         {"confidence", jnum(r["confidence"])},
                         {"signals", (long long)jnum(r["signals"])},
                         {"locked", jnum(r["locked"]) != 0}};
  json recent = json::array();
  for (auto& r : db_.query("SELECT ts, kind, dim, delta, text, agent FROM style_signals WHERE user_id=? ORDER BY id DESC "
                           "LIMIT 20",
                           {user}))
    recent.push_back(r);
  return {{"user", user}, {"dims", d}, {"recent", recent}, {"routines", routines(user, 2)}, {"prompt", prompt(user)}};
}

void StyleLearner::set(const std::string& user, const std::string& dim, double value, bool locked) {
  if (std::find(dims().begin(), dims().end(), dim) == dims().end()) throw std::invalid_argument("dimension desconocida: " + dim);
  value = std::max(-1.0, std::min(1.0, value));
  db_.execute(
      "INSERT INTO user_style(user_id, dim, value, confidence, signals, locked, updated) VALUES(?,?,?,1,0,?,?) "
      "ON CONFLICT(user_id, dim) DO UPDATE SET value=excluded.value, confidence=1, locked=excluded.locked, "
      "updated=excluded.updated",
      {user, dim, value, locked ? 1 : 0, now()});
}

void StyleLearner::reset(const std::string& user, const std::string& dim) {
  if (dim.empty()) {
    db_.execute("DELETE FROM user_style WHERE user_id=?", {user});
    db_.execute("DELETE FROM user_routines WHERE user_id=?", {user});
  } else {
    db_.execute("DELETE FROM user_style WHERE user_id=? AND dim=?", {user, dim});
  }
}

}  // namespace nx::mem
