#include "core/config.h"

#include <memory>

#include "util/crypto.h"
#include "util/fs.h"
#include "util/text.h"
#include "util/yaml.h"

namespace nx {

const json& config_defaults() {
  static const json D = json::parse(R"JSON({
  "user": {
    "assistant_name": "Nexus-Office", "sexo": "", "language": "es", "response_style": "equilibrado", "voice_enabled": true,
    "voice_name": "", "speech_rate": 1.0, "auto_speak": false, "default_agent": "auto", "memory_enabled": true,
    "accent_color": "#00f0ff", "avatar_enabled": true, "avatar_style": "geisha", "avatar_model": "", "theme": {}
  },
  "llm": {
    "backend_order": ["llamaserver", "llamacpp", "ollama", "openai", "transformers", "builtin"],
    "engine": "auto", "default_model": "", "ollama_url": "http://127.0.0.1:11434",
    "openai_base_url": "http://127.0.0.1:1234/v1", "openai_api_key": "", "openai_model": "",
    "temperature": 0.7, "max_tokens": 1024, "reasoning": "auto", "reasoning_budget": 2048,
    "reasoning_budget_tasks": 768, "max_tool_steps": 5, "request_timeout": 180
  },
  "memory": {
    "embedder": "auto", "embed_model": "", "embed_threads": 0, "embed_dims": 0,
    "ollama_embed_model": "nomic-embed-text",
    "organizer": {"min_affinity": 0.35, "margin": 0.03, "min_keyword_hits": 1},
    "zip_reason_docs": 12, "top_k": 6, "llm_graph_extraction": false, "decay_days": 30, "consolidate_every": 12
  },
  "tools": {
    "policy": {"low": "auto", "medium": "ask", "high": "admin"}, "timeout_s": 30,
    "allow_agent_proposals": true, "sandbox_community_tools": true
  },
  "hardware": {"profile_override": "", "accel": "auto", "llama_variant": "", "adaptive_tuning": true,
               "monitor_interval_s": 2.0, "perfil": "auto", "perfiles": [], "gpu": ""},
  "models": {"huggingface_token": "", "civitai_token": "", "kaggle_username": "", "kaggle_key": "",
             "modelscope_token": ""},
  "media": {"tts_engine": "auto", "stt_engine": "auto", "image_model": "", "music_engine": "auto",
            "vision_model": "", "auto_enhance_prompts": true},
  "coop": {"enabled": false, "node_name": "", "network_id": "", "passphrase": "", "listen": false,
           "listen_host": "0.0.0.0", "listen_port": 8766, "public_urls": [], "peers": [], "max_hops": 6,
           "allow_remote_inference": true, "allow_remote_agents": false, "share_memory": false, "share_tools": true},
  "office": {"profile": ""},
  "lounge": {"auto_meet": true, "meet_every_tasks": 8},
  "startup": {"ask_accel": true},
  "server": {"host": "127.0.0.1", "port": 8775, "open_browser": true, "lan_key": ""},
  "remote": {"enabled": false, "port": 8443, "host": "0.0.0.0"},
  "vpn": {"enabled": false, "port": 51820, "subnet": "10.78.0.0/24", "endpoints": []},
  "performance": {"workers": 0, "crawl_connections": 0},
  "rag": {"crawl_batch_pages": 0, "allow_private_urls": false, "browser": "auto", "browser_path": ""},
  "bolsa": {"datos": "reales", "refresco_s": 20,
            "agente": {"activo": true, "intervalo_s": 60, "capital": 10000.0, "riesgo_pct": 1.0, "max_posiciones": 8,
                       "max_peso_pct": 20.0, "reserva_pct": 10.0, "universo": []}},
  "avatares": {"sketchfab_token": ""},
  "nube": {"proveedor": "", "modelo": "", "base_url": "", "esfuerzo": "medium",
           "clave_gemini": "", "clave_openai": "", "clave_perplexity": "", "clave_claude": "", "clave_mistral": "",
           "clave_deepseek": "", "clave_groq": "", "clave_openrouter": "", "clave_compatible": ""},
  "admin": {"pin_hash": "", "pin_salt": ""}
})JSON");
  return D;
}

const std::set<std::string>& user_keys() {
  static const std::set<std::string> K = [] {
    std::set<std::string> s;
    for (auto it = config_defaults()["user"].begin(); it != config_defaults()["user"].end(); ++it)
      s.insert("user." + it.key());
    s.insert("avatares.sketchfab_token");   // clave de API de Sketchfab (para descargar; buscar no la necesita)
    return s;
  }();
  return K;
}

const std::set<std::string>& secret_keys() {
  static const std::set<std::string> K = {"admin.pin_hash",        "admin.pin_salt",       "llm.openai_api_key",
                                          "models.huggingface_token", "models.civitai_token", "models.kaggle_key",
                                          "models.modelscope_token", "coop.passphrase",      "server.lan_key",
                                          "avatares.sketchfab_token",
                                          "nube.clave_gemini", "nube.clave_openai", "nube.clave_perplexity",
                                          "nube.clave_claude", "nube.clave_mistral", "nube.clave_deepseek",
                                          "nube.clave_groq", "nube.clave_openrouter", "nube.clave_compatible"};
  return K;
}

json deep_merge(const json& base, const json& extra) {
  json out = base;
  if (!extra.is_object()) return out;
  for (auto it = extra.begin(); it != extra.end(); ++it) {
    if (it.value().is_object() && out.contains(it.key()) && out[it.key()].is_object())
      out[it.key()] = deep_merge(out[it.key()], it.value());
    else
      out[it.key()] = it.value();
  }
  return out;
}

std::string hash_pin(const std::string& pin, const std::string& salt_hex) {
  return to_hex(pbkdf2_sha256(pin, from_hex(salt_hex), 200000));
}

Config::Config() { load(); }

void Config::load() {
  fs::path f = paths::config_file();
  json raw = json::object();
  bool existed = is_file(f);
  if (existed) {
    raw = yaml::load(read_text(f));   // un YAML roto se senala (como yaml.safe_load en Python)
    if (!raw.is_object()) raw = json::object();
  }
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    data_ = deep_merge(config_defaults(), raw);
  }
  if (!existed) save();
}

void Config::save() {
  std::string txt;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    txt = yaml::dump(data_);
  }
  write_file(paths::config_file(), txt);
}

json Config::get(const std::string& dotted, const json& def) const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  const json* node = &data_;
  for (auto& part : split(dotted, ".")) {
    if (!node->is_object()) return def;
    auto it = node->find(part);
    if (it == node->end()) return def;
    node = &*it;
  }
  return *node;
}

std::string Config::str(const std::string& dotted, const std::string& def) const {
  json v = get(dotted);
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return def;
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_float()) return pyfloat(v.get<double>());
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  return def;
}

double Config::num(const std::string& dotted, double def) const { return jnum(get(dotted), def); }

bool Config::flag(const std::string& dotted, bool def) const {
  json v = get(dotted);
  return v.is_null() ? def : truthy(v);
}

void Config::set(const std::string& dotted, const json& value, bool do_save) {
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    json* node = &data_;
    auto parts = split(dotted, ".");
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      if (!node->is_object()) *node = json::object();
      if (!node->contains(parts[i])) (*node)[parts[i]] = json::object();
      node = &(*node)[parts[i]];
    }
    if (!node->is_object()) *node = json::object();
    (*node)[parts.back()] = value;
  }
  if (do_save) save();
}

json Config::update(const json& changes, bool admin) {
  json applied = json::object();
  for (auto it = changes.begin(); it != changes.end(); ++it) {
    const std::string& key = it.key();
    if (starts_with(key, "admin.")) continue;
    if (!admin && !user_keys().count(key)) throw PermissionError("'" + key + "' requiere modo administrador");
    if (secret_keys().count(key) && it.value() == "********") continue;   // la vista enmascarada: se conserva
    set(key, it.value(), false);
    applied[key] = it.value();
  }
  save();
  return applied;
}

json Config::view(bool admin) const {
  json d;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    d = data_;
  }
  if (!admin) return json{{"user", d["user"]}};
  for (auto& key : secret_keys()) {
    auto parts = split(key, ".");
    if (d.contains(parts[0]) && d[parts[0]].is_object() && truthy(d[parts[0]].value(parts[1], json())))
      d[parts[0]][parts[1]] = "********";
  }
  d.erase("admin");
  return d;
}

json Config::data() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return data_;
}

bool Config::has_pin() const { return truthy(get("admin.pin_hash")); }

void Config::set_pin(const std::string& pin) {
  if (ulen(pin) < 4) throw std::invalid_argument("El PIN debe tener al menos 4 caracteres");
  std::string salt = token_hex(16);
  set("admin.pin_salt", salt, false);
  set("admin.pin_hash", hash_pin(pin, salt));
}

bool Config::check_pin(const std::string& pin) const {
  if (!has_pin()) return false;
  return consteq(hash_pin(pin, str("admin.pin_salt")), str("admin.pin_hash"));
}

static std::unique_ptr<Config> g_config;
static std::mutex g_mu;

Config& get_config() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_config) g_config = std::make_unique<Config>();
  return *g_config;
}

void reset_config() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_config.reset();
}

}  // namespace nx
