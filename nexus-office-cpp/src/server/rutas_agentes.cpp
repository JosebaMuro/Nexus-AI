// Rutas de la arquitectura agentica: metricas clave por tarea (/api/metricas), estilo de cada usuario final
// (/api/estilo) y aprobaciones diferidas (/api/aprobaciones). Se registran desde register_routes.
#include <httplib.h>

#include <algorithm>
#include <cmath>

#include "agents/agents.h"
#include "core/services.h"
#include "memory/estilo.h"
#include "remote/remote.h"
#include "server/app.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::server {

namespace {

std::string cookie_value(const std::string& header, const std::string& name) {
  for (auto& part : split(header, ";")) {
    std::string p = trim(part);
    if (starts_with(p, name + "=")) return p.substr(name.size() + 1);
  }
  return "";
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double idx = p * (double)(v.size() - 1);
  size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
  return v[lo] + (v[hi] - v[lo]) * (idx - (double)lo);
}

double ratio(double a, double b) { return b > 0 ? round_to(a / b, 4) : 0.0; }

}  // namespace

// usuario final de una peticion: el dispositivo emparejado (puerto remoto) o el perfil que envia la interfaz
// (cabecera X-Nexus-Usuario o ?usuario=). "" es el titular del equipo.
std::string user_of(Ctx& c) {
  if (c.remote) {
    std::string token = cookie_value(c.header("Cookie"), remote::COOKIE);
    if (token.empty()) token = c.header("X-Nexus-Device");
    std::string dev = token.empty() ? "" : c.app.S().remote->devices.check(token, c.host);
    if (!dev.empty()) return "dispositivo:" + dev;
  }
  std::string u = c.header("X-Nexus-Usuario");
  if (u.empty()) u = c.q("usuario");
  return sanitize_user(u);
}

std::string sanitize_user(const std::string& raw) {
  static const Rx BAD(R"([^\w .@:-])");
  return uleft(trim(BAD.sub(raw, "")), 60);
}

// las cuatro metricas de la guia (tasa de exito, coste y latencia por tarea, escalado humano y precision de las
// llamadas a herramientas) y la adaptacion al estilo, globales, por agente y por usuario
json metricas(DB& db, double since, const std::string& user, const std::string& agent) {
  std::string where = "WHERE t.ts>=?";
  Params p = {since};
  if (!user.empty()) {
    where += " AND t.user_id=?";
    p.push_back(user == "titular" ? "" : user);
  }
  if (!agent.empty()) {
    where += " AND t.agent=?";
    p.push_back(agent);
  }
  auto rows = db.query("SELECT t.*, f.score AS fb FROM tasks t LEFT JOIN (SELECT message_id, MAX(id) AS fid FROM feedback "
                       "GROUP BY message_id) lf ON lf.message_id=t.message_id LEFT JOIN feedback f ON f.id=lf.fid " +
                           where + " ORDER BY t.ts",
                       p);
  struct Acc {
    double n = 0, ok = 0, fb_n = 0, fb_pos = 0, calls = 0, call_ok = 0, call_clean = 0, escalated = 0, denied = 0,
           cost = 0, tok_in = 0, tok_out = 0, tainted = 0, injections = 0, corrections = 0, reformulated = 0;
    std::vector<double> lat, ttft;
    json to_json() const {
      return {{"tareas", (long long)n},
              {"tasa_exito", ratio(ok, n)},
              {"tasa_exito_valorada", ratio(fb_pos, fb_n)},
              {"valoradas", (long long)fb_n},
              {"latencia_p50_s", round_to(percentile(lat, 0.5), 2)},
              {"latencia_p95_s", round_to(percentile(lat, 0.95), 2)},
              {"primer_token_p50_s", round_to(percentile(ttft, 0.5), 2)},
              {"tokens_por_tarea", n ? (long long)std::llround((tok_in + tok_out) / n) : 0},
              {"coste_por_tarea", n ? round_to(cost / n, 6) : 0.0},
              {"coste_total", round_to(cost, 4)},
              {"tasa_escalado_humano", ratio(escalated, n)},
              {"aprobaciones_rechazadas_o_pendientes", (long long)denied},
              {"llamadas_herramientas", (long long)calls},
              {"precision_herramientas", ratio(call_ok, calls)},
              {"precision_herramientas_sin_reparar", ratio(call_clean, calls)},
              {"tareas_con_contenido_externo", (long long)tainted},
              {"posibles_inyecciones", (long long)injections},
              {"correcciones_estilo_por_100", n ? round_to(100.0 * corrections / n, 2) : 0.0},
              {"reformulaciones_por_100", n ? round_to(100.0 * reformulated / n, 2) : 0.0}};
    }
  };
  Acc all;
  std::map<std::string, Acc> by_agent, by_user, by_day;
  for (auto& r : rows) {
    bool fb = r.contains("fb") && r["fb"].is_number();
    double score = fb ? jnum(r["fb"]) : 0;
    bool ok = jnum(r["ok"]) != 0 && !(fb && score < 0);
    double calls = jnum(r["tool_calls"]), errors = jnum(r["tool_errors"]), repaired = jnum(r["tool_repaired"]);
    std::string day = strftime_local("%Y-%m-%d", jnum(r["ts"]));
    for (Acc* a : {&all, &by_agent[jstr(r, "agent")], &by_user[jstr(r, "user_id").empty() ? "titular" : jstr(r, "user_id")],
                   &by_day[day]}) {
      a->n += 1;
      a->ok += ok ? 1 : 0;
      if (fb) {
        a->fb_n += 1;
        a->fb_pos += score > 0 ? 1 : 0;
      }
      a->calls += calls;
      a->call_ok += std::max(0.0, calls - errors);
      a->call_clean += std::max(0.0, calls - errors - repaired);
      a->escalated += jnum(r["approvals"]) > 0 ? 1 : 0;
      a->denied += jnum(r["approvals_denied"]);
      a->cost += jnum(r["cost"]);
      a->tok_in += jnum(r["tokens_in"]);
      a->tok_out += jnum(r["tokens_out"]);
      a->tainted += jnum(r["tainted"]) != 0 ? 1 : 0;
      a->injections += jnum(r["injection_flags"]);
      a->corrections += jnum(r["style_corrections"]);
      a->reformulated += jnum(r["reformulated"]);
      a->lat.push_back(jnum(r["latency"]));
      if (jnum(r["first_token"]) > 0) a->ttft.push_back(jnum(r["first_token"]));
    }
  }
  json agents = json::object(), users = json::object(), days = json::array();
  for (auto& [k, a] : by_agent) agents[k] = a.to_json();
  for (auto& [k, a] : by_user) users[k] = a.to_json();
  for (auto& [k, a] : by_day) {
    json d = a.to_json();
    d["dia"] = k;
    days.push_back(d);
  }
  return {{"desde", since}, {"global", all.to_json()}, {"por_agente", agents}, {"por_usuario", users}, {"por_dia", days}};
}

void register_agent_routes(App& app) {
  // ------------------------------------------------------------------ metricas clave
  app.get("/api/metricas", [](Ctx& c) -> Out {
    double days = std::max(1.0, std::min(365.0, (double)c.qi("dias", 7)));
    return metricas(*c.app.S().db, now() - days * 86400, c.q("usuario_filtro"), c.q("agente"));
  });

  app.get("/api/metricas/tareas", [](Ctx& c) -> Out {
    json out = json::array();
    for (auto& r : c.app.S().db->query("SELECT * FROM tasks ORDER BY id DESC LIMIT ?", {std::min<long long>(500, c.qi("limit", 100))}))
      out.push_back(r);
    return out;
  }, true);

  // ------------------------------------------------------------------ estilo de cada usuario
  app.get("/api/estilo", [](Ctx& c) -> Out {
    std::string u = user_of(c);
    json p = c.app.S().estilo->profile(u);
    p["aprender"] = truthy(c.app.S().config.get("estilo.aprender", true));
    return p;
  });

  app.put("/api/estilo", [](Ctx& c) -> Out {
    std::string u = user_of(c);
    auto& e = *c.app.S().estilo;
    try {
      e.set(u, c.bs("dim"), c.bn("value", 0), c.bb("fijar", true));
    } catch (const std::invalid_argument& ex) {
      throw HttpErr(422, ex.what());
    }
    return e.profile(u);
  });

  app.del("/api/estilo", [](Ctx& c) -> Out {
    std::string u = user_of(c);
    c.app.S().estilo->reset(u, c.q("dim"));
    return c.app.S().estilo->profile(u);
  });

  // ------------------------------------------------------------------ aprobaciones diferidas
  app.get("/api/aprobaciones", [](Ctx& c) -> Out {
    return c.app.S().supervisor->deferred(c.q("estado", "pendiente"), (int)c.qi("limit", 50));
  });
}

}  // namespace nx::server
