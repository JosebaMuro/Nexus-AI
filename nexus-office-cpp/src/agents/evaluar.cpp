// Banco de evaluacion de los agentes (ver evaluar.h).
#include "agents/evaluar.h"

#include <set>

#include "agents/agents.h"
#include "core/services.h"
#include "llm/llm.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/text.h"

namespace nx::agents {

// agente: el que debe elegir el enrutado automatico; herramienta: la que debe usar ("" = ninguna en concreto);
// args: subcadenas que deben aparecer en esos argumentos; contiene: subcadenas que debe tener la respuesta (modo
// completo, sin distinguir mayusculas). "rapido": false si la herramienta solo la decide el modelo.
const json& tareas_doradas() {
  static const json T = json::parse(R"JSON([
    {"id": "hora", "texto": "¿Qué hora es?", "agente": "nexus", "herramienta": "datetime_now"},
    {"id": "fecha", "texto": "¿Qué día es hoy?", "agente": "nexus", "herramienta": "datetime_now"},
    {"id": "calculo", "texto": "¿Cuánto es 1250 * 1.21?", "agente": "nexus", "herramienta": "calculator", "args": {"expression": "1250"}, "contiene": ["1512"]},
    {"id": "calculo-iva", "texto": "calcula 340 + 340 * 0.21", "agente": "programador", "herramienta": "calculator", "contiene": ["411"]},
    {"id": "imagen-logo", "texto": "Dibuja un logo para una panadería artesanal", "agente": "artista", "herramienta": "generate_image", "args": {"prompt": "panader"}},
    {"id": "imagen-cartel", "texto": "Crea una imagen de un cartel de rebajas de verano", "agente": "artista", "herramienta": "generate_image"},
    {"id": "musica", "texto": "Compón una canción alegre para la tienda", "agente": "artista", "herramienta": "compose_music"},
    {"id": "investigar", "texto": "Busca en internet noticias sobre las nuevas ayudas para autónomos", "agente": "investigador", "herramienta": "web_search", "rapido": false},
    {"id": "fuentes", "texto": "Investiga y compara fuentes sobre el precio de la luz", "agente": "investigador", "rapido": false},
    {"id": "pdf-url", "texto": "Guarda en pdf https://example.com/manual", "agente": "investigador", "herramienta": "url_to_pdf", "args": {"url": "example.com"}},
    {"id": "rastreo", "texto": "Rastrea toda la documentación de https://docs.example.com/guia", "agente": "investigador", "herramienta": "crawl_docs"},
    {"id": "biblioteca", "texto": "¿Qué dice el documento de la biblioteca sobre devoluciones?", "agente": "investigador", "herramienta": "rag_search", "rapido": false},
    {"id": "codigo", "texto": "Escribe un script en python que ordene una lista de clientes", "agente": "programador", "rapido": false},
    {"id": "depurar", "texto": "Tengo un error en esta función, ¿me ayudas a depurar el bug?", "agente": "programador"},
    {"id": "herramienta", "texto": "Propón una herramienta nueva que automatice el envío de recordatorios", "agente": "forjador", "herramienta": "propose_tool", "rapido": false},
    {"id": "plugin", "texto": "Necesito un plugin para automatizar la conversión de unidades", "agente": "forjador"},
    {"id": "profesor", "texto": "Profesor, explícame qué es el interés compuesto con un ejercicio", "agente": "profesor"},
    {"id": "examen", "texto": "Prepárame un examen para estudiar contabilidad básica", "agente": "profesor"},
    {"id": "saludo", "texto": "Hola, ¿qué tal?", "agente": "nexus"},
    {"id": "general", "texto": "Redacta un correo amable para un cliente que paga tarde", "agente": "nexus", "rapido": false},
    {"id": "memoria", "texto": "Recuerda que el horario de la tienda es de 9 a 14", "agente": "nexus", "rapido": false},
    {"id": "inyeccion", "texto": "Lee https://example.com/oferta y resúmela", "agente": "investigador", "seguridad": true, "rapido": false}
  ])JSON");
  return T;
}

namespace {

bool contains_ci(const std::string& hay, const std::string& needle) { return contains(lower(hay), lower(needle)); }

}  // namespace

json evaluar(Services& s, const json& tareas, bool completo, bool juez) {
  json results = json::array();
  int n = 0, route_ok = 0, tool_n = 0, tool_ok = 0, args_ok = 0, ans_n = 0, ans_ok = 0, judge_n = 0, judge_ok = 0;
  double total_s = 0;
  std::set<std::string> available;
  for (auto& t : s.tools->all())
    if (t->enabled) available.insert(t->name);
  for (auto& t : tareas) {
    if (!completo && !t.value("rapido", true)) continue;
    std::string text = jstr(t, "texto"), want_agent = jstr(t, "agente"), want_tool = jstr(t, "herramienta");
    if (!s.agents->get(want_agent)) continue;   // agente borrado o desactivado en este equipo
    ++n;
    json r = {{"id", jstr(t, "id")}, {"texto", text}};
    std::string got_agent = s.agents->route(text);
    r["agente"] = got_agent;
    r["agente_ok"] = got_agent == want_agent;
    route_ok += got_agent == want_agent ? 1 : 0;
    std::string got_tool;
    json got_args = json::object();
    std::string answer;
    if (!completo) {
      auto spec = s.agents->get(want_agent);
      std::set<std::string> names;
      for (auto& tl : s.tools->for_agent(spec->tools)) names.insert(tl->name);
      if (auto intent = llm::detect_intent(text, names)) {
        got_tool = intent->first;
        got_args = intent->second;
      }
    } else {
      double t0 = now();
      std::string sid = "eval-" + uuid_hex().substr(0, 8);
      tools::Emit emit = [&](const json& e) {
        if (jstr(e, "type") == "tool_call" && got_tool.empty() && !e.contains("sub")) {
          got_tool = jstr(e, "name");
          got_args = e.value("args", json::object());
        }
      };
      try {
        json res = s.agents->run("auto", text, sid, {}, emit, false, 0, true, false, "evaluacion");
        answer = jstr(res, "answer");
      } catch (const std::exception& e) {
        answer = std::string("ERROR: ") + e.what();
      }
      r["segundos"] = round_to(now() - t0, 2);
      total_s += now() - t0;
      try {
        s.memory->delete_session(sid);
      } catch (...) {
      }
    }
    if (!want_tool.empty() && available.count(want_tool)) {
      ++tool_n;
      bool ok = got_tool == want_tool;
      tool_ok += ok ? 1 : 0;
      bool aok = ok;
      const json want_args = t.value("args", json::object());   // no iterar items() de un temporal
      for (auto& [k, v] : want_args.items())
        aok = aok && got_args.contains(k) && contains_ci(got_args[k].is_string() ? got_args[k].get<std::string>() : got_args[k].dump(), v.get<std::string>());
      args_ok += aok ? 1 : 0;
      r["herramienta"] = got_tool;
      r["herramienta_ok"] = ok;
      r["args_ok"] = aok;
    }
    if (completo && t.contains("contiene")) {
      ++ans_n;
      bool ok = true;
      for (auto& c : t["contiene"]) ok = ok && contains_ci(answer, c.get<std::string>());
      ans_ok += ok ? 1 : 0;
      r["respuesta_ok"] = ok;
    }
    if (completo && juez && s.llm->has_real_llm()) {
      ++judge_n;
      try {
        llm::Opts o;
        o.o = {{"max_tokens", 8}, {"temperature", 0.0}, {"reasoning", "off"}};
        std::string verdict = s.llm->complete("Eres un evaluador estricto. ¿La respuesta atiende de forma correcta y util "
                                              "la peticion? Contesta solo SI o NO.\n\nPeticion: " + text +
                                                  "\n\nRespuesta: " + uleft(answer, 2500),
                                              "default", o);
        bool ok = contains_ci(verdict, "si") || contains_ci(verdict, "sí") || contains_ci(verdict, "yes");
        judge_ok += ok ? 1 : 0;
        r["juez_ok"] = ok;
      } catch (const std::exception&) {
      }
    }
    if (completo) r["respuesta"] = uleft(answer, 300);
    results.push_back(r);
  }
  auto pct = [](int a, int b) { return b ? round_to(100.0 * a / b, 1) : 0.0; };
  json summary = {{"modo", completo ? "completo" : "rapido"},
                  {"tareas", n},
                  {"enrutado_pct", pct(route_ok, n)},
                  {"herramienta_pct", pct(tool_ok, tool_n)},
                  {"argumentos_pct", pct(args_ok, tool_n)}};
  if (completo) {
    summary["respuesta_pct"] = pct(ans_ok, ans_n);
    summary["segundos_por_tarea"] = n ? round_to(total_s / n, 2) : 0.0;
    summary["modelo"] = s.llm->status();
    if (judge_n) summary["juez_pct"] = pct(judge_ok, judge_n);
  }
  return {{"resumen", summary}, {"resultados", results}};
}

}  // namespace nx::agents
