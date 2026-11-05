// Asistente de configuracion de agentes: plantillas, grupos de herramientas, borrador, avisos y prueba de enrutado.
#include "agents/asistente.h"
#include "check.h"

using namespace nx;
using namespace nx::agents;

namespace {
json herramientas() {
  return json::array({{{"name", "memory_search"}, {"risk", "low"}},
                      {{"name", "memory_save"}, {"risk", "low"}},
                      {{"name", "rag_search"}, {"risk", "low"}},
                      {{"name", "calculator"}, {"risk", "low"}},
                      {{"name", "web_search"}, {"risk", "medium"}},
                      {{"name", "web_fetch"}, {"risk", "medium"}},
                      {{"name", "python_exec"}, {"risk", "high"}},
                      {{"name", "delegate"}, {"risk", "low"}}});
}
json politica() { return json{{"low", "auto"}, {"medium", "ask"}, {"high", "admin"}}; }

AgentSpec agente(const std::string& id, const std::string& name, std::vector<std::string> kw) {
  AgentSpec a;
  a.id = id;
  a.name = name;
  a.keywords = std::move(kw);
  return a;
}
}  // namespace

TEST("asistente: catalogo con plantillas, grupos y tonos") {
  json c = asistente::catalogo();
  CHECK(c["plantillas"].size() >= 8);
  bool contable = false;
  for (auto& p : c["plantillas"]) contable = contable || jstr(p, "id") == "contable";
  CHECK(contable);
  CHECK(c["grupos"].size() >= 10);
  CHECK(c["tonos"].size() == 5);
}

TEST("asistente: solo ofrece los grupos con herramientas instaladas y su aprobacion") {
  json g = asistente::grupos_disponibles(herramientas(), politica());
  json internet, codigo;
  bool imagenes = false;
  for (auto& x : g) {
    if (jstr(x, "id") == "internet") internet = x;
    if (jstr(x, "id") == "codigo") codigo = x;
    imagenes = imagenes || jstr(x, "id") == "imagenes";
  }
  CHECK(!imagenes);   // ninguna herramienta de imagenes en esta instalacion
  CHECK_EQ(internet["herramientas"].size(), (size_t)2);
  CHECK(internet["red"].get<bool>());
  CHECK_EQ(jstr(internet, "aprobacion"), std::string("ask"));
  CHECK_EQ(jstr(codigo, "aprobacion"), std::string("admin"));
}

TEST("asistente: id a partir del nombre sin pisar a otros") {
  CHECK_EQ(asistente::id_desde_nombre("Atención al cliente", {}), std::string("atencion-al-cliente"));
  CHECK_EQ(asistente::id_desde_nombre("Contable", {"contable", "contable-2"}), std::string("contable-3"));
  CHECK_EQ(asistente::id_desde_nombre("¡¡!!", {}), std::string("agente"));
}

TEST("asistente: borrador desde plantilla con avisos y enrutado") {
  std::vector<AgentSpec> hay = {agente("nexus", "Agente IA", {}), agente("investigador", "Investigador", {"busca", "internet"}),
                                agente("ventas", "Ventas", {"factura"})};
  json r = {{"plantilla", "contable"},
            {"nombre", "Contable"},
            {"tono", "formal"},
            {"creatividad", "preciso"},
            {"longitud", "breve"},
            {"grupos", {"memoria", "biblioteca", "calculo", "internet", "imagenes"}},
            {"delegar", {"investigador", "inventado"}},
            {"palabras", "modelo 303, Trimestre"},
            {"reglas", {"No des asesoramiento fiscal definitivo"}},
            {"frases_prueba", {"revisa esta factura con el iva", "busca noticias en internet", "hola"}}};
  json b = asistente::borrador(r, hay, herramientas(), politica());
  json s = b["spec"];
  CHECK_EQ(jstr(s, "id"), std::string("contable"));
  CHECK_EQ(s["temperature"].get<double>(), 0.2);
  CHECK_EQ(s["max_tokens"].get<int>(), 512);
  std::vector<std::string> tools = s["tools"];
  auto tiene = [&](const char* t) { return std::find(tools.begin(), tools.end(), t) != tools.end(); };
  CHECK(tiene("rag_search") && tiene("calculator") && tiene("web_search") && tiene("delegate"));
  CHECK(!tiene("python_exec"));
  CHECK_EQ(s["can_delegate_to"].size(), (size_t)1);   // el agente inventado se descarta
  std::vector<std::string> kw = s["keywords"];
  CHECK(std::find(kw.begin(), kw.end(), "trimestre") != kw.end());
  CHECK(std::find(kw.begin(), kw.end(), "modelo 303") != kw.end());
  std::string p = jstr(s, "system_prompt");
  CHECK(p.find("Eres Contable, agente de {assistant_name}") != std::string::npos);
  CHECK(p.find("No des asesoramiento fiscal definitivo") != std::string::npos);
  CHECK(p.find("delegate") != std::string::npos);
  CHECK(p.find("formal") != std::string::npos);

  std::string avisos = b["avisos"].dump();
  CHECK(avisos.find("internet") != std::string::npos);
  CHECK(avisos.find("Ventas") != std::string::npos);        // «factura» ya lleva a Ventas
  CHECK(avisos.find("Imágenes") == std::string::npos);      // no instalado: aviso con su id
  CHECK(avisos.find("imagenes") != std::string::npos);

  CHECK_EQ(b["enrutado"].size(), (size_t)3);
  CHECK(b["enrutado"][0]["ok"].get<bool>());
  CHECK_EQ(jstr(b["enrutado"][1], "llega_a"), std::string("Investigador"));
  CHECK_EQ(jstr(b["enrutado"][2], "llega_a"), std::string("el agente principal"));
}

TEST("asistente: ajustar un agente existente conserva su id y su prompt escrito") {
  AgentSpec base = agente("soporte", "Soporte", {"impresora"});
  base.system_prompt = "Prompt original";
  json b = asistente::borrador(json{{"base", "soporte"}, {"nombre", "Soporte IT"}, {"system_prompt", "Mi prompt"},
                                    {"grupos", json::array()}},
                               {base}, herramientas(), politica());
  CHECK_EQ(jstr(b["spec"], "id"), std::string("soporte"));
  CHECK_EQ(jstr(b["spec"], "name"), std::string("Soporte IT"));
  CHECK_EQ(jstr(b["spec"], "system_prompt"), std::string("Mi prompt"));
  CHECK_EQ(b["spec"]["keywords"][0].get<std::string>(), std::string("impresora"));

  bool falla = false;
  try { asistente::borrador(json{{"plantilla", "blanco"}}, {}, herramientas(), politica()); } catch (const std::invalid_argument&) { falla = true; }
  CHECK(falla);
}

TEST("asistente: al ajustar se conservan «todas» y las herramientas sin grupo") {
  AgentSpec todo = agente("nexus", "Agente IA", {});
  todo.tools = {"*"};
  json todos = json::array();
  for (auto& g : asistente::grupos_disponibles(herramientas(), politica())) todos.push_back(g["id"]);
  json b = asistente::borrador(json{{"base", "nexus"}, {"grupos", todos}}, {todo}, herramientas(), politica());
  CHECK_EQ(b["spec"]["tools"].dump(), std::string("[\"*\"]"));

  AgentSpec raro = agente("gimp", "Retocador", {});
  raro.tools = {"gimp_script", "memory_search"};
  b = asistente::borrador(json{{"base", "gimp"}, {"grupos", {"calculo"}}}, {raro}, herramientas(), politica());
  std::string t = b["spec"]["tools"].dump();
  CHECK(t.find("gimp_script") != std::string::npos);
  CHECK(t.find("memory_search") == std::string::npos);
  CHECK(t.find("calculator") != std::string::npos);
}
