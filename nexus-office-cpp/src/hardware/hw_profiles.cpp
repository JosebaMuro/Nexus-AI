// Perfiles de hardware (hardware/hw_profiles.py).
#include "hardware/hw_profiles.h"

#include <cmath>

#include "core/services.h"
#include "core/system.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "models/models.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::hw::profiles {

namespace {

const auto L = log::get("perfiles");

bool es_entero(const json& v) { return v.is_number_integer() || v.is_number_unsigned(); }

int a_entero(const json& v, bool& ok) {
  ok = true;
  if (es_entero(v)) return v.get<int>();
  if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
  if (v.is_number_float()) return int(v.get<double>());
  if (v.is_string()) {
    std::string s = trim(v.get<std::string>());
    try {
      size_t n = 0;
      int r = std::stoi(s, &n);
      if (n == s.size()) return r;
    } catch (...) {
    }
  }
  ok = false;
  return 0;
}

std::string slug(const std::string& s) {
  std::string out;
  bool sep = false;
  for (unsigned char ch : lower(s)) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
      out += char(ch);
      sep = false;
    } else if (!sep) {
      out += '-';
      sep = true;
    }
  }
  size_t a = out.find_first_not_of('-'), b = out.find_last_not_of('-');
  out = a == std::string::npos ? "" : out.substr(a, b - a + 1).substr(0, 40);
  return out.empty() ? "perfil" : out;
}

json propios_crudos(Config& cfg) {
  json p = cfg.get("hardware.perfiles", json::array());
  json out = json::array();
  if (p.is_array())
    for (auto& x : p)
      if (x.is_object()) out.push_back(x);
  return out;
}

std::string activo(Config& cfg) {
  std::string a = cfg.str("hardware.perfil", "auto");
  return a.empty() ? "auto" : a;
}

}  // namespace

const json CAMPOS = {{"nombre", ""}, {"descripcion", ""}, {"motor", "auto"}, {"acelerador", "auto"}, {"gpu", ""},
                     {"modelo", ""}, {"capas_gpu", "auto"}, {"kv", "auto"}, {"mlock", false}, {"contexto", 0},
                     {"hilos", 0}, {"lote", 0}};

const json DE_SERIE = jparse(R"([
  {"id": "auto", "nombre": "Automático", "descripcion": "Lo que Nexus elige para tu equipo"},
  {"id": "rendimiento", "nombre": "Rendimiento", "descripcion": "GPU, modelo entero en la VRAM y contexto máximo",
   "acelerador": "gpu", "capas_gpu": "todas", "kv": "vram", "contexto": -1, "lote": 1024},
  {"id": "equilibrado", "nombre": "Equilibrado", "descripcion": "GPU si la hay, contexto moderado", "contexto": 8192},
  {"id": "grande", "nombre": "Modelo grande", "descripcion": "Parte del modelo en la VRAM y el resto y el contexto en la RAM",
   "acelerador": "gpu", "capas_gpu": "auto", "kv": "ram", "mlock": true},
  {"id": "ahorro", "nombre": "Ahorro", "descripcion": "CPU, poco contexto y la mitad de los hilos: menos consumo",
   "acelerador": "cpu", "contexto": 2048, "hilos": -50},
  {"id": "cpu", "nombre": "Solo CPU", "descripcion": "Sin GPU ni NPU", "acelerador": "cpu"}
])");

json norm(const json& p) {
  json out = CAMPOS;
  for (auto& [k, v] : p.items())
    if (CAMPOS.contains(k) || k == "id" || k == "de_serie") out[k] = v;
  // el id va delante, como en Python ({**CAMPOS, ...} y despues out["id"])
  std::string base = p.contains("id") && truthy(p["id"]) ? (p["id"].is_string() ? p["id"].get<std::string>() : p["id"].dump())
                     : truthy(out["nombre"]) ? (out["nombre"].is_string() ? out["nombre"].get<std::string>() : out["nombre"].dump())
                                            : "perfil";
  out["id"] = slug(base);
  auto una_de = [&](const char* k, std::initializer_list<const char*> ok, const char* def) {
    std::string v = out[k].is_string() ? out[k].get<std::string>() : "";
    for (auto* o : ok)
      if (v == o) return;
    out[k] = def;
  };
  una_de("motor", {"auto", "llamaserver", "llamacpp"}, "auto");
  una_de("acelerador", {"auto", "cpu", "gpu", "npu"}, "auto");
  una_de("kv", {"auto", "vram", "ram"}, "auto");
  json& c = out["capas_gpu"];
  bool palabra = c.is_string() && (c == "auto" || c == "todas" || c == "ninguna");
  if (!(palabra || (es_entero(c) && c.get<long long>() >= 0))) {
    bool ok;
    int n = a_entero(c, ok);
    c = ok ? json(std::max(0, n)) : json("auto");
  }
  for (const char* k : {"contexto", "hilos", "lote"}) {
    bool ok;
    int n = truthy(out[k]) ? a_entero(out[k], ok) : (ok = true, 0);
    out[k] = ok ? n : 0;
  }
  out["mlock"] = truthy(out["mlock"]);
  return out;
}

json lista(Config& cfg) {
  json propios = json::array();
  std::set<std::string> ids;
  for (auto& p : propios_crudos(cfg)) {
    json n = norm(p);
    ids.insert(jstr(n, "id"));
    propios.push_back(n);
  }
  json out = json::array();
  for (auto& p : DE_SERIE)
    if (!ids.count(jstr(p, "id"))) {
      json n = norm(p);
      n["de_serie"] = true;
      out.push_back(n);
    }
  for (auto& p : propios) out.push_back(p);
  return out;
}

json get(Config& cfg, const std::string& pid) {
  for (auto& p : lista(cfg))
    if (jstr(p, "id") == pid) return p;
  return nullptr;
}

json guardar(Config& cfg, const json& p_in) {
  json p = norm(p_in);
  json propios = json::array();
  for (auto& x : propios_crudos(cfg))
    if (jstr(x, "id") != jstr(p, "id")) propios.push_back(x);
  json guardado = p;
  guardado.erase("de_serie");
  propios.push_back(guardado);
  cfg.set("hardware.perfiles", propios);
  return p;
}

bool borrar(Config& cfg, const std::string& pid) {
  json propios = propios_crudos(cfg), nuevos = json::array();
  for (auto& x : propios)
    if (jstr(x, "id") != pid) nuevos.push_back(x);
  if (nuevos.size() == propios.size()) return false;
  cfg.set("hardware.perfiles", nuevos);
  if (cfg.str("hardware.perfil", "auto") == pid) cfg.set("hardware.perfil", "auto");
  return true;
}

json& superponer(json& profile, const json& p) {
  json& llm = profile["llm"];
  bool gpu_on = jnum(llm.value("gpu_layers", json(0))) != 0;
  int contexto = p.value("contexto", 0);
  if (contexto == -1) {
    llm["n_ctx"] = llm.value("max_ctx", llm["n_ctx"]);
  } else if (contexto > 0) {
    long long tope = (long long)jnum(llm.value("max_ctx", json(contexto))) * 2;
    llm["n_ctx"] = std::max(512LL, std::min((long long)contexto, tope));
  }
  if (gpu_on) {
    const json& c = p["capas_gpu"];
    if (c == "todas") llm["gpu_layers"] = -1;
    else if (c == "ninguna") llm["gpu_layers"] = 0;
    else if (es_entero(c) && c.get<long long>() > 0) llm["gpu_layers"] = c;
  }
  llm["kv_ram"] = p.value("kv", "auto") == "ram";
  llm["mlock"] = p.value("mlock", false);
  int hilos = p.value("hilos", 0);
  if (hilos > 0) {
    llm["n_threads"] = hilos;
  } else if (hilos < 0) {   // porcentaje de los automaticos (-50 = la mitad)
    llm["n_threads"] = std::max(1LL, std::llrint(jnum(llm["n_threads"]) * -hilos / 100.0));
  }
  if (p.value("lote", 0) > 0) llm["n_batch"] = p["lote"];
  if (!jstr(p, "gpu").empty() && gpu_on) llm["gpu_select"] = p["gpu"];
  profile["perfil_hw"] = {{"id", p["id"]}, {"nombre", p["nombre"]}};
  return profile;
}

void al_arrancar(Config& cfg) {
  json p = get(cfg, activo(cfg));
  std::string g = p.is_object() ? jstr(p, "gpu") : "";
  select_gpu(g.empty() ? cfg.str("hardware.gpu", "") : g);
}

void aplicar_al_perfil(Services& s) {
  json p = get(s.config, activo(s.config));
  if (p.is_object() && jstr(p, "id") != "auto") superponer(s.profile, p);
}

json activar(const std::string& pid, Config& cfg, Services* s) {
  json p = get(cfg, pid);
  if (!p.is_object()) throw std::invalid_argument("perfil de hardware desconocido: " + pid);
  cfg.set("hardware.perfil", p["id"]);
  cfg.set("hardware.gpu", p["gpu"]);
  select_gpu(jstr(p, "gpu"));
  json res = {{"perfil", p}, {"nota", ""}};
  if (!s) return res;   // se aplicara al arrancar
  json r = sys2::accel_apply(*s, jstr(p, "motor"), jstr(p, "acelerador"), std::nullopt);
  std::string nota = jstr(r, "note");
  superponer(s->profile, p);
  if (!jstr(p, "modelo").empty()) {
    try {
      s->models->activate(jstr(p, "modelo"));
    } catch (const std::exception& e) {
      std::string err = "modelo " + jstr(p, "modelo") + ": " + e.what();
      nota = nota.empty() ? err : nota + "; " + err;
    }
  }
  res["nota"] = nota;
  s->llm->rebuild();
  L.info("Perfil de hardware «", jstr(p, "nombre"), "» activo (", jstr(p, "motor"), ", ", jstr(p, "acelerador"), ")");
  json llm = json::object();
  for (const char* k : {"n_ctx", "gpu_layers", "kv_ram", "mlock", "n_threads", "n_batch", "gpu_select"})
    llm[k] = s->profile["llm"].value(k, json());
  res["llm"] = llm;
  return res;
}

json elegir_gpu(Services& s, const std::string& gpu) {
  Config& cfg = s.config;
  cfg.set("hardware.gpu", gpu);
  select_gpu(gpu);
  auto built = build_for(s.hw, cfg.str("hardware.accel", "auto"));
  json profile = built.first;
  json p = get(cfg, activo(cfg));
  if (p.is_object() && jstr(p, "id") != "auto") {
    json q = p;
    if (!gpu.empty()) q["gpu"] = gpu;
    superponer(profile, q);
  }
  s.profile = profile;
  s.llm->rebuild();
  return {{"gpu", gpu}, {"dispositivo", profile["device_name"]}, {"acelerador", profile["accelerator"]}};
}

json gpus(Services& s) {
  const HardwareInfo& hw = s.hw;
  json devs = hw.runtimes.is_object() ? hw.runtimes.value("llama_devices", json::array()) : json::array();
  if (!devs.is_array()) devs = json::array();
  std::string sel = selected_gpu();
  json out = json::array();
  for (size_t i = 0; i < hw.gpus.size(); ++i) {
    const GPU& g = hw.gpus[i];
    const json* dev = nullptr;
    for (auto& d : devs)
      if (!d.value("software", false) && gpu_matches(g, jstr(d, "name"))) {
        dev = &d;
        break;
      }
    double vram = g.vram_gb ? g.vram_gb : dev ? std::round(jnum((*dev)["mem_mb"]) / 1024 * 10) / 10 : 0;
    bool elegida = !sel.empty() && (gpu_matches(g, sel) || (dev && lower(jstr(*dev, "id")) == lower(sel)));
    out.push_back({{"indice", i}, {"nombre", g.name}, {"fabricante", g.vendor}, {"vram_gb", vram},
                   {"integrada", g.integrated}, {"llama", dev ? jstr(*dev, "id") : ""},
                   {"variante", dev ? jstr(*dev, "variant") : ""}, {"elegida", elegida}});
  }
  for (auto& d : devs)
    if (d.value("software", false))
      out.push_back({{"indice", -1}, {"nombre", d["name"]}, {"fabricante", "software"}, {"vram_gb", 0},
                     {"integrada", false}, {"llama", d["id"]}, {"software", true}, {"elegida", false}});
  return out;
}

}  // namespace nx::hw::profiles
