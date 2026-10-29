// Herramientas basicas de bajo riesgo (tools/builtin/core_tools.py y agent_tools.py): calculadora segura,
// fecha/hora, memoria, grafo, sistema y colaboracion entre agentes.
#include <climits>
#include <cmath>
#include <cstdlib>
#include <set>

#include "agents/agents.h"
#include "core/services.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "memory/kg.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/text.h"

namespace nx::tools {

namespace {

// ------------------------------------------------------------------ calculadora (subconjunto seguro de Python)
struct Num {
  bool is_int = true;
  long long i = 0;
  double f = 0;
  double d() const { return is_int ? (double)i : f; }
  static Num I(long long v) { return {true, v, 0}; }
  static Num F(double v) { return {false, 0, v}; }
};

[[noreturn]] void bad() { throw ToolError("ValueError", "expresion no permitida"); }

class Calc {
 public:
  explicit Calc(std::string s) : s_(std::move(s)) {}
  Num parse() {
    Num v = expr();
    skip();
    if (p_ != s_.size()) throw ToolError("SyntaxError", "invalid syntax");
    return v;
  }

 private:
  std::string s_;
  size_t p_ = 0;
  void skip() {
    while (p_ < s_.size() && isspace((unsigned char)s_[p_])) ++p_;
  }
  bool eat(const char* tok) {
    skip();
    size_t n = strlen(tok);
    if (s_.compare(p_, n, tok) == 0) {
      // "*" no debe comerse el primer caracter de "**" ni "/" el de "//"
      if ((n == 1 && (tok[0] == '*' || tok[0] == '/')) && p_ + 1 < s_.size() && s_[p_ + 1] == tok[0]) return false;
      p_ += n;
      return true;
    }
    return false;
  }
  // enteros de 64 bits mientras quepan (como Python); si se desbordan, coma flotante
  static bool add_ok(long long a, long long b, long long* r) {
    if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return false;
    *r = a + b;
    return true;
  }
  static bool mul_ok(long long a, long long b, long long* r) {
    if (a == 0 || b == 0) {
      *r = 0;
      return true;
    }
    if (a == LLONG_MIN || b == LLONG_MIN) {
      if (a == 1 || b == 1) {
        *r = a == 1 ? b : a;
        return true;
      }
      return false;
    }
    if (std::llabs(a) > LLONG_MAX / std::llabs(b)) return false;
    *r = a * b;
    return true;
  }
  static Num add(Num a, Num b, int sign) {
    if (a.is_int && b.is_int && !(sign < 0 && b.i == LLONG_MIN)) {
      long long r;
      if (add_ok(a.i, sign * b.i, &r)) return Num::I(r);
    }
    return Num::F(a.d() + sign * b.d());
  }
  static Num mul(Num a, Num b) {
    if (a.is_int && b.is_int) {
      long long r;
      if (mul_ok(a.i, b.i, &r)) return Num::I(r);
    }
    return Num::F(a.d() * b.d());
  }
  static Num div(Num a, Num b) {
    if (b.d() == 0) throw ToolError("ZeroDivisionError", "division by zero");
    return Num::F(a.d() / b.d());
  }
  static Num floordiv(Num a, Num b) {
    if (b.d() == 0) throw ToolError("ZeroDivisionError", "integer division or modulo by zero");
    if (a.is_int && b.is_int) {
      long long q = a.i / b.i;
      if ((a.i % b.i != 0) && ((a.i < 0) != (b.i < 0))) --q;
      return Num::I(q);
    }
    return Num::F(std::floor(a.d() / b.d()));
  }
  static Num mod(Num a, Num b) {
    if (b.d() == 0) throw ToolError("ZeroDivisionError", "integer division or modulo by zero");
    if (a.is_int && b.is_int) {
      long long r = a.i % b.i;
      if (r != 0 && ((r < 0) != (b.i < 0))) r += b.i;
      return Num::I(r);
    }
    double r = std::fmod(a.d(), b.d());
    if (r != 0 && ((r < 0) != (b.d() < 0))) r += b.d();
    return Num::F(r);
  }
  static Num pow(Num a, Num b) {
    if (std::abs(b.d()) > 1000) throw ToolError("ValueError", "exponente demasiado grande");
    if (a.is_int && b.is_int && b.i >= 0) {
      double r = std::pow((double)a.i, (double)b.i);
      if (std::abs(r) < 9e18) {
        long long acc = 1;
        for (long long k = 0; k < b.i; ++k) acc *= a.i;
        return Num::I(acc);
      }
      return Num::F(r);
    }
    if (a.d() == 0 && b.d() < 0) throw ToolError("ZeroDivisionError", "0.0 cannot be raised to a negative power");
    double r = std::pow(a.d(), b.d());
    if (std::isnan(r)) throw ToolError("ValueError", "math domain error");
    return Num::F(r);
  }
  Num expr() {
    Num v = term();
    for (;;) {
      if (eat("+"))
        v = add(v, term(), 1);
      else if (eat("-"))
        v = add(v, term(), -1);
      else
        return v;
    }
  }
  Num term() {
    Num v = factor();
    for (;;) {
      if (eat("//"))
        v = floordiv(v, factor());
      else if (eat("*"))
        v = mul(v, factor());
      else if (eat("/"))
        v = div(v, factor());
      else if (eat("%"))
        v = mod(v, factor());
      else
        return v;
    }
  }
  Num factor() {
    if (eat("-")) {
      Num v = factor();
      return v.is_int ? Num::I(-v.i) : Num::F(-v.f);
    }
    if (eat("+")) return factor();
    return power();
  }
  Num power() {
    Num base = atom();
    if (eat("**")) return pow(base, factor());
    return base;
  }
  Num atom() {
    skip();
    if (p_ >= s_.size()) throw ToolError("SyntaxError", "invalid syntax");
    char c = s_[p_];
    if (c == '(') {
      ++p_;
      Num v = expr();
      if (!eat(")")) throw ToolError("SyntaxError", "invalid syntax");
      return v;
    }
    if (isdigit((unsigned char)c) || c == '.') {
      size_t st = p_;
      bool flt = false;
      while (p_ < s_.size() && (isdigit((unsigned char)s_[p_]) || s_[p_] == '.' || s_[p_] == '_')) flt |= s_[p_++] == '.';
      if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
        size_t save = p_++;
        if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
        if (p_ < s_.size() && isdigit((unsigned char)s_[p_])) {
          flt = true;
          while (p_ < s_.size() && isdigit((unsigned char)s_[p_])) ++p_;
        } else {
          p_ = save;
        }
      }
      std::string tok = replace_all(s_.substr(st, p_ - st), "_", "");
      if (tok == "." || std::count(tok.begin(), tok.end(), '.') > 1) throw ToolError("SyntaxError", "invalid syntax");
      if (flt) return Num::F(std::stod(tok));
      try {
        return Num::I(std::stoll(tok));
      } catch (...) {
        return Num::F(std::stod(tok));
      }
    }
    if (isalpha((unsigned char)c) || c == '_') {
      size_t st = p_;
      while (p_ < s_.size() && (isalnum((unsigned char)s_[p_]) || s_[p_] == '_')) ++p_;
      std::string name = s_.substr(st, p_ - st);
      if (eat("(")) {
        std::vector<Num> args;
        if (!eat(")")) {
          do args.push_back(expr());
          while (eat(","));
          if (!eat(")")) throw ToolError("SyntaxError", "invalid syntax");
        }
        return call(name, args);
      }
      if (name == "pi") return Num::F(M_PI);
      if (name == "e") return Num::F(M_E);
      if (name == "tau") return Num::F(2 * M_PI);
      bad();
    }
    bad();
  }
  static Num call(const std::string& f, const std::vector<Num>& a) {
    auto need = [&](size_t n) {
      if (a.size() != n) throw ToolError("TypeError", f + "() takes exactly " + std::to_string(n) + " argument");
    };
    auto domain = [](double r) {
      if (std::isnan(r)) throw ToolError("ValueError", "math domain error");
      return Num::F(r);
    };
    if (f == "sqrt") { need(1); if (a[0].d() < 0) throw ToolError("ValueError", "math domain error"); return Num::F(std::sqrt(a[0].d())); }
    if (f == "sin") { need(1); return domain(std::sin(a[0].d())); }
    if (f == "cos") { need(1); return domain(std::cos(a[0].d())); }
    if (f == "tan") { need(1); return domain(std::tan(a[0].d())); }
    if (f == "asin") { need(1); return domain(std::asin(a[0].d())); }
    if (f == "acos") { need(1); return domain(std::acos(a[0].d())); }
    if (f == "atan") { need(1); return Num::F(std::atan(a[0].d())); }
    if (f == "exp") { need(1); return Num::F(std::exp(a[0].d())); }
    if (f == "radians") { need(1); return Num::F(a[0].d() * M_PI / 180); }
    if (f == "degrees") { need(1); return Num::F(a[0].d() * 180 / M_PI); }
    if (f == "log" || f == "log10" || f == "log2") {
      if (a.empty() || a.size() > (f == "log" ? 2u : 1u)) throw ToolError("TypeError", f + "() argumentos invalidos");
      if (a[0].d() <= 0) throw ToolError("ValueError", "math domain error");
      if (f == "log10") return Num::F(std::log10(a[0].d()));
      if (f == "log2") return Num::F(std::log2(a[0].d()));
      if (a.size() == 2) {
        if (a[1].d() <= 0 || a[1].d() == 1) throw ToolError("ValueError", "math domain error");
        return Num::F(std::log(a[0].d()) / std::log(a[1].d()));
      }
      return Num::F(std::log(a[0].d()));
    }
    if (f == "floor" || f == "ceil") {
      need(1);
      if (a[0].is_int) return a[0];
      double r = f == "floor" ? std::floor(a[0].f) : std::ceil(a[0].f);
      return std::abs(r) < 9e18 ? Num::I((long long)r) : Num::F(r);
    }
    if (f == "factorial") {
      need(1);
      if (!a[0].is_int && a[0].f != std::floor(a[0].f)) throw ToolError("ValueError", "factorial() only accepts integral values");
      long long n = a[0].is_int ? a[0].i : (long long)a[0].f;
      if (n < 0) throw ToolError("ValueError", "factorial() not defined for negative values");
      if (n > 1000) throw ToolError("ValueError", "numero demasiado grande");
      Num r = Num::I(1);
      for (long long k = 2; k <= n; ++k) r = mul(r, Num::I(k));
      return r;
    }
    if (f == "abs") {
      need(1);
      return a[0].is_int ? Num::I(std::llabs(a[0].i)) : Num::F(std::abs(a[0].f));
    }
    if (f == "round") {
      if (a.empty() || a.size() > 2) throw ToolError("TypeError", "round() argumentos invalidos");
      if (a.size() == 1) {
        if (a[0].is_int) return a[0];
        return Num::I((long long)std::nearbyint(a[0].f));
      }
      if (a[0].is_int) return a[0];
      return Num::F(round_to(a[0].f, (int)a[1].d()));
    }
    if (f == "min" || f == "max") {
      if (a.empty()) throw ToolError("TypeError", f + " expected at least 1 argument, got 0");
      Num best = a[0];
      for (auto& x : a)
        if (f == "min" ? x.d() < best.d() : x.d() > best.d()) best = x;
      return best;
    }
    bad();
  }
};

json calculator(const json& args, ToolContext&) {
  std::string original = jstr(args, "expression");
  std::string expr = replace_all(replace_all(replace_all(replace_all(original, "^", "**"), ",", "."), "×", "*"), "÷", "/");
  Num r = Calc(expr).parse();
  json result;
  if (r.is_int) {
    result = r.i;
  } else if (std::isfinite(r.f) && r.f == std::floor(r.f) && std::abs(r.f) < 1e15) {
    result = (long long)r.f;
  } else {
    result = std::isfinite(r.f) ? json(round_to(r.f, 10)) : json(r.f);
  }
  return {{"expression", original}, {"result", result}};
}

json datetime_now(const json&, ToolContext&) {
  static const char* DAYS[7] = {"lunes", "martes", "miercoles", "jueves", "viernes", "sabado", "domingo"};
  double t = now();
  return {{"date", strftime_local("%d/%m/%Y", t)},
          {"time", strftime_local("%H:%M", t)},
          {"weekday", DAYS[weekday_local(t)]},
          {"iso", strftime_local("%Y-%m-%dT%H:%M:%S", t)}};
}

json memory_search(const json& args, ToolContext& ctx) {
  Services& s = *ctx.services;
  std::string q = jstr(args, "query");
  int k = (int)jnum(args.value("k", json(6)), 6);
  auto items = s.memory->recall(q, k, {}, ctx.agent.empty() ? std::nullopt : std::optional<std::string>(ctx.agent));
  json results = json::array();
  for (auto& i : items) results.push_back({{"content", i["content"]}, {"kind", i["kind"]}, {"score", i["score"]}});
  std::string low = lower(q);
  if (contains(low, "usuario") || low == "yo" || low == "mi" || low == "me") {
    std::string known;
    for (auto& r : results) known += lower(jstr(r, "content")) + " ";
    for (auto& f : s.graph->user_facts(8)) {   // hechos del grafo que la memoria aun no contiene
      auto parts = split(f, " ", 3);
      if (!contains(known, lower(parts.back()))) results.push_back({{"content", f}, {"kind", "grafo"}});
    }
  }
  return {{"results", results}};
}

json memory_save(const json& args, ToolContext& ctx) {
  Services& s = *ctx.services;
  std::string content = jstr(args, "content");
  long long mid = s.memory->remember(content, jstr(args, "kind", "note"), jnum(args.value("importance", json(0.7)), 0.7),
                                     "agente:" + ctx.agent);
  // datos del usuario, pautas y relaciones al general; los temas, al especialista que corresponda (o comunes)
  s.learner->learn_text(content, ctx.agent);
  return {{"saved", true}, {"id", mid}};
}

json knowledge_graph(const json& args, ToolContext& ctx) {
  auto& g = *ctx.services->graph;
  std::string entity = jstr(args, "entity");
  if (entity.empty()) return g.stats();
  std::vector<std::string> own, own_notes;
  if (!ctx.agent.empty()) {
    own = g.for_agent(ctx.agent).facts_about({entity}, 12);
    own_notes = g.for_agent(ctx.agent).notes_about({entity}, 3);
  }
  std::vector<std::string> facts;
  std::set<std::string> seen;
  for (auto& lst : {own, g.facts_about({entity}, 20)})
    for (auto& f : lst)
      if (seen.insert(f).second && facts.size() < 20) facts.push_back(f);
  for (auto& n : g.general().notes_about({entity}, 3)) own_notes.push_back(n);
  return {{"facts", facts}, {"attributes", own_notes}};
}

json graph_import(const json& args, ToolContext& ctx) {
  Services& s = *ctx.services;
  std::string data, name;
  if (truthy(args.value("path", json()))) {
    fs::path p = sandbox_safe(jstr(args, "path"));
    if (!is_file(p)) throw ToolError("ValueError", "no existe " + jstr(args, "path") + " en el sandbox");
    std::error_code ec;
    if (fs::file_size(p, ec) > 300ull * 1024 * 1024) throw ToolError("ValueError", "el grafo supera 300 MB");
    data = read_file(p).value_or("");
    name = S(p.filename());
  } else if (truthy(args.value("content", json()))) {
    data = jstr(args, "content");
    name = truthy(args.value("name", json())) ? jstr(args, "name") : "grafo.txt";
  } else {
    throw ToolError("ValueError", "indica path (fichero del sandbox) o content (el grafo en texto)");
  }
  std::string target = jstr(args, "target", "general");
  target = target == "agente" ? ctx.agent : target == "auto" ? "auto" : "";
  json parsed;
  try {
    parsed = kg::parse_bytes(std::move(data), name);
  } catch (const kg::FormatError& e) {
    throw ToolError("GraphFormatError", e.what());
  }
  json res = kg::insert_graph(*s.graph, parsed, target, s.organizer.get(), s.agents->ids(), "grafo:" + name);
  if (ctx.emit) ctx.emit({{"type", "graph_update"}});
  return res;
}

json system_info(const json&, ToolContext& ctx) {
  Services& s = *ctx.services;
  return {{"profile", s.profile}, {"live", s.monitor->live()["current"]}, {"llm", s.llm->status()}, {"tuning", s.tuning_reasons}};
}

// ------------------------------------------------------------------ agentes
json delegate(const json& args, ToolContext& ctx) {
  if (ctx.depth >= 2) return {{"error", "profundidad maxima de delegacion alcanzada"}};
  auto& agents = *ctx.services->agents;
  auto spec = agents.get(ctx.agent);
  std::string target = jstr(args, "agent");
  if (spec && !spec->can_delegate_to.empty()) {
    auto& cd = spec->can_delegate_to;
    if (std::find(cd.begin(), cd.end(), target) == cd.end() && std::find(cd.begin(), cd.end(), "*") == cd.end())
      return {{"error", ctx.agent + " no puede delegar en " + target}};
  }
  if (!agents.get(target)) return {{"error", "agente desconocido: " + target + ". Disponibles: " + join(agents.ids(), ", ")}};
  std::string answer = agents.run_subtask(target, jstr(args, "task"), ctx);
  return {{"agent", target}, {"answer", answer}};
}

// Pregunta al Profesor (IA online si hay clave, si no el modelo local) y aprende de su respuesta.
json consultar_profesor(const json& args, ToolContext& ctx) {
  if (ctx.agent == "profesor") return {{"error", "el Profesor no se consulta a si mismo"}};
  if (ctx.depth >= 2) return {{"error", "profundidad maxima de delegacion alcanzada"}};
  Services& s = *ctx.services;
  auto& agents = *s.agents;
  if (!agents.get("profesor")) return {{"error", "no hay agente Profesor (restauralo en Agentes)"}};
  std::string pregunta = trim(jstr(args, "pregunta"));
  if (pregunta.empty()) return {{"error", "falta la pregunta"}};
  std::string tarea = pregunta;
  if (truthy(args.value("contexto", json()))) tarea += "\n\nContexto: " + jstr(args, "contexto");
  std::string answer = agents.run_subtask("profesor", tarea, ctx);
  json out = {{"agent", "profesor"}, {"answer", answer}, {"online", s.llm->backend("nube")->available()}};
  if (truthy(args.value("guardar", json(true))) && !trim(answer).empty()) {
    // lo aprendido queda en la memoria y el conocimiento del agente que pregunto
    std::string lesson = "Lección del Profesor sobre «" + uleft(pregunta, 160) + "»: " + uleft(trim(answer), 1200);
    out["memoria"] = s.memory->remember(lesson, "leccion", 0.75, "agente:profesor", ctx.agent);
    s.learner->learn_text(lesson, ctx.agent);
  }
  return out;
}

json propose_tool(const json& args, ToolContext& ctx) {
  Services& s = *ctx.services;
  if (!truthy(s.config.get("tools.allow_agent_proposals", true)))
    return {{"error", "las propuestas de herramientas estan desactivadas"}};
  json params = truthy(args.value("parameters", json())) ? args["parameters"]
                                                         : json{{"type", "object"}, {"properties", json::object()}};
  json prop = s.proposals->create(jstr(args, "name"), jstr(args, "description"), jstr(args, "code"), params,
                                  jstr(args, "risk", "medium"), jstr(args, "rationale"), "agente:" + ctx.agent);
  return {{"proposal_id", prop["id"]},
          {"status", prop["status"]},
          {"analysis", prop["analysis"]},
          {"note", "La herramienta quedara disponible cuando un administrador la apruebe."}};
}

json list_agents(const json&, ToolContext& ctx) {
  json out = json::array();
  for (auto& a : ctx.services->agents->list())
    out.push_back({{"id", a.id}, {"name", a.name}, {"description", a.description}});
  return {{"agents", out}};
}

Register r1("calculator", calculator), r2("datetime_now", datetime_now), r3("memory_search", memory_search),
    r4("memory_save", memory_save), r5("knowledge_graph", knowledge_graph), r6("system_info", system_info),
    r10("graph_import", graph_import),
    r7("delegate", delegate), r8("propose_tool", propose_tool), r9("list_agents", list_agents),
    r11("consultar_profesor", consultar_profesor);

}  // namespace

// referencia para que el enlazador conserve este fichero dentro de la biblioteca estatica
void link_builtin_core() {}

}  // namespace nx::tools
