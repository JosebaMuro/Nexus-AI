// Actividad de cada agente (nexus/agents/activity.py): mismos estados, escenas y JSON.
#include "agents/activity.h"

#include <cmath>

#include "util/crypto.h"
#include "util/text.h"

namespace nx::agents {

namespace {
const std::map<std::string, std::string> TOOL_SCENES = {   // herramienta -> escena del despacho
    {"web_search", "browsing"},  {"web_fetch", "browsing"},     {"rag_search", "reading"}, {"rag_import_url", "reading"},
    {"generate_image", "drawing"}, {"imagen_editar", "drawing"}, {"gimp_script", "drawing"}, {"compose_music", "music"},
    {"calculator", "calculating"}, {"python", "coding"},        {"run_code", "coding"},     {"delegate", "delegating"},
    {"graph_import", "reading"}, {"consultar_profesor", "phone"}};
}  // namespace

void Activity::set(const std::string& agent, const std::string& estado, const std::optional<std::string>& tarea,
                   const std::string& herramienta) {
  double t = now();
  json snap;
  {
    std::lock_guard<std::mutex> lk(mu_);
    json cur = state_.count(agent) ? state_[agent]
                                   : json{{"agente", agent}, {"tareas", 0}, {"desde", t}, {"tarea", ""}};
    if (jstr(cur, "estado") == estado && cur.contains("estado") && jstr(cur, "herramienta") == herramienta && !tarea)
      return;
    if (estado == "idle" && cur.contains("estado") && !cur["estado"].is_null() && jstr(cur, "estado") != "idle") {
      cur["tareas"] = cur.value("tareas", 0) + 1;
      cur["ultima"] = t;
    }
    std::string escena = estado;
    if (estado == "tool") {
      auto it = TOOL_SCENES.find(herramienta);
      escena = it == TOOL_SCENES.end() ? "working" : it->second;
    }
    cur["estado"] = estado;
    cur["herramienta"] = herramienta;
    cur["desde"] = t;
    cur["escena"] = escena;
    if (tarea) cur["tarea"] = uleft(*tarea, 140);
    state_[agent] = cur;
    snap = cur;
  }
  json ev = {{"type", "agent_activity"}, {"activity", snap}};
  for (auto& fn : listeners) {
    try {
      fn(ev);
    } catch (...) {
    }
  }
}

void Activity::on_event(const std::string& agent, const json& e, const std::string& task) {
  std::string t = jstr(e, "type");
  if (t == "state") {
    std::string st = jstr(e, "state");
    if (st == "thinking") {
      std::string cur = jstr(get(agent), "estado");
      set(agent, "thinking", cur.empty() || cur == "idle" ? std::optional<std::string>(task) : std::nullopt);
    } else if (st == "idle") {
      set(agent, "idle");
    }
  } else if (t == "token") {
    set(agent, "writing");
  } else if (t == "tool_call") {
    std::string name = jstr(e, "name");
    set(agent, name == "delegate" ? "delegating" : "tool", std::nullopt, name);
  }
}

json Activity::get(const std::string& agent) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = state_.find(agent);
  return it == state_.end() ? json::object() : it->second;
}

json Activity::all() {
  double t = now();
  std::lock_guard<std::mutex> lk(mu_);
  json out = json::object();
  for (auto& [a, v] : state_) {
    json x = v;
    x["segundos"] = (long long)std::llround(t - jnum(v.value("desde", json(t))));
    out[a] = x;
  }
  return out;
}

void Activity::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  state_.clear();
}

Activity& activity() {
  static Activity A;
  return A;
}

}  // namespace nx::agents
