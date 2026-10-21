// Perfiles de oficina (nexus/office/__init__.py).
//
// Un perfil (perfiles/<id>.yaml) describe la empresa y elige del catalogo (agentes/<id>.yaml) los agentes de su
// oficina. Cada agente aprende en su propio subgrafo (scope=<id>), sembrado al aplicar el perfil con su clave
// "grafo". Aplicar un perfil no borra nada: los agentes que sobran se desactivan y se guarda copia de cada YAML.
#include "office/office.h"

#include <algorithm>
#include <set>

#include "agents/agents.h"
#include "core/services.h"
#include "memory/memory.h"
#include "rag/rag.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"
#include "util/yaml.h"

namespace nx::office {

static auto L = log::get("office");

const std::vector<std::string> OFFICE_ROLES = {"recepcion", "administracion", "presupuestos", "compras",
                                               "prl",       "rrhh",           "calidad",      "comercial",
                                               "agenda",    "proteccion-datos", "medioambiente"};
// herramientas del coordinador: todas las que funcionan sin internet
const std::vector<std::string> COORDINATOR_TOOLS = {
    "delegate",   "list_agents",  "memory_search", "memory_save", "knowledge_graph", "graph_import", "rag_search",
    "rag_list",   "calculator",   "datetime_now",  "file_read",   "file_write",      "file_list",    "system_info"};
const char* COORDINATOR_PROMPT = R"(Eres {assistant_name}, la oficina virtual de una pyme y coordinas a su equipo de agentes.
{context}
Atiendes al usuario (el dueño o el personal de la empresa) con trato cercano y profesional. Lo que es de un gremio
o de un departamento lo encargas con delegate al agente que corresponde y luego das una respuesta final ordenada;
lo sencillo lo resuelves tú. Calcula siempre con calculator. Si falta un dato (medidas, dirección, plazos), pídelo.
No inventes precios, normas ni plazos: si algo depende de la normativa vigente o de la gestoría, dilo.)";

fs::path catalog_dir() { return paths::package() / "office" / "agentes"; }
fs::path profiles_dir() { return paths::package() / "office" / "perfiles"; }
fs::path knowledge_dir() { return paths::package() / "office" / "conocimiento"; }

static json read_yaml(const fs::path& p) {
  json d = yaml::load(read_text(p));
  return d.is_object() ? d : json::object();
}

static std::vector<fs::path> sorted_glob(const fs::path& dir, const std::string& pat) {
  auto v = glob(dir, pat);
  std::sort(v.begin(), v.end());
  return v;
}

static json by_id(const fs::path& dir) {
  json out = json::object();
  for (auto& f : sorted_glob(dir, "*.yaml")) {
    json d = read_yaml(f);
    out[jstr(d, "id")] = d;
  }
  return out;
}

json catalog() { return by_id(catalog_dir()); }
json profiles() { return by_id(profiles_dir()); }

std::vector<std::tuple<std::string, std::string, std::string, std::string>> seeds(const json& agent) {
  std::vector<std::tuple<std::string, std::string, std::string, std::string>> out;
  json items = agent.value("grafo", json());
  if (!items.is_array()) return out;
  static const Rx WS(R"(\s+)");
  for (auto& item : items) {
    std::vector<std::string> parts;
    if (item.is_string()) {
      for (auto& p : split(item.get<std::string>(), "|")) parts.push_back(trim(p));
    } else if (item.is_array()) {
      for (auto& p : item) parts.push_back(trim(p.is_string() ? p.get<std::string>() : p.dump()));
    } else {
      continue;
    }
    if (parts.size() < 3 || parts[0].empty() || parts[1].empty() || parts[2].empty()) continue;
    std::string rel = WS.sub(lower(parts[1]), "_");
    out.emplace_back(parts[0], rel, parts[2], parts.size() > 3 && !parts[3].empty() ? parts[3] : "especialidad");
  }
  return out;
}

bool is_trade(const std::string& aid) {
  return std::find(OFFICE_ROLES.begin(), OFFICE_ROLES.end(), aid) == OFFICE_ROLES.end();
}

// Modelos de IA especificos que conviene sugerir a cada sector (ademas del general elegido por hardware).
static const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& sector_table() {
  static const std::vector<std::pair<std::string, std::string>> HEALTH = {
      {"medgemma-4b", "Terminología y documentación sanitaria (apoyo al profesional, no diagnostica)"},
      {"qwen3-embedding-0.6b", "Buscar en protocolos, prospectos y fichas de tu biblioteca"}};
  static const std::vector<std::pair<std::string, std::string>> DOCS = {
      {"qwen3.5-9b", "Razona mejor sobre normativa, contratos y expedientes largos"},
      {"qwen3-embedding-0.6b", "Buscar en tu archivo de documentos, leyes y plantillas"}};
  static const std::vector<std::pair<std::string, std::string>> IMAGE = {
      {"sdxl-turbo", "Bocetos, maquetas y propuestas visuales para clientes"},
      {"sd-turbo", "Bocetos y propuestas visuales (equipos sin GPU potente)"}};
  static const auto T = [] {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> t;
    for (auto p : {"clinica-dental", "fisioterapia", "farmacia", "optica", "ayuda-domicilio-residencia", "veterinaria"})
      t[p] = HEALTH;
    for (auto p : {"asesoria-gestoria", "despacho-abogados", "inmobiliaria", "arquitectura-ingenieria",
                   "academia-formacion", "autoescuela"})
      t[p] = DOCS;
    for (auto p : {"agencia-marketing", "imprenta-artes-graficas", "estudio-fotografia", "floristeria", "pintura",
                   "carpinteria", "textil-confeccion", "catering-eventos"})
      t[p] = IMAGE;
    t["servicios-informaticos"] = {{"qwen2.5-coder-7b", "Programación, scripts, redes y configuración de equipos"},
                                   {"qwen3-embedding-0.6b", "Buscar en manuales y documentación técnica"}};
    return t;
  }();
  return T;
}

json sector_models(const std::string& pid) {
  json out = json::object();
  if (pid.empty()) return out;
  json all = profiles();
  json p = all.contains(pid) ? all[pid] : json::object();
  for (auto& m : p.value("modelos", json::array()))
    if (m.is_object() && truthy(m.value("id", json()))) out[jstr(m, "id")] = jstr(m, "motivo");
  if (!out.empty()) return out;
  auto& t = sector_table();
  auto it = t.find(pid);
  if (it != t.end())
    for (auto& [id, why] : it->second) out[id] = why;
  return out;
}

json describe(const std::string& pid, const std::string& active) {
  json all = profiles(), cat = catalog();
  if (!all.contains(pid)) throw tools::ToolError("KeyError", pid);
  json p = all[pid];
  json agents = json::array();
  for (auto& aj : p.value("agents", json::array())) {
    std::string a = aj.get<std::string>();
    if (!cat.contains(a)) continue;
    json& c = cat[a];
    agents.push_back({{"id", a},
                      {"name", c["name"]},
                      {"icon", c.value("icon", json("◆"))},
                      {"color", c.value("color", json(""))},
                      {"gremio", is_trade(a)},
                      {"description", c.value("description", json(""))}});
  }
  return {{"id", p["id"]},
          {"name", p["name"]},
          {"sector", p.value("sector", json(""))},
          {"description", p.value("description", json(""))},
          {"icon", p.value("icon", json("◆"))},
          {"assistant_name", p.value("assistant_name", json(""))},
          {"agents", agents},
          {"active", pid == active},
          {"models", sector_models(pid)}};
}

json coordinator(const json& profile, const std::map<std::string, std::string>& agent_names) {
  json base = read_yaml(agents::defaults_dir() / "nexus.yaml");
  json coord = profile.value("coordinator", json());
  std::string ctx = coord.is_object() ? trim(jstr(coord, "context")) : "";
  std::vector<std::string> team;
  json members = profile.value("agents", json::array());
  for (auto& aj : members) {
    std::string a = aj.get<std::string>();
    auto it = agent_names.find(a);
    team.push_back(a + " (" + (it == agent_names.end() ? a : it->second) + ")");
  }
  std::string an = jstr(profile, "assistant_name");
  base["name"] = an.empty() ? base["name"] : json(an);
  base["description"] = "Oficina de la empresa (" + jstr(profile, "name") + "); coordina a su equipo";
  base["system_prompt"] = replace_all(COORDINATOR_PROMPT, "{context}", ctx) + "\nTu equipo: " + join(team, "; ") + ".\n";
  base["temperature"] = 0.5;
  base["tools"] = COORDINATOR_TOOLS;
  base["can_delegate_to"] = members;
  base["icon"] = profile.value("icon", base.value("icon", json("◆")));
  base["enabled"] = true;
  return base;
}

// ======================================================================== Office
Office::Office(Config& config, fs::path agents_dir)
    : config_(config), dir_(agents_dir.empty() ? paths::agents_dir() : agents_dir) {}

std::string Office::active() { return config_.str("office.profile", ""); }

json Office::list() {
  json out = json::array();
  std::string act = active();
  for (json items_tmp_ = profiles(); auto& [pid, _] : items_tmp_.items()) out.push_back(describe(pid, act));
  return out;
}

void Office::backup(const fs::path& f) {
  if (!path_exists(f)) return;
  fs::path dst = dir_ / "respaldo";
  mkdirs(dst);
  std::error_code ec;
  fs::copy_file(f, dst / P(stem(f) + "-" + strftime_local("%Y%m%d-%H%M%S") + ".yaml"), fs::copy_options::overwrite_existing, ec);
}

void Office::write(const json& data) {
  fs::path f = dir_ / P(jstr(data, "id") + ".yaml");
  std::string txt = agents::yaml_dump_spec(agents::AgentSpec::from_json(data));   // mismo formato que AgentManager.save
  if (path_exists(f) && read_text(f) == txt) return;
  backup(f);
  write_file(f, txt);
}

void Office::set_enabled(const fs::path& f, bool enabled) {
  json d = read_yaml(f);
  if (truthy(d.value("enabled", json(true))) != enabled) {
    d["enabled"] = enabled;
    write_file(f, yaml::dump(d));
  }
}

json Office::apply(const std::string& pid, bool reset_agents) {
  json allp = profiles();
  if (!allp.contains(pid)) {
    std::vector<std::string> ids;
    for (auto& [k, _] : allp.items()) ids.push_back(k);
    throw std::invalid_argument("perfil desconocido: " + pid + ". Disponibles: " + join(ids, ", "));
  }
  json profile = allp[pid], cat = catalog();
  std::vector<std::string> members, missing;
  for (auto& a : profile.value("agents", json::array())) {
    members.push_back(a.get<std::string>());
    if (!cat.contains(members.back())) missing.push_back(members.back());
  }
  if (!missing.empty())
    throw std::invalid_argument("el perfil " + pid + " usa agentes que no estan en el catalogo: " + join(missing, ", "));
  std::set<std::string> team(members.begin(), members.end());
  for (auto& f : glob(agents::defaults_dir(), "*.yaml")) {   // instalacion nueva: se siembran para poder desactivarlos
    if (!path_exists(dir_ / f.filename())) {
      std::error_code ec;
      fs::copy_file(f, dir_ / f.filename(), ec);
    }
  }
  for (auto& f : glob(dir_, "*.yaml"))   // fuera de la oficina: se desactivan (se conservan con su memoria)
    if (!team.count(stem(f)) && stem(f) != "nexus") set_enabled(f, false);
  for (auto& aid : members) {
    fs::path f = dir_ / P(aid + ".yaml");
    if (path_exists(f) && !reset_agents) {
      set_enabled(f, true);   // el usuario lo pudo personalizar: solo se activa
    } else {
      json d = cat[aid];
      d.erase("grafo");
      d["enabled"] = true;
      write(d);
    }
  }
  std::map<std::string, std::string> names;
  for (auto& a : team) names[a] = jstr(cat[a], "name");
  write(coordinator(profile, names));
  std::string an = jstr(profile, "assistant_name");
  config_.set("user.assistant_name", an.empty() ? "Nexus-Office" : an, false);
  config_.set("user.default_agent", "auto", false);
  config_.set("office.profile", pid);
  L.info("Perfil de oficina aplicado: ", pid, " (", team.size(), " agentes)");
  return describe(pid, pid);
}

void Office::clear() {
  json cat = catalog();
  for (auto& f : glob(dir_, "*.yaml"))
    if (cat.contains(stem(f))) set_enabled(f, false);
  for (auto& f : glob(agents::defaults_dir(), "*.yaml")) {
    fs::path dst = dir_ / f.filename();
    if (stem(f) == "nexus" || !path_exists(dst)) {
      if (path_exists(dst) && read_text(dst) != read_text(f)) backup(dst);
      std::error_code ec;
      fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
    } else {
      set_enabled(dst, true);
    }
  }
  config_.set("user.assistant_name", "Nexus-Office", false);
  config_.set("office.profile", "");
}

json Office::seed_graphs(mem::KnowledgeGraph& graph, const std::vector<std::string>& agent_ids) {
  json cat = catalog(), out = json::object();
  for (auto& aid : agent_ids) {
    if (!cat.contains(aid) || graph.db.one("SELECT 1 FROM relations WHERE scope=? AND rel='domina' LIMIT 1", {aid}))
      continue;
    auto& g = graph.for_agent(aid);
    int n = 0;
    graph.db.transaction([&] {
      for (auto& [s, rel, o, category] : seeds(cat[aid]))
        if (g.relate(s, rel, o, "concepto", "concepto", 1.5, category, category)) {
          g.relate(g.self_name, "domina", s, "agente", "concepto", 0.5, category);
          ++n;
        }
    });
    out[aid] = n;
  }
  if (!out.empty()) {
    std::vector<std::string> parts;
    for (auto& [a, n] : out.items()) parts.push_back(a + " (" + n.dump() + ")");
    L.info("Grafos sembrados: ", join(parts, ", "));
  }
  return out;
}

// ======================================================================== paquetes de conocimiento
json packs() {
  json out = json::object();
  fs::path kd = knowledge_dir();
  if (!is_dir(kd)) return out;
  std::vector<fs::path> dirs;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(kd, ec))
    if (path_exists(e.path() / "paquete.yaml")) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (auto& d : dirs) {
    json meta = read_yaml(d / "paquete.yaml");
    meta["dir"] = S(d);
    json g = path_exists(d / "grafo.yaml") ? read_yaml(d / "grafo.yaml").value("grafo", json()) : json();
    meta["grafo"] = g.is_array() ? g : json::array();
    out[jstr(meta, "id")] = meta;
  }
  return out;
}

std::vector<std::string> sections(const std::string& text) {
  // re.split(r"(?m)^(?=## )", text): se corta antes de cada linea que empieza por "## "
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    bool line_start = i == 0 || text[i - 1] == '\n';
    if (line_start && i > start && text.compare(i, 3, "## ") == 0) {
      parts.push_back(text.substr(start, i - start));
      start = i;
    }
  }
  parts.push_back(text.substr(start));
  std::string head = !parts.empty() && !starts_with(parts[0], "## ") ? parts[0] : "";
  std::vector<std::string> cand;
  if (!trim(head).empty()) cand.push_back(head);
  for (auto& p : parts)
    if (starts_with(p, "## ")) cand.push_back(p);
  std::vector<std::string> out;
  for (auto& p : cand)
    if (!trim(p).empty()) out.push_back(trim(p));
  return out;
}

int install_pack(Services& s, const json& pack, const std::string& aid) {
  std::string pid = jstr(pack, "id");
  int version = (int)jnum(pack.value("version", json(1)), 1);
  std::vector<json> mine;
  for (auto& d : s.rag->list(aid)) {
    json meta = d.value("meta", json::object());
    if (jstr(d, "agent") == aid && meta.is_object() && jstr(meta, "paquete") == pid) mine.push_back(d);
  }
  for (auto& d : mine)
    if ((int)jnum(d["meta"].value("version", json(0))) == version) return 0;
  for (auto& d : mine) s.rag->remove(d["id"].get<long long>());   // version anterior del paquete
  int n = 0;
  static const Rx H1(R"(^# (.+)$)", Rx::M);
  for (auto& f : sorted_glob(P(jstr(pack, "dir")), "*.md")) {
    std::string text = read_text(f);
    auto m = H1.search(text);
    s.rag->ingest_pages(m ? trim(m->str(1)) : stem(f), sections(text), pid + "/" + S(f.filename()), "md", "",
                        {{"paquete", pid}, {"version", version}}, {}, aid);
    ++n;
  }
  auto& g = s.graph->for_agent(aid);
  s.graph->db.transaction([&] {
    for (auto& [sub, rel, o, category] : seeds({{"grafo", pack.value("grafo", json::array())}}))
      g.relate(sub, rel, o, "concepto", "concepto", 1.2, category, category);
    // ancla propia («conoce», no «domina»: «domina» marca la siembra del catalogo en Office::seed_graphs)
    std::string nombre = jstr(pack, "nombre");
    g.relate(g.self_name, "conoce", nombre.empty() ? pid : nombre, "agente", "concepto", 0.5, pid);
  });
  auto spec = s.agents->get(aid);
  std::vector<std::string> missing;
  if (spec && !spec->has_tool("*"))
    for (auto& t : pack.value("herramientas", json::array())) {
      std::string tn = t.get<std::string>();
      if (std::find(spec->tools.begin(), spec->tools.end(), tn) == spec->tools.end()) missing.push_back(tn);
    }
  if (!missing.empty()) {   // agentes instalados antes de este paquete: se le suman sus herramientas
    json d = spec->to_json();
    for (auto& t : missing) d["tools"].push_back(t);
    s.agents->save(d);
  }
  L.info("Paquete de conocimiento ", pid, " v", version, " instalado en ", aid, " (", n, " documentos)");
  return n;
}

json install_packs(Services& s, const std::vector<std::string>* agent_ids) {
  json out = json::object();
  // office.knowledge_packs: false los desactiva (NEXUS_NO_KNOWLEDGE_PACKS=1 en las pruebas que cuentan documentos)
  if (!env("NEXUS_NO_KNOWLEDGE_PACKS").empty() || !truthy(s.config.get("office.knowledge_packs", true))) return out;
  std::vector<std::string> v = agent_ids ? *agent_ids : s.agents->ids();
  std::set<std::string> ids(v.begin(), v.end());
  for (json items_tmp_ = packs(); auto& [pid, pack] : items_tmp_.items())
    for (auto& a : pack.value("agentes", json::array())) {
      std::string aid = a.get<std::string>();
      if (!ids.count(aid)) continue;
      int n = install_pack(s, pack, aid);
      if (n) out[pid + ":" + aid] = n;
    }
  return out;
}

json ensure(Services& s) {
  Office office(s.config);
  json out = office.active().empty() ? json::object() : office.seed_graphs(*s.graph, s.agents->ids());
  try {
    install_packs(s);
  } catch (const std::exception& e) {
    L.warning("No se pudieron instalar los paquetes de conocimiento: ", e.what());
  }
  return out;
}

}  // namespace nx::office
