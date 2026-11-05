// Asistente de configuracion de agentes: plantillas, paquetes, prompt, identificadores y perfil guardado.
#include <algorithm>
#include <set>

#include "agents/asistente.h"
#include "agents/spec.h"
#include "check.h"
#include "util/text.h"
#include "util/yaml.h"

using namespace nx;
namespace as = nx::agents::asistente;

namespace {
bool en(const json& arr, const std::string& x) {
  for (auto& v : arr)
    if (v.is_string() && v.get<std::string>() == x) return true;
  return false;
}
}  // namespace

TEST("asistente: catalogo de puestos y paquetes de herramientas") {
  json pl = as::plantillas();
  CHECK(pl.is_array() && pl.size() > 50);   // oficios y puestos de oficina + agentes de serie
  bool fontanero = false, coordinador = false, general = false;
  for (auto& p : pl) {
    if (jstr(p, "id") == "fontanero") {
      fontanero = true;
      CHECK_EQ(jstr(p, "grupo"), std::string("Oficios y profesiones"));
      CHECK(p["conocimiento"].get<int>() > 0);
      CHECK(en(p["perfil"]["paquetes"], "basico") && en(p["perfil"]["paquetes"], "equipo"));
      CHECK_EQ(jstr(p["perfil"], "creatividad"), std::string("equilibrada"));   // temperatura 0.4
    }
    if (jstr(p, "id") == "nexus") coordinador = true;
    if (jstr(p, "grupo") == "General") general = true;
  }
  CHECK(fontanero && general && !coordinador);   // el coordinador no es una plantilla
  // cada herramienta en un solo paquete
  std::set<std::string> vistas;
  for (auto& p : as::paquetes())
    for (auto& t : p["tools"]) CHECK(vistas.insert(t.get<std::string>()).second);
  CHECK(vistas.count("memory_search") && vistas.count("delegate") && vistas.count("file_write"));
  CHECK_EQ(as::id_de("Atención al Cliente"), std::string("atencion-al-cliente"));
}

TEST("asistente: desde cero, prompt redactado con la forma de ser elegida") {
  json r = {{"nombre", "Lucía"}, {"puesto", "recepcionista"}, {"descripcion", "Atiende el teléfono y la agenda"},
            {"trato", "usted"}, {"longitud", "breves"}, {"creatividad", "precisa"}, {"proactivo", false},
            {"reglas", "No des precios\n- Despídete con el horario"}, {"paquetes", {"basico", "internet"}},
            {"especialidad", "agenda, citas"}};
  json out = as::construir(r, {}, {"nexus"});
  json a = out["agente"];
  CHECK_EQ(jstr(a, "id"), std::string("lucia"));
  CHECK_EQ(jstr(a, "name"), std::string("Lucía"));
  CHECK_EQ(a["temperature"].get<double>(), 0.2);
  std::string p = jstr(a, "system_prompt");
  CHECK(contains(p, "Eres Lucía, recepcionista de {assistant_name}. Atiende el teléfono y la agenda."));
  CHECK(contains(p, "Tu especialidad: agenda, citas."));
  CHECK(contains(p, "Trata de usted"));
  CHECK(contains(p, "Responde breve"));
  CHECK(contains(p, "pregunta antes de actuar"));
  CHECK(contains(p, "- No des precios") && contains(p, "- Despídete con el horario"));
  CHECK(en(a["tools"], "memory_search") && en(a["tools"], "web_search") && !en(a["tools"], "delegate"));
  CHECK(en(a["keywords"], "lucía"));   // sin palabras clave: al menos su nombre y su puesto
  CHECK(en(a["keywords"], "recepcionista"));
  CHECK(a["can_delegate_to"].empty());   // sin el paquete de equipo no delega
  bool aviso_internet = false;
  for (auto& x : out["avisos"]) aviso_internet |= contains(x.get<std::string>(), "Internet");
  CHECK(aviso_internet);
  // sin nombre no se puede crear
  bool lanza = false;
  try {
    as::construir(json{{"nombre", "  "}}, {}, {});
  } catch (const std::invalid_argument&) {
    lanza = true;
  }
  CHECK(lanza);
}

TEST("asistente: plantilla de oficio, id libre, herramientas instaladas y equipo valido") {
  json r = {{"plantilla", "fontanero"}, {"nombre", "Paco"}, {"delegar", {"electricista", "no-existe", "paco"}}};
  std::vector<std::string> instaladas = {"memory_search", "memory_save", "calculator", "delegate", "list_agents", "rag_search"};
  json out = as::construir(r, instaladas, {"nexus", "paco", "electricista"});
  json a = out["agente"];
  CHECK_EQ(jstr(a, "id"), std::string("paco-2"));   // "paco" ya existe
  CHECK(!out["avisos"].empty());
  std::string p = jstr(a, "system_prompt");
  CHECK(starts_with(p, "Te llamas Paco."));
  CHECK(contains(p, "Eres el Fontanero de {assistant_name}"));   // la base es la del oficio
  CHECK(contains(p, "Forma de trabajar:"));
  for (auto& t : a["tools"]) CHECK(std::find(instaladas.begin(), instaladas.end(), t.get<std::string>()) != instaladas.end());
  CHECK(en(a["tools"], "delegate"));
  CHECK_EQ(a["can_delegate_to"].size(), (size_t)2);   // solo los agentes que existen (el "paco" de antes es otro)
  CHECK(!en(a["can_delegate_to"], "no-existe"));
  CHECK(en(a["can_delegate_to"], "electricista"));
  CHECK(contains(jstr(a, "specialty"), "fontanería"));
  CHECK(out["conocimiento"].get<int>() > 0);
  // editando se mantiene el id; "todas" da todas las herramientas; el prompt manual manda
  json r2 = {{"plantilla", "fontanero"}, {"nombre", "Paco"}, {"paquetes", {"todas"}}, {"prompt_manual", "Eres Paco."}};
  json b = as::construir(r2, instaladas, {"paco"}, "paco")["agente"];
  CHECK_EQ(jstr(b, "id"), std::string("paco"));
  CHECK(en(b["tools"], "*") && b["tools"].size() == 1);
  CHECK_EQ(jstr(b, "system_prompt"), std::string("Eres Paco."));
}

TEST("asistente: el perfil se guarda en la ficha y vuelve igual del YAML") {
  json a = as::construir(json{{"plantilla", "comercial"}, {"nombre", "Marta"}, {"trato", "desenfadado"}}, {}, {})["agente"];
  auto spec = agents::AgentSpec::from_json(yaml::load(yaml::dump(agents::AgentSpec::from_json(a).to_json())));
  CHECK_EQ(jstr(spec.perfil, "plantilla"), std::string("comercial"));
  CHECK_EQ(jstr(spec.perfil, "trato"), std::string("desenfadado"));
  CHECK(spec.perfil["paquetes"].is_array() && !spec.perfil["paquetes"].empty());
  // una ficha sin perfil no lo escribe (los YAML de siempre no cambian)
  auto viejo = agents::AgentSpec::from_json(json{{"id", "x"}, {"name", "X"}});
  CHECK(!viejo.to_json().contains("perfil"));
}
