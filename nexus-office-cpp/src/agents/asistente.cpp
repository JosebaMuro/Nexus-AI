// Asistente de configuracion de agentes (ver asistente.h).
#include "agents/asistente.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "agents/agents.h"
#include "memory/memory.h"
#include "office/office.h"
#include "util/fs.h"
#include "util/text.h"
#include "util/yaml.h"

namespace nx::agents::asistente {

namespace {

struct Paquete {
  const char* id;
  const char* nombre;
  const char* icono;
  const char* desc;
  std::vector<std::string> tools;
};

// cada herramienta en un solo paquete; "basico" va siempre marcado de inicio
const std::vector<Paquete>& PAQ() {
  static const std::vector<Paquete> v = {
      {"basico", "Memoria, cálculo y fecha", "🧠", "Recuerda lo que le cuentas, calcula sin errores y sabe qué día es.",
       {"memory_search", "memory_save", "calculator", "datetime_now"}},
      {"documentos", "Documentos de la empresa", "📚",
       "Consulta la biblioteca, el grafo de conocimiento y los ficheros de la carpeta de trabajo.",
       {"rag_search", "rag_list", "knowledge_graph", "graph_import", "file_list", "file_read"}},
      {"escribir", "Crear ficheros", "📝", "Guarda informes, presupuestos y textos en la carpeta de trabajo (pide permiso).",
       {"file_write"}},
      {"internet", "Internet", "🌐", "Busca y lee páginas web (solo si hay conexión).",
       {"web_search", "web_fetch", "url_to_pdf", "crawl_docs"}},
      {"creativo", "Imágenes y música", "🎨", "Crea y retoca imágenes, mejora prompts y compone música.",
       {"generate_image", "enhance_prompt", "imagen_info", "imagen_editar", "gimp_estado", "gimp_script",
        "compose_music"}},
      {"codigo", "Programar", "⌘", "Ejecuta código para cálculos, datos y automatizaciones (pide permiso).",
       {"python_exec"}},
      {"equipo", "Trabajo en equipo", "🤝", "Pasa a otros agentes lo que es de su especialidad.",
       {"delegate", "list_agents", "consultar_profesor", "propose_tool"}},
      {"bolsa", "Bolsa (simulada)", "📈", "Mercados, cotizaciones, cartera y órdenes simuladas.",
       {"bolsa_mercados", "bolsa_cotizacion", "bolsa_cartera", "bolsa_costes", "bolsa_orden", "bolsa_cancelar",
        "bolsa_informe_fiscal"}},
  };
  return v;
}

const Paquete* paquete_de(const std::string& tool) {
  for (auto& p : PAQ())
    if (std::find(p.tools.begin(), p.tools.end(), tool) != p.tools.end()) return &p;
  return nullptr;
}

std::vector<std::string> lista(const json& v) {
  std::vector<std::string> out;
  auto add = [&](std::string s) {
    s = trim(s);
    if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  };
  if (v.is_array()) {
    for (auto& x : v)
      if (x.is_string()) add(x.get<std::string>());
  } else if (v.is_string()) {
    for (auto& x : split(v.get<std::string>(), ",")) add(x);
  }
  return out;
}

bool tiene(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

std::string opcion(const json& r, const char* k, const std::vector<std::string>& validas) {
  std::string v = jstr(r, k);
  return tiene(validas, v) ? v : validas[0];
}

// creatividad <-> temperatura
double temperatura(const std::string& c) { return c == "precisa" ? 0.2 : c == "creativa" ? 0.9 : 0.5; }
std::string creatividad(double t) { return t <= 0.35 ? "precisa" : t <= 0.7 ? "equilibrada" : "creativa"; }

json defaults_cache() {
  json d = [] {
    json out = json::object();
    auto files = glob(defaults_dir(), "*.yaml");
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
      try {
        json y = yaml::load(read_text(f));
        if (y.is_object() && !jstr(y, "id").empty() && jstr(y, "id") != "nexus") out[jstr(y, "id")] = y;
      } catch (...) {
      }
    }
    return out;
  }();
  return d;
}

json catalogo() { return office::catalog(); }

}  // namespace

json paquetes() {
  json out = json::array();
  for (auto& p : PAQ())
    out.push_back({{"id", p.id}, {"nombre", p.nombre}, {"icono", p.icono}, {"desc", p.desc}, {"tools", p.tools}});
  return out;
}

std::string id_de(const std::string& nombre) { return slug(strip_accents(nombre), 30); }

json perfil_de(const json& t) {
  bool hay = t.is_object();
  std::vector<std::string> tools = hay ? lista(t.value("tools", json())) : std::vector<std::string>{};
  json paq = json::array();
  if (tiene(tools, "*")) {
    paq.push_back("todas");
  } else {
    std::set<std::string> ids = {"basico"};
    if (!hay) ids.insert({"documentos", "equipo"});
    for (auto& tl : tools)
      if (auto* p = paquete_de(tl)) ids.insert(p->id);
    for (auto& p : PAQ())   // en el orden de los paquetes
      if (ids.count(p.id)) paq.push_back(p.id);
  }
  json t_ = hay ? t : json::object();
  double temp = t_.contains("temperature") && t_["temperature"].is_number() ? t_["temperature"].get<double>() : 0.5;
  return {{"plantilla", jstr(t_, "id")},
          {"nombre", jstr(t_, "name")},
          {"puesto", hay ? lower(jstr(t_, "name")) : ""},
          {"descripcion", jstr(t_, "description")},
          {"icono", jstr(t_, "icon", "◆")},
          {"color", jstr(t_, "color", "#00f0ff")},
          {"sexo", jstr(t_, "sexo")},
          {"trato", "cercano"},
          {"longitud", "normales"},
          {"creatividad", creatividad(temp)},
          {"proactivo", true},
          {"idioma", "es"},
          {"reglas", ""},
          {"especialidad", jstr(t_, "specialty")},
          {"palabras", lista(t_.value("keywords", json()))},
          {"paquetes", paq},
          {"modelo", jstr(t_, "model", "default")},
          {"memoria", hay ? truthy(t_.value("memory", json(true))) : true},
          {"delegar", lista(t_.value("can_delegate_to", json()))},
          {"prompt_manual", ""}};
}

json plantillas() {
  json out = json::array();
  auto add = [&](const json& t, const char* grupo) {
    out.push_back({{"id", jstr(t, "id")},
                   {"name", jstr(t, "name")},
                   {"description", jstr(t, "description")},
                   {"icon", jstr(t, "icon", "◆")},
                   {"color", jstr(t, "color", "#00f0ff")},
                   {"grupo", grupo},
                   {"conocimiento", t.contains("grafo") && t["grafo"].is_array() ? t["grafo"].size() : 0},
                   {"perfil", perfil_de(t)}});
  };
  json def = defaults_cache(), cat = catalogo();
  for (auto& [id, t] : def.items()) add(t, "General");
  for (auto& [id, t] : cat.items())
    if (!office::is_trade(id)) add(t, "Oficina");
  for (auto& [id, t] : cat.items())
    if (office::is_trade(id)) add(t, "Oficios y profesiones");
  return out;
}

json plantilla(const std::string& id) {
  if (id.empty()) return nullptr;
  json cat = catalogo();
  if (cat.contains(id)) return cat[id];
  json def = defaults_cache();
  if (def.contains(id)) return def[id];
  return nullptr;
}

std::string redactar_prompt(const json& t, const json& r) {
  std::string nombre = trim(jstr(r, "nombre"));
  std::string base = t.is_object() ? trim(jstr(t, "system_prompt")) : "";
  std::vector<std::string> partes;
  if (!base.empty()) {
    if (!nombre.empty() && lower(nombre) != lower(jstr(t, "name"))) partes.push_back("Te llamas " + nombre + ".");
    partes.push_back(base);
  } else {
    std::string puesto = trim(jstr(r, "puesto"));
    std::string desc = trim(jstr(r, "descripcion"));
    std::string l = "Eres " + (nombre.empty() ? std::string("un agente") : nombre) + ", " +
                    (puesto.empty() ? std::string("agente especialista") : puesto) + " de {assistant_name}.";
    if (!desc.empty()) l += " " + (ends_with(desc, ".") ? desc : desc + ".");
    partes.push_back(l);
    std::string esp = trim(jstr(r, "especialidad"));
    if (!esp.empty()) partes.push_back("Tu especialidad: " + esp + ".");
  }

  std::vector<std::string> forma;
  std::string trato = opcion(r, "trato", {"cercano", "usted", "desenfadado"});
  forma.push_back(trato == "usted"        ? "Trata de usted, con un registro formal y profesional."
                  : trato == "desenfadado" ? "Habla con un tono muy cercano y desenfadado, tuteando."
                                           : "Habla con un tono cercano y amable, tuteando.");
  std::string resp = opcion(r, "longitud", {"normales", "breves", "detalladas"});
  forma.push_back(resp == "breves"       ? "Responde breve y al grano: pocas frases o una lista corta."
                  : resp == "detalladas" ? "Responde de forma detallada y ordenada (apartados, listas y ejemplos)."
                                         : "Responde con la extensión justa: completo pero sin rodeos.");
  forma.push_back(truthy(r.value("proactivo", json(true)))
                      ? "Si ves algo importante que no te han pedido (un riesgo, un paso siguiente), menciónalo al final."
                      : "Haz solo lo que se te pide; si falta un dato, pregunta antes de actuar.");
  std::string idioma = opcion(r, "idioma", {"es", "auto"});
  forma.push_back(idioma == "auto" ? "Responde en el idioma en el que te escriban." : "Responde siempre en español.");
  for (auto& linea : splitlines(jstr(r, "reglas"))) {
    std::string x = trim(trim(linea), "-•* ");
    if (!x.empty()) forma.push_back(x);
  }
  std::string txt = join(partes, "\n\n") + "\n\nForma de trabajar:";
  for (auto& f : forma) txt += "\n- " + f;
  return txt;
}

json construir(const json& r_in, const std::vector<std::string>& existentes, const std::vector<std::string>& agentes,
               const std::string& editando) {
  if (!r_in.is_object()) throw std::invalid_argument("faltan las respuestas del asistente");
  json t = plantilla(jstr(r_in, "plantilla"));
  // lo que no venga se toma de la plantilla (o de los valores por defecto)
  json r = perfil_de(t);
  for (auto& [k, v] : r_in.items())
    if (!v.is_null()) r[k] = v;
  if (t.is_null()) r["plantilla"] = "";

  json avisos = json::array();
  std::string nombre = trim(jstr(r, "nombre"));
  if (nombre.empty()) throw std::invalid_argument("ponle un nombre al agente");

  // identificador: el que se esta editando, o uno nuevo a partir del nombre (libre)
  std::string id = editando;
  if (id.empty()) {
    std::string base = id_de(trim(jstr(r, "id")).empty() ? nombre : jstr(r, "id"));
    if (base.empty()) base = "agente";
    id = base;
    for (int n = 2; tiene(agentes, id); ++n) id = base.substr(0, 27) + "-" + std::to_string(n);
    if (id != base) avisos.push_back("Ya hay un agente «" + base + "»: este se guarda como «" + id + "».");
  }

  // herramientas: los paquetes elegidos + las de la plantilla que no son de ningun paquete
  std::vector<std::string> paq = lista(r.value("paquetes", json()));
  std::vector<std::string> tools;
  if (tiene(paq, "todas")) {
    tools = {"*"};
  } else {
    auto add = [&](const std::string& tl) {
      if (!tiene(tools, tl) && (existentes.empty() || tiene(existentes, tl))) tools.push_back(tl);
    };
    for (auto& p : PAQ())
      if (tiene(paq, p.id))
        for (auto& tl : p.tools) add(tl);
    if (t.is_object())
      for (auto& tl : lista(t.value("tools", json())))
        if (!paquete_de(tl) && tl != "*") add(tl);
  }
  r["paquetes"] = paq;

  bool memoria = truthy(r.value("memoria", json(true)));
  if (!memoria && (tiene(paq, "basico") || tiene(paq, "todas")))
    avisos.push_back("Sin memoria propia no recordará conversaciones anteriores (sí podrá consultar la común).");
  if (tools.empty()) avisos.push_back("No tiene herramientas: solo podrá conversar.");
  if (tiene(tools, "file_write") || tiene(tools, "python_exec") || tiene(tools, "*"))
    avisos.push_back("Puede escribir ficheros o ejecutar código: esas acciones te pedirán aprobación.");
  if (tiene(paq, "internet")) avisos.push_back("Internet solo funciona con conexión; sin ella usa lo que ya sabe.");

  // equipo: solo agentes que existen y nunca el mismo
  std::vector<std::string> delegar;
  bool puede_delegar = tiene(tools, "delegate") || tiene(tools, "*");
  if (puede_delegar)
    for (auto& a : lista(r.value("delegar", json())))
      if (a == "*" || (a != id && (agentes.empty() || tiene(agentes, a)))) delegar.push_back(a);
  r["delegar"] = delegar;

  std::vector<std::string> palabras = lista(r.value("palabras", json()));
  if (palabras.empty()) {   // sin palabras clave el enrutado automatico no lo encuentra: al menos su nombre y puesto
    palabras.push_back(lower(nombre));
    std::string p = lower(trim(jstr(r, "puesto")));
    if (!p.empty() && p != lower(nombre)) palabras.push_back(p);
  }
  r["palabras"] = palabras;

  std::string manual = trim(jstr(r, "prompt_manual"));
  std::string modelo = trim(jstr(r, "modelo"));
  json a = {{"id", id},
            {"name", nombre},
            {"description", trim(jstr(r, "descripcion"))},
            {"system_prompt", manual.empty() ? redactar_prompt(t, r) : manual},
            {"model", modelo.empty() ? "default" : modelo},
            {"temperature", temperatura(opcion(r, "creatividad", {"equilibrada", "precisa", "creativa"}))},
            {"max_tokens", nullptr},
            {"tools", tools},
            {"memory", memoria},
            {"can_delegate_to", delegar},
            {"keywords", palabras},
            {"specialty", trim(jstr(r, "especialidad"))},
            {"color", jstr(r, "color", "#00f0ff")},
            {"icon", jstr(r, "icono").empty() ? std::string("◆") : jstr(r, "icono")},
            {"enabled", true},
            {"sexo", opcion(r, "sexo", {"", "mujer", "hombre"})},
            {"perfil", r}};
  return {{"agente", a}, {"avisos", avisos}, {"conocimiento", t.is_object() && t.contains("grafo") && t["grafo"].is_array() ? t["grafo"].size() : 0}};
}

int sembrar(mem::KnowledgeGraph& graph, const std::string& aid, const json& t) {
  if (!t.is_object() || aid.empty()) return 0;
  auto items = office::seeds(t);
  if (items.empty()) return 0;
  auto& g = graph.for_agent(aid);
  int n = 0;
  graph.db.transaction([&] {
    for (auto& [s, rel, o, category] : items)
      if (g.relate(s, rel, o, "concepto", "concepto", 1.5, category, category)) {
        g.relate(g.self_name, "domina", s, "agente", "concepto", 0.5, category);
        ++n;
      }
  });
  return n;
}

}  // namespace nx::agents::asistente
