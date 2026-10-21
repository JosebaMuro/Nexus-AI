// Supervision de herramientas (tools/supervisor.py): politica por riesgo (auto, ask, admin, deny), aprobaciones
// humanas, limites de tiempo, aislamiento de las de la comunidad y registro de auditoria.
#include <algorithm>
#include <chrono>
#include <set>
#include <future>
#include <thread>

#include "tools/tools.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::tools {

static auto L = log::get("supervisor");
static const std::map<std::string, std::string> STRICTER = {
    {"auto", "ask"}, {"ask", "admin"}, {"admin", "admin"}, {"deny", "deny"}};

Supervisor::Supervisor(ToolRegistry& r, DB& db, Config& c) : registry(r), db_(db), config_(c) {}

std::string Supervisor::decision_for(const std::string& risk, bool remote) {
  json pol = config_.get("tools.policy", json::object());
  std::string policy = pol.is_object() && pol.contains(risk) ? jstr(pol, risk) : "admin";
  if (remote && risk != "low") {   // peticiones de otros nodos: un escalon mas estricto
    auto it = STRICTER.find(policy);
    policy = it == STRICTER.end() ? "admin" : it->second;
  }
  return policy;
}

std::pair<bool, std::string> Supervisor::await_approval(const json& call, ToolContext& ctx, const std::string& level,
                                                        double timeout) {
  std::string pid = uuid_hex().substr(0, 10);
  auto p = std::make_shared<Pending>();
  p->item = {{"id", pid},          {"tool", call["tool"]}, {"args", call["args"]},     {"risk", call["risk"]},
             {"agent", ctx.agent}, {"session_id", ctx.session_id}, {"level", level}, {"remote", ctx.remote},
             {"created", now()},   {"approved", nullptr},  {"approver", ""}};
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_[pid] = p;
  }
  json event = {{"type", "approval_required"}, {"approval", p->item}};
  if (ctx.emit) ctx.emit(event);
  for (auto& cb : std::vector<std::function<void(const json&)>>(listeners)) {
    try {
      cb(event);
    } catch (...) {
    }
  }
  std::unique_lock<std::mutex> lk(mu_);
  p->cv.wait_for(lk, std::chrono::milliseconds((long)(timeout * 1000)), [&] { return p->approved.has_value(); });
  pending_.erase(pid);
  if (!p->approved) return {false, "timeout"};
  return {*p->approved, p->approver};
}

bool Supervisor::resolve(const std::string& pid, bool approve, bool is_admin) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = pending_.find(pid);
  if (it == pending_.end()) return false;
  if (jstr(it->second->item, "level") == "admin" && !is_admin)
    throw PermissionError("Esta accion requiere aprobacion de un administrador");
  it->second->approved = approve;
  it->second->approver = is_admin ? "admin" : "usuario";
  it->second->item["approved"] = approve;
  it->second->item["approver"] = it->second->approver;
  it->second->cv.notify_all();
  return true;
}

json Supervisor::list_pending() {
  std::lock_guard<std::mutex> lk(mu_);
  json out = json::array();
  for (auto& [_, p] : pending_) out.push_back(p->item);
  return out;
}

static std::string error_type(const std::exception& e) {
  if (auto* t = dynamic_cast<const ToolError*>(&e)) return t->type;
  if (dynamic_cast<const PermissionError*>(&e)) return "PermissionError";
  if (dynamic_cast<const std::invalid_argument*>(&e)) return "ValueError";
  if (dynamic_cast<const std::out_of_range*>(&e)) return "KeyError";
  if (dynamic_cast<const json::exception*>(&e)) return "KeyError";
  return "RuntimeError";
}

json Supervisor::execute(std::string name, json args, ToolContext& ctx) {
  auto spec = registry.get(name);
  double t0 = now();
  std::vector<std::string> fixed;
  if ((!spec || !spec->enabled) && assist) {
    // el modelo pidio una herramienta que no existe ("search_web"): la equivalente si esta clara
    std::vector<mem::ToolView> enabled;
    for (auto& t : registry.all())
      if (t->enabled) enabled.push_back(t->view());
    if (auto alt = assist->fix_tool_name(name, enabled)) {
      fixed.push_back("herramienta " + name + " -> " + *alt);
      name = *alt;
      spec = registry.get(name);
    }
  }
  if (!spec || !spec->enabled) return {{"error", "herramienta desconocida o desactivada: " + name}};
  if (assist) {
    auto [a2, changes] = assist->repair_args(spec->parameters, args.is_object() ? args : json::object());
    args = a2;
    fixed.insert(fixed.end(), changes.begin(), changes.end());
  }
  auto [clean, errors] = validate_args(spec->parameters, args);
  if (!errors.empty()) return {{"error", join(errors, "; ")}};
  std::string decision = decision_for(spec->risk, ctx.remote);
  std::string approver = "politica";
  json call = {{"tool", name}, {"args", clean}, {"risk", spec->risk}};
  if (decision == "deny") {
    audit(ctx, call, "deny", approver, "bloqueada", "", 0);
    return {{"error", "'" + name + "' esta bloqueada por la politica de seguridad"}};
  }
  if (decision == "ask" || decision == "admin") {
    auto [ok, who] = await_approval(call, ctx, decision);
    approver = who;
    if (!ok) {
      audit(ctx, call, decision, approver, "rechazada", "", 0);
      return {{"error", "El uso de '" + name + "' no fue aprobado (" + (approver.empty() ? "rechazado" : approver) + ")"}};
    }
  }
  double timeout = spec->timeout ? *spec->timeout : jnum(config_.get("tools.timeout_s", 30), 30);
  json result;
  std::string status;
  if (spec->func) {
    auto prom = std::make_shared<std::promise<json>>();
    auto fut = prom->get_future();
    ToolContext c2 = ctx;
    std::thread([prom, f = spec->func, clean = clean, c2]() mutable {
      try {
        prom->set_value(f(clean, c2));
      } catch (...) {
        prom->set_exception(std::current_exception());
      }
    }).detach();
    if (fut.wait_for(std::chrono::milliseconds((long)(timeout * 1000))) == std::future_status::timeout) {
      result = {{"error", cat("tiempo agotado (", fnum(timeout, 0), "s)")}};
      status = "timeout";
    } else {
      try {
        result = fut.get();
        status = "ok";
      } catch (const std::exception& e) {
        result = {{"error", error_type(e) + ": " + e.what()}};
        status = "error";
        L.error("Herramienta ", name, " fallo: ", e.what());
      }
    }
  } else {
    try {
      result = run_sandboxed(P(spec->path), clean, timeout);
      status = "ok";
    } catch (const ToolError& e) {
      if (e.type == "TimeoutError") {
        result = {{"error", cat("tiempo agotado (", fnum(timeout, 0), "s)")}};
        status = "timeout";
      } else {
        result = {{"error", e.type + ": " + e.what()}};
        status = "error";
      }
    } catch (const std::exception& e) {
      result = {{"error", error_type(e) + ": " + e.what()}};
      status = "error";
    }
  }
  if (result.is_object() && truthy(result.value("error", json())) && status == "ok") status = "error";
  audit(ctx, call, decision, approver, status, result, now() - t0);
  if (!result.is_object()) result = {{"result", result}};
  if (!fixed.empty()) result["_corregido"] = join(fixed, "; ");
  return result;
}

json Supervisor::run_sandboxed(const fs::path& path, const json& args, double timeout) {
  json out = python_helper({{"cmd", "run"}, {"path", S(path)}, {"args", args}}, timeout, paths::sandbox_dir());
  if (!out.is_object()) return {{"error", "la herramienta no devolvio resultado"}};
  return out.value("ok", false) ? out["result"] : json{{"error", out.value("error", json())}};
}

void Supervisor::audit(ToolContext& ctx, const json& call, const std::string& decision, const std::string& approver,
                       const std::string& status, const json& result, double dur) {
  std::string preview = truthy(result) ? uleft(pydump(result), 500) : "";
  db_.execute("INSERT INTO tool_calls(ts, agent, session_id, tool, args, risk, decision, approver, status, result, "
              "duration) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
              {now(), ctx.agent + (ctx.remote ? " (remoto)" : ""), ctx.session_id, call["tool"],
               uleft(pydump(call["args"]), 1000), call["risk"], decision, approver, status, preview, round_to(dur, 3)});
}

json Supervisor::audit_log(int limit) {
  json out = json::array();
  for (auto& r : db_.query("SELECT * FROM tool_calls ORDER BY id DESC LIMIT ?", {limit})) out.push_back(r);
  return out;
}

// ---------------------------------------------------------------- propuestas (tools/proposals.py)
static std::string max_risk(const std::vector<std::string>& risks) {
  size_t best = 0;
  for (auto& r : risks) {
    auto it = std::find(RISKS.begin(), RISKS.end(), r);
    if (it != RISKS.end()) best = std::max(best, (size_t)(it - RISKS.begin()));
  }
  return RISKS[best];
}

// analisis con el AST de Python si lo hay; si no, por patrones (mismas reglas, menos preciso)
json analyze(const std::string& code, const std::string& name) {
  try {
    json a = python_helper({{"cmd", "analyze"}, {"code", code}, {"name", name}}, 30, paths::sandbox_dir());
    if (a.is_object()) return a;
  } catch (const std::exception&) {
  }
  static const std::map<std::string, std::string> IMPORTS = {
      {"subprocess", "high"}, {"os", "medium"},      {"shutil", "high"},   {"socket", "medium"}, {"ctypes", "high"},
      {"multiprocessing", "high"}, {"winreg", "high"}, {"importlib", "high"}, {"pickle", "medium"},
      {"requests", "medium"}, {"httpx", "medium"},   {"urllib", "medium"}, {"sys", "medium"},  {"pathlib", "medium"},
      {"threading", "medium"}, {"signal", "high"},   {"marshal", "high"},  {"builtins", "high"}};
  static const std::map<std::string, std::string> CALLS = {
      {"eval", "high"},    {"exec", "high"},   {"compile", "high"}, {"__import__", "high"}, {"open", "medium"},
      {"globals", "high"}, {"locals", "medium"}, {"getattr", "medium"}, {"setattr", "high"}, {"delattr", "high"},
      {"system", "high"},  {"popen", "high"},  {"remove", "high"},  {"rmtree", "high"},    {"unlink", "high"},
      {"kill", "high"}};
  json errors = json::array(), warnings = json::array();
  std::set<std::string> imports, calls;
  std::vector<std::string> risks;
  for (auto& m : Rx(R"(^\s*import\s+([\w.]+(?:\s*,\s*[\w.]+)*))", Rx::M).finditer(code))
    for (auto& part : split(m.str(1), ",")) imports.insert(split(trim(part), ".")[0]);
  for (auto& m : Rx(R"(^\s*from\s+([\w.]+)\s+import)", Rx::M).finditer(code)) imports.insert(split(m.str(1), ".")[0]);
  for (auto& m : Rx(R"(\b(\w+)\s*\()").finditer(code))
    if (CALLS.count(m.str(1))) calls.insert(m.str(1));
  bool has_tool = Rx(R"(^TOOL\s*=\s*\{)", Rx::M).test(code);
  bool has_run = Rx(R"(^def\s+run\s*\(\s*\w+)", Rx::M).test(code);
  if (!has_tool) errors.push_back("Falta TOOL = {...} con name/description/parameters");
  if (!has_run) errors.push_back("Falta la funcion run(args, ctx)");
  for (auto& i : imports)
    if (IMPORTS.count(i)) {
      warnings.push_back("importa '" + i + "' (riesgo " + IMPORTS.at(i) + ")");
      risks.push_back(IMPORTS.at(i));
    }
  for (auto& c : calls) {
    warnings.push_back("llama a '" + c + "' (riesgo " + CALLS.at(c) + ")");
    risks.push_back(CALLS.at(c));
  }
  warnings.push_back("analisis aproximado: no hay Python en este equipo para el analisis por AST");
  json manifest = nullptr;
  if (auto m = Rx(R"rx(["']name["']\s*:\s*["']([^"']+)["'])rx").search(code)) manifest = json{{"name", m->str(1)}};
  if (auto m = Rx(R"rx(["']risk["']\s*:\s*["'](\w+)["'])rx").search(code); m && manifest.is_object())
    manifest["risk"] = m->str(1);
  return {{"ok", errors.empty()}, {"errors", errors}, {"warnings", warnings}, {"imports", imports},
          {"calls", calls}, {"suggested_risk", max_risk(risks)}, {"manifest", manifest}};
}

ProposalManager::ProposalManager(ToolRegistry& r, Supervisor& s, mem::Learner* l)
    : registry_(r), supervisor_(s), learner_(l), root_(paths::sub({"tools", "proposals"})) {}

json ProposalManager::save(const json& prop) {
  fs::path d = root_ / P(jstr(prop, "id"));
  mkdirs(d);
  write_file(d / "tool.py", jstr(prop, "code"));
  json meta = prop;
  meta.erase("code");
  write_file(d / "proposal.json", meta.dump(1, ' ', false, json::error_handler_t::replace));
  return prop;
}

static json without_manifest(json a) {
  a.erase("manifest");
  return a;
}

json ProposalManager::create(std::string name, const std::string& description, std::string code, const json& parameters,
                             const std::string& risk, const std::string& rationale, const std::string& author) {
  static const Rx bad("[^a-z0-9_]");
  name = uleft(bad.sub(trim(lower(name)), "_"), 40);
  if (!Rx("^[a-z][a-z0-9_]{2,40}$").test(name))
    throw std::invalid_argument("nombre invalido (minusculas, numeros y _; 3-40 caracteres)");
  if (!contains(code, "TOOL")) {   // el agente solo envio la funcion: completamos el manifiesto
    json manifest = {{"name", name},
                     {"description", description},
                     {"parameters", truthy(parameters) ? parameters : json{{"type", "object"}, {"properties", json::object()}}},
                     {"risk", std::find(RISKS.begin(), RISKS.end(), risk) != RISKS.end() ? risk : "medium"},
                     {"permissions", json::array()},
                     {"version", "1.0"},
                     {"author", author}};
    code = "TOOL = " + manifest.dump(4, ' ', false, json::error_handler_t::replace) + "\n\n\n" + code;
  }
  json a = analyze(code, name);
  json prop = {{"id", strftime_local("%Y%m%d%H%M%S") + "-" + uuid_hex().substr(0, 4)},
               {"name", name},
               {"description", description},
               {"declared_risk", risk},
               {"rationale", rationale},
               {"author", author},
               {"created", now()},
               {"status", "pendiente"},
               {"analysis", without_manifest(a)},
               {"code", code},
               {"review", ""}};
  auto existing = registry_.get(name);
  if (existing && existing->source == "builtin") {
    prop["analysis"]["errors"].push_back("ya existe una herramienta integrada con ese nombre");
    prop["analysis"]["ok"] = false;
  }
  log::get("proposals").info("Nueva propuesta de herramienta '", name, "' por ", author, " (riesgo sugerido ",
                             jstr(a, "suggested_risk"), ")");
  return save(prop);
}

json ProposalManager::get(const std::string& pid) {
  fs::path d = root_ / P(pid);
  if (!is_file(d / "proposal.json")) return nullptr;
  json prop = jparse(read_text(d / "proposal.json"));
  if (!prop.is_object()) return nullptr;
  prop["code"] = read_text(d / "tool.py");
  return prop;
}

json ProposalManager::list(const std::string& status) {
  std::vector<std::string> names;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(root_, ec))
    if (e.is_directory()) names.push_back(S(e.path().filename()));
  std::sort(names.rbegin(), names.rend());
  json out = json::array();
  for (auto& n : names) {
    json p = get(n);
    if (!p.is_null() && (status.empty() || jstr(p, "status") == status)) out.push_back(p);
  }
  return out;
}

json ProposalManager::update_code(const std::string& pid, const std::string& code) {
  json prop = get(pid);
  if (prop.is_null()) throw std::out_of_range(pid);
  prop["code"] = code;
  prop["analysis"] = without_manifest(analyze(code, jstr(prop, "name")));
  return save(prop);
}

json ProposalManager::test(const std::string& pid, const json& args) {
  json prop = get(pid);
  if (prop.is_null()) throw std::out_of_range(pid);
  if (!prop["analysis"].value("ok", false)) {
    std::vector<std::string> e;
    for (auto& x : prop["analysis"].value("errors", json::array())) e.push_back(x.get<std::string>());
    return {{"error", "la propuesta tiene errores: " + join(e, "; ")}};
  }
  try {
    return {{"result", supervisor_.run_sandboxed(root_ / P(pid) / "tool.py", args, 20)}};
  } catch (const std::exception& e) {
    return {{"error", e.what()}};
  }
}

json ProposalManager::approve(const std::string& pid, const std::string& reviewer, std::string note) {
  json prop = get(pid);
  if (prop.is_null()) throw std::out_of_range(pid);
  json a = analyze(jstr(prop, "code"), jstr(prop, "name"));
  if (!a.value("ok", false)) {
    std::vector<std::string> e;
    for (auto& x : a.value("errors", json::array())) e.push_back(x.get<std::string>());
    throw std::invalid_argument("No se puede aprobar: " + join(e, "; "));
  }
  json manifest = a["manifest"];
  std::string mrisk = manifest.is_object() && manifest.contains("risk") ? jstr(manifest, "risk") : "high";
  std::string sug = jstr(a, "suggested_risk");
  auto idx = [](const std::string& r) {
    auto it = std::find(RISKS.begin(), RISKS.end(), r);
    return it == RISKS.end() ? 2 : (int)(it - RISKS.begin());
  };
  if (idx(mrisk) < idx(sug)) {
    note = trim(note + " | riesgo elevado a " + sug + " por el analisis", " |");
    static const Rx risk_rx(R"rx((["']risk["']\s*:\s*)["']\w+["'])rx");
    prop["code"] = risk_rx.sub(jstr(prop, "code"), [&](const Rx::Match& m) { return m.str(1) + "\"" + sug + "\""; }, 1);
    if (!analyze(jstr(prop, "code"), jstr(prop, "name")).value("ok", false))
      throw std::invalid_argument("No se pudo ajustar el riesgo del manifiesto automaticamente");
  }
  write_file(paths::sub({"tools", "community"}) / (jstr(prop, "name") + ".py"), jstr(prop, "code"));
  prop["status"] = "aprobada";
  prop["review"] = note;
  prop["reviewer"] = reviewer;
  prop["reviewed"] = now();
  save(prop);
  registry_.reload();
  log::get("proposals").info("Propuesta ", pid, " aprobada e instalada como herramienta '", jstr(prop, "name"), "'");
  return prop;
}

json ProposalManager::reject(const std::string& pid, const std::string& reason, const std::string& reviewer) {
  json prop = get(pid);
  if (prop.is_null()) throw std::out_of_range(pid);
  prop["status"] = "rechazada";
  prop["review"] = reason;
  prop["reviewer"] = reviewer;
  prop["reviewed"] = now();
  save(prop);
  if (learner_)   // el sistema aprende por que se rechazo
    learner_->memory.remember("La herramienta propuesta '" + jstr(prop, "name") + "' fue rechazada: " + reason, "lesson",
                              0.8, "revision-herramientas", replace_all(jstr(prop, "author"), "agente:", ""));
  return prop;
}

bool ProposalManager::uninstall(const std::string& name) {
  fs::path f = paths::sub({"tools", "community"}) / (name + ".py");
  if (!is_file(f)) return false;
  remove_file(f);
  registry_.reload();
  return true;
}

}  // namespace nx::tools
