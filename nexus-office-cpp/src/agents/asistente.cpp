// Asistente de configuracion de agentes (ver asistente.h).
#include "agents/asistente.h"

#include <algorithm>
#include <map>
#include <set>

#include "agents/agents.h"
#include "util/text.h"

namespace nx::agents::asistente {

namespace {

// grupos de herramientas tal como los entiende quien no es tecnico; «red» = necesita internet
struct Grupo {
  const char* id;
  const char* nombre;
  const char* explicacion;
  std::vector<std::string> herramientas;
  bool red;
};

const std::vector<Grupo>& grupos() {
  static const std::vector<Grupo> G = {
      {"memoria", "Memoria", "Recuerda lo que le cuentas y consulta lo aprendido",
       {"memory_search", "memory_save", "knowledge_graph"}, false},
      {"biblioteca", "Biblioteca de documentos", "Busca en los documentos, manuales y PDF que has importado",
       {"rag_search", "rag_list"}, false},
      {"ficheros", "Leer ficheros", "Abre y lee los ficheros de su carpeta de trabajo", {"file_list", "file_read"}, false},
      {"escribir", "Crear y modificar ficheros", "Guarda informes, borradores y tablas en su carpeta de trabajo",
       {"file_write"}, false},
      {"calculo", "Cálculos y fechas", "Calculadora y fecha y hora actuales", {"calculator", "datetime_now"}, false},
      {"internet", "Internet", "Busca en la web, lee páginas y archiva documentación",
       {"web_search", "web_fetch", "url_to_pdf", "crawl_docs"}, true},
      {"imagenes", "Imágenes", "Crea y retoca imágenes en este equipo",
       {"generate_image", "enhance_prompt", "imagen_editar", "imagen_info"}, false},
      {"musica", "Música", "Compone piezas musicales cortas", {"compose_music"}, false},
      {"codigo", "Ejecutar código", "Ejecuta pequeños programas para calcular o transformar datos", {"python_exec"},
       false},
      {"bolsa", "Bolsa", "Cotizaciones, cartera simulada y órdenes (siempre las confirmas tú)",
       {"bolsa_mercados", "bolsa_cotizacion", "bolsa_cartera", "bolsa_costes", "bolsa_orden", "bolsa_cancelar",
        "bolsa_informe_fiscal"},
       true},
      {"equipo", "Trabajo en equipo", "Pide ayuda a otros agentes y al Profesor",
       {"delegate", "list_agents", "consultar_profesor"}, false},
      {"sistema", "Datos del equipo", "Consulta la memoria, el disco y el procesador del equipo", {"system_info"},
       false},
  };
  return G;
}

struct Plantilla {
  const char* id;
  const char* nombre;
  const char* icono;
  const char* color;
  const char* descripcion;
  const char* mision;
  std::vector<std::string> tareas;
  std::vector<std::string> palabras;
  const char* especialidad;
  std::vector<std::string> grupos;
  const char* tono;
  const char* creatividad;
};

const std::vector<Plantilla>& plantillas() {
  static const std::vector<Plantilla> P = {
      {"atencion", "Atención al cliente", "☎", "#4fc3f7", "Responde a clientes con amabilidad y resuelve sus dudas",
       "atender a los clientes, resolver sus dudas y dejar constancia de cada caso",
       {"Contesta preguntas sobre productos, horarios, precios y pedidos",
        "Redacta respuestas a correos y mensajes de clientes",
        "Si hay una queja, recoge los datos y propone una solución"},
       {"cliente", "clientes", "pedido", "reclamación", "queja", "devolución", "horario", "atención al cliente",
        "responder correo"},
       "atención al cliente, pedidos, reclamaciones, devoluciones, preguntas frecuentes",
       {"memoria", "biblioteca", "calculo", "equipo"}, "cercano", "equilibrado"},
      {"comercial", "Comercial", "€", "#ffb74d", "Prepara ofertas, presupuestos y seguimiento de ventas",
       "ayudar a vender: preparar ofertas y presupuestos y hacer el seguimiento de clientes potenciales",
       {"Prepara presupuestos y ofertas a partir de la tarifa", "Redacta correos comerciales y de seguimiento",
        "Resume el estado de cada oportunidad de venta"},
       {"presupuesto", "oferta", "venta", "ventas", "tarifa", "descuento", "cliente potencial", "seguimiento"},
       "ventas, presupuestos, ofertas, tarifas, seguimiento comercial", {"memoria", "biblioteca", "calculo", "escribir", "equipo"},
       "cercano", "equilibrado"},
      {"contable", "Contabilidad", "∑", "#81c784", "Facturas, gastos, impuestos y cuentas del negocio",
       "llevar las cuentas: revisar facturas y gastos, calcular impuestos y explicar los números con claridad",
       {"Revisa facturas y gastos y detecta errores", "Calcula IVA, retenciones y totales",
        "Prepara resúmenes de ingresos y gastos por periodo"},
       {"factura", "facturas", "gasto", "gastos", "iva", "irpf", "impuesto", "contabilidad", "balance", "nómina"},
       "contabilidad, facturación, impuestos, IVA, IRPF, gastos, tesorería",
       {"memoria", "biblioteca", "ficheros", "escribir", "calculo"}, "formal", "preciso"},
      {"rrhh", "Personal y RRHH", "☺", "#ba68c8", "Vacaciones, turnos, contratos y dudas del equipo",
       "ayudar con la gestión del personal: turnos, vacaciones, permisos y documentación del equipo",
       {"Organiza turnos y calendarios de vacaciones", "Responde dudas sobre permisos y convenio",
        "Prepara borradores de comunicados internos"},
       {"vacaciones", "turno", "turnos", "permiso", "contrato", "convenio", "empleado", "plantilla", "baja"},
       "recursos humanos, turnos, vacaciones, permisos, contratos, convenio",
       {"memoria", "biblioteca", "calculo", "escribir"}, "cercano", "preciso"},
      {"redactor", "Marketing y redacción", "✎", "#f06292", "Textos para la web, redes sociales y folletos",
       "escribir textos que vendan: publicaciones, anuncios, fichas de producto y boletines",
       {"Escribe publicaciones para redes sociales", "Redacta fichas de producto y textos para la web",
        "Propone ideas de campañas y eslóganes"},
       {"redes sociales", "publicación", "anuncio", "eslogan", "marketing", "campaña", "texto", "boletín",
        "instagram"},
       "marketing, redacción publicitaria, redes sociales, contenidos, marca",
       {"memoria", "biblioteca", "imagenes", "escribir"}, "cercano", "creativo"},
      {"soporte", "Soporte técnico", "⚙", "#90a4ae", "Resuelve problemas con ordenadores, programas y red",
       "resolver incidencias informáticas de la oficina paso a paso y con instrucciones claras",
       {"Diagnostica problemas de ordenadores, impresoras y red",
        "Explica soluciones paso a paso para quien no es técnico", "Lleva un registro de incidencias"},
       {"ordenador", "impresora", "wifi", "red", "contraseña", "error", "no funciona", "incidencia", "instalar"},
       "soporte informático, incidencias, redes, impresoras, ordenadores",
       {"memoria", "biblioteca", "sistema", "ficheros"}, "tecnico", "preciso"},
      {"legal", "Documentación y normativa", "§", "#a1887f", "Contratos, normativa y documentos oficiales",
       "revisar documentos y explicar la normativa aplicable sin sustituir a un profesional",
       {"Resume contratos y documentos oficiales", "Explica obligaciones y plazos de la normativa",
        "Prepara borradores de escritos y avisa de lo que debe revisar un profesional"},
       {"contrato", "ley", "normativa", "rgpd", "protección de datos", "plazo", "cláusula", "documento oficial"},
       "normativa, contratos, protección de datos, documentación legal",
       {"memoria", "biblioteca", "ficheros", "escribir"}, "formal", "preciso"},
      {"blanco", "En blanco", "◆", "#00f0ff", "Un agente a tu medida", "", {}, {}, "", {"memoria", "calculo"},
       "equilibrado", "equilibrado"},
  };
  return P;
}

const std::map<std::string, std::string>& tonos() {
  static const std::map<std::string, std::string> T = {
      {"formal", "formal y respetuoso, trata de usted salvo que te pidan lo contrario"},
      {"cercano", "cercano y amable, tutea y usa frases sencillas"},
      {"tecnico", "técnico y preciso, con pasos numerados cuando expliques algo"},
      {"breve", "muy breve: ve al grano y responde en pocas frases"},
      {"equilibrado", "claro y natural, ni demasiado formal ni demasiado informal"},
  };
  return T;
}

const std::map<std::string, double>& creatividades() {
  static const std::map<std::string, double> C = {{"preciso", 0.2}, {"equilibrado", 0.5}, {"creativo", 0.9}};
  return C;
}

const Plantilla* buscar_plantilla(const std::string& id) {
  for (auto& p : plantillas())
    if (id == p.id) return &p;
  return nullptr;
}

std::vector<std::string> lista(const json& v) {   // array o texto separado por comas o lineas
  std::vector<std::string> out;
  auto add = [&](const std::string& s) {
    std::string t = trim(s);
    if (!t.empty()) out.push_back(t);
  };
  if (v.is_array()) {
    for (auto& x : v)
      if (x.is_string()) add(x.get<std::string>());
  } else if (v.is_string()) {
    for (auto& l : splitlines(v.get<std::string>()))
      for (auto& p : split(l, ",")) add(p);
  }
  return out;
}

std::vector<std::string> sin_repetir(const std::vector<std::string>& v) {
  std::vector<std::string> out;
  std::set<std::string> vistos;
  for (auto& x : v)
    if (vistos.insert(normalize(x)).second) out.push_back(x);
  return out;
}

std::string txt(const json& r, const char* k, const std::string& def = "") {
  return r.contains(k) && r[k].is_string() && !trim(r[k].get<std::string>()).empty() ? trim(r[k].get<std::string>())
                                                                                    : def;
}

}  // namespace

json catalogo() {
  json ps = json::array();
  for (auto& p : plantillas())
    ps.push_back({{"id", p.id},
                  {"nombre", p.nombre},
                  {"icono", p.icono},
                  {"color", p.color},
                  {"descripcion", p.descripcion},
                  {"mision", p.mision},
                  {"tareas", p.tareas},
                  {"palabras", p.palabras},
                  {"especialidad", p.especialidad},
                  {"grupos", p.grupos},
                  {"tono", p.tono},
                  {"creatividad", p.creatividad}});
  json gs = json::array();
  for (auto& g : grupos())
    gs.push_back({{"id", g.id}, {"nombre", g.nombre}, {"explicacion", g.explicacion}, {"herramientas", g.herramientas},
                  {"red", g.red}});
  json ts = json::array();
  for (auto id : {"cercano", "formal", "tecnico", "breve", "equilibrado"}) ts.push_back({{"id", id}, {"texto", tonos().at(id)}});
  return {{"plantillas", ps},
          {"grupos", gs},
          {"tonos", ts},
          {"creatividad", {{{"id", "preciso"}, {"texto", "Preciso: siempre igual, para números y datos"}},
                           {{"id", "equilibrado"}, {"texto", "Equilibrado"}},
                           {{"id", "creativo"}, {"texto", "Creativo: ideas y textos variados"}}}},
          {"longitud", {{{"id", "breve"}, {"texto", "Respuestas cortas"}},
                        {{"id", "normal"}, {"texto", "Lo que haga falta"}},
                        {{"id", "extensa"}, {"texto", "Respuestas largas y detalladas"}}}}};
}

json grupos_disponibles(const json& herramientas, const json& politica) {
  std::map<std::string, std::string> riesgo;
  if (herramientas.is_array())
    for (auto& h : herramientas)
      if (h.is_object() && h.contains("name")) riesgo[jstr(h, "name")] = h.value("risk", std::string("low"));
  static const std::map<std::string, int> ORDEN = {{"low", 0}, {"medium", 1}, {"high", 2}};
  json out = json::array();
  for (auto& g : grupos()) {
    std::vector<std::string> hay;
    std::string max = "low";
    for (auto& t : g.herramientas) {
      auto it = riesgo.find(t);
      if (it == riesgo.end()) continue;
      hay.push_back(t);
      if (ORDEN.count(it->second) && ORDEN.at(it->second) > ORDEN.at(max)) max = it->second;
    }
    if (hay.empty()) continue;
    std::string pol = politica.is_object() && politica.contains(max) ? jstr(politica, max) : "admin";
    out.push_back({{"id", g.id},
                   {"nombre", g.nombre},
                   {"explicacion", g.explicacion},
                   {"herramientas", hay},
                   {"red", g.red},
                   {"riesgo", max},
                   {"aprobacion", pol}});   // auto | ask (la confirma el usuario) | admin | deny
  }
  return out;
}

std::string id_desde_nombre(const std::string& nombre, const std::vector<std::string>& ocupados) {
  std::string base = slug(strip_accents(nombre), 26);
  if (base.empty()) base = "agente";
  std::set<std::string> o(ocupados.begin(), ocupados.end());
  std::string id = base;
  for (int n = 2; o.count(id); ++n) id = base + "-" + std::to_string(n);
  return id;
}

json borrador(const json& r, const std::vector<AgentSpec>& existentes, const json& herramientas, const json& politica) {
  if (!r.is_object()) throw std::invalid_argument("respuestas no válidas");
  const Plantilla* pl = buscar_plantilla(txt(r, "plantilla", "blanco"));
  if (!pl) pl = buscar_plantilla("blanco");

  // punto de partida: un agente existente (para ajustarlo) o la plantilla
  std::optional<AgentSpec> base;
  std::string base_id = txt(r, "base");
  for (auto& a : existentes)
    if (!base_id.empty() && a.id == base_id) base = a;

  std::string nombre = txt(r, "nombre", base ? base->name : "");
  if (nombre.empty()) throw std::invalid_argument("Ponle un nombre al agente");

  json avisos = json::array();
  auto aviso = [&](const std::string& tipo, const std::string& texto) { avisos.push_back({{"tipo", tipo}, {"texto", texto}}); };

  // id: se conserva al editar; si es nuevo sale del nombre y no pisa a ninguno
  std::vector<std::string> ocupados;
  for (auto& a : existentes) ocupados.push_back(a.id);
  std::string id = base ? base->id : txt(r, "id");
  if (!base) {
    if (id.empty() || std::find(ocupados.begin(), ocupados.end(), id) != ocupados.end())
      id = id_desde_nombre(id.empty() ? nombre : id, ocupados);
  }

  AgentSpec s = base ? *base : AgentSpec{};
  s.id = id;
  s.name = nombre;
  s.description = txt(r, "descripcion", base ? base->description : pl->descripcion);
  s.icon = txt(r, "icono", base ? base->icon : pl->icono);
  s.color = txt(r, "color", base ? base->color : pl->color);
  if (r.contains("sexo") && r["sexo"].is_string()) s.sexo = r["sexo"].get<std::string>();
  if (r.contains("memoria")) s.memory = truthy(r["memoria"]);
  s.enabled = true;

  // comportamiento: tono, creatividad y longitud
  std::string tono = txt(r, "tono", pl->tono);
  if (!tonos().count(tono)) tono = "equilibrado";
  std::string crea = txt(r, "creatividad", pl->creatividad);
  if (!creatividades().count(crea)) crea = "equilibrado";
  s.temperature = creatividades().at(crea);
  std::string largo = txt(r, "longitud", "normal");
  s.max_tokens.reset();
  if (largo == "breve") s.max_tokens = 512;
  if (largo == "extensa") s.max_tokens = 2048;

  // herramientas: las de los grupos elegidos que existen en esta instalacion
  json disponibles = grupos_disponibles(herramientas, politica);
  std::vector<std::string> elegidos = r.contains("grupos") ? lista(r["grupos"]) : pl->grupos;
  std::vector<std::string> tools;
  bool red = false;
  for (auto& g : disponibles) {
    if (std::find(elegidos.begin(), elegidos.end(), jstr(g, "id")) == elegidos.end()) continue;
    for (auto& t : g["herramientas"]) tools.push_back(t.get<std::string>());
    if (g["red"].get<bool>()) red = true;
    std::string ap = jstr(g, "aprobacion");
    if (ap == "ask")
      aviso("aprobacion", cat("«", jstr(g, "nombre"), "»: te pedirá permiso antes de usarlo"));
    else if (ap == "admin")
      aviso("aprobacion", cat("«", jstr(g, "nombre"), "»: necesita el permiso de un administrador cada vez"));
    else if (ap == "deny")
      aviso("aprobacion", cat("«", jstr(g, "nombre"), "»: está bloqueado por la política de herramientas"));
  }
  for (auto& e : elegidos) {
    bool hay = false;
    for (auto& g : disponibles) hay = hay || jstr(g, "id") == e;
    if (!hay) aviso("herramientas", cat("«", e, "» no está disponible en este equipo y se omite"));
  }
  std::vector<std::string> delegar = r.contains("delegar") ? lista(r["delegar"]) : std::vector<std::string>{};
  std::vector<std::string> validos;
  for (auto& d : delegar)
    if (d == "*" || std::any_of(existentes.begin(), existentes.end(), [&](const AgentSpec& a) { return a.id == d; }))
      validos.push_back(d);
  s.can_delegate_to = validos;
  if (!validos.empty() && std::find(tools.begin(), tools.end(), "delegate") == tools.end()) tools.push_back("delegate");
  if (base) {   // al ajustar uno existente se conservan las herramientas que no estan en ningun grupo
    std::set<std::string> agrupadas;
    for (auto& g : grupos()) agrupadas.insert(g.herramientas.begin(), g.herramientas.end());
    bool todos = std::all_of(disponibles.begin(), disponibles.end(), [&](const json& g) {
      return std::find(elegidos.begin(), elegidos.end(), jstr(g, "id")) != elegidos.end();
    });
    for (auto& t : base->tools)
      if (t == "*" && todos) tools = {"*"};
      else if (t != "*" && !agrupadas.count(t)) tools.push_back(t);
  }
  s.tools = sin_repetir(tools);
  if (red)
    aviso("internet", "Usa herramientas de internet: sin conexión el agente sigue funcionando en local, pero sin ellas");

  // especialidad y palabras clave de enrutado
  s.specialty = txt(r, "especialidad", base ? base->specialty : pl->especialidad);
  std::vector<std::string> palabras = base ? base->keywords : pl->palabras;
  if (r.contains("palabras")) {
    auto extra = lista(r["palabras"]);
    if (r.value("reemplazar_palabras", false)) palabras.clear();
    palabras.insert(palabras.end(), extra.begin(), extra.end());
  }
  std::vector<std::string> norm;
  for (auto& p : palabras) norm.push_back(lower(trim(p)));
  s.keywords = sin_repetir(norm);
  for (auto& k : s.keywords)
    for (auto& a : existentes)
      if (a.id != s.id && a.enabled &&
          std::any_of(a.keywords.begin(), a.keywords.end(), [&](const std::string& x) { return normalize(x) == normalize(k); }))
        aviso("palabras", cat("«", k, "» ya lleva a ", a.name, ": decide cuál de los dos debe atender esos mensajes"));
  if (s.keywords.empty())
    aviso("palabras", "Sin palabras clave solo le llegarán mensajes si lo eliges tu o lo delega otro agente");

  // prompt de sistema: se respeta el escrito a mano; si no, se compone con las respuestas
  std::string prompt = txt(r, "system_prompt");
  if (prompt.empty()) {
    std::string mision = txt(r, "mision", pl->mision);
    std::vector<std::string> tareas = r.contains("tareas") ? lista(r["tareas"]) : pl->tareas;
    std::vector<std::string> reglas = r.contains("reglas") ? lista(r["reglas"]) : std::vector<std::string>{};
    std::string idioma = txt(r, "idioma", "español");
    std::string p = cat("Eres ", nombre, ", agente de {assistant_name}");
    if (!mision.empty()) p += cat(". Tu trabajo es ", mision);
    p += ".\n";
    if (!tareas.empty()) {
      p += "Tareas habituales:\n";
      for (auto& t : tareas) p += "- " + t + "\n";
    }
    p += cat("Tu tono es ", tonos().at(tono), ". Respondes en ", idioma, ".\n");
    if (largo == "breve") p += "Tus respuestas son cortas: lo esencial en pocas líneas.\n";
    if (largo == "extensa") p += "Cuando ayude, das respuestas completas y detalladas, con ejemplos.\n";
    if (!reglas.empty()) {
      p += "Normas que siempre cumples:\n";
      for (auto& t : reglas) p += "- " + t + "\n";
    }
    auto tiene = [&](const char* t) { return std::find(s.tools.begin(), s.tools.end(), t) != s.tools.end(); };
    if (tiene("rag_search")) p += "Antes de responder sobre la empresa, consulta la biblioteca de documentos con rag_search.\n";
    if (tiene("memory_save")) p += "Guarda con memory_save los datos importantes que te den para no volver a preguntarlos.\n";
    if (tiene("web_search")) p += "Si necesitas datos actuales de internet usa web_search y cita la fuente.\n";
    if (tiene("file_write")) p += "Los informes y borradores largos los guardas como fichero con file_write.\n";
    if (!validos.empty()) p += "Si una petición no es de tu especialidad, delégala con delegate en el agente adecuado.\n";
    else p += "Si una petición no es de tu especialidad, dilo con claridad y sugiere a quien acudir.\n";
    p += "Nunca inventes datos: si no lo sabes, dilo y pregunta.";
    prompt = p;
  }
  s.system_prompt = prompt;

  // prueba de enrutado por palabras clave: a quien llegaria cada frase de ejemplo
  json enrutado = json::array();
  for (auto& frase : r.contains("frases_prueba") ? lista(r["frases_prueba"]) : std::vector<std::string>{}) {
    double mio = AgentManager::keyword_score(s, frase), mejor = 0;
    std::string quien;
    for (auto& a : existentes) {
      if (a.id == s.id || !a.enabled || a.id == "nexus") continue;
      double v = AgentManager::keyword_score(a, frase);
      if (v > mejor) mejor = v, quien = a.name;
    }
    std::string gana = mio > 0 && mio >= mejor ? s.name : (mejor > 0 ? quien : "el agente principal");
    enrutado.push_back({{"frase", frase}, {"puntos", mio}, {"rival", quien}, {"puntos_rival", mejor}, {"llega_a", gana},
                        {"ok", mio > 0 && mio >= mejor}});
  }
  return {{"spec", s.to_json()}, {"avisos", avisos}, {"enrutado", enrutado}};
}

}  // namespace nx::agents::asistente
