// Modelos (nexus/models): catalogo local, 5 repositorios, descargas reanudables, importacion y activacion por tipo.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "util/fs.h"
#include "util/http.h"
#include "util/json.h"

namespace nx::models {

// ---------------------------------------------------------------- GGUF
json gguf_metadata(const fs::path& p, size_t max_keys = 400);   // lanza si no es GGUF
json gguf_summarize(const fs::path& p);
std::vector<std::string> gguf_tensor_names(const fs::path& p, size_t max = 64);   // vacio si no se puede leer

// ---------------------------------------------------------------- modelos de imagen GGUF (models/difusion.cpp)
// Que es un GGUF de imagen: arquitectura (sd1, sdxl, sd3, flux, chroma, z_image, qwen_image, flux2...), si es solo
// el modelo de difusion (los GGUF de Flux, Z-Image, Qwen-Image o SD3 que no traen VAE ni codificadores de texto) y
// que piezas le faltan entonces. null si no parece un modelo de imagen.
json diffusion_info(const json& meta, const std::vector<std::string>& tensors, const std::string& name);
json diffusion_info(const fs::path& p);
// piezas que necesita (vae, clip_l, clip_g, t5xxl, llm) buscadas junto al modelo y en la carpeta de modelos:
// {pieza: ruta}; las que no aparecen se quedan fuera
json find_components(const fs::path& model, const json& dinfo, const json& configured = json::object());
// LLM que hace de codificador de texto para esa arquitectura ("Qwen3-4B" en Z-Image; "" si no usa LLM). Los LLM que
// no encajan (otra familia u otro tamano) no se eligen y quedan en piezas["_descartados"].
std::string llm_encoder_for(const std::string& arch, const std::string& model_name);

// ---------------------------------------------------------------- catalogo recomendado
const json& recommended_list();   // RECOMMENDED (incluye SECTOR)
std::string fit(const json& rec, const json& profile);
json recommend(const json& profile, const json& sector = json::object());
const json& legacy_llm();

// ---------------------------------------------------------------- fuentes
class Source {
 public:
  explicit Source(Config& c) : config(c) {}
  virtual ~Source() = default;
  virtual std::string id() const = 0;
  virtual std::string name() const = 0;
  virtual http::Headers headers() const { return {}; }
  virtual json search(const std::string& query, const std::string& kind) = 0;
  virtual json files(const std::string& repo_id) = 0;
  virtual std::string download_url(const std::string& repo_id, const std::string& file) = 0;
  Config& config;
};
std::map<std::string, std::shared_ptr<Source>> all_sources(Config& c);
// Ollama: capas del registro {"model": [url, size], "projector": [url, size]}
json ollama_layers(const std::string& ref);

// ---------------------------------------------------------------- utilidades
json annotate(json files, const json& profile, const std::string& source = "", const std::string& repo = "");
json best_mmproj(const json& listing);
json classify(const fs::path& p, const std::string& hint = "");
// recomendado con formato de un motor (.litertlm, ONNX): el fichero o la carpeta para el NPU/GPU de este equipo
json resolve_por_hardware(const json& rec, const json& profile, const json& listing);
std::string slug(const std::string& text);
extern const std::map<std::string, std::string> ACTIVE_KEYS;

class Catalog {
 public:
  Catalog();
  void save();
  json get(const std::string& mid);   // null si no existe o falta el fichero
  json list();
  json add(const json& entry);
  json remove(const std::string& mid);
  json items();

 private:
  fs::path file_;
  std::recursive_mutex mu_;
  json items_;
};

class ModelManager {
 public:
  ModelManager(Config& config, std::function<json()> profile_getter);
  json search(const std::string& source, const std::string& query, const std::string& kind = "");
  json files(const std::string& source, const std::string& repo_id);
  json recommended();
  std::string download(const std::string& source, const std::string& repo_id, const std::string& file,
                       const std::string& kind = "", bool with_extras = true);
  std::string download_repo(const std::string& source, const std::string& repo_id, const std::string& kind = "");
  std::string download_recommended(const std::string& rec_id);
  json resolve_recommended(const json& rec);
  void cancel(const std::string& jid);
  json register_model(const fs::path& path, const std::string& source = "local", const std::string& repo = "",
                      const std::string& kind = "", const std::string& mmproj = "", const std::string& mid = "",
                      const std::string& name = "", const json& extra = json::object());
  json import_local(const std::string& path, bool copy = false, const std::string& kind = "");
  void remove(const std::string& mid, bool remove_files = false);
  json activate(const std::string& mid);
  json active();
  json jobs();
  json job(const std::string& jid);

  Catalog catalog;
  std::map<std::string, std::shared_ptr<Source>> sources;
  // "ollama pull" con un Ollama local: recibe el nombre y una funcion por evento de progreso
  std::function<void(const std::string&, const std::function<bool(const json&)>&)> ollama_pull;
  std::vector<std::function<void(const json&)>> on_activate;

 private:
  struct File {
    std::string url;
    fs::path dest;
    http::Headers headers;
    uint64_t size = 0;
  };
  Config& config_;
  std::function<json()> profile_;
  std::mutex mu_;
  json jobs_ = json::object();
  std::set<std::string> cancel_;
  std::map<std::string, json> files_cache_;
  fs::path dest(const std::string& source, const std::string& repo_id, const std::string& file);
  std::string new_job(const std::string& title);
  void job_update(const std::string& jid, const json& patch);
  void run(const std::string& jid, std::vector<File> files, const std::function<json()>& on_done);
  void pull_ollama(const std::string& jid, const std::string& name);
  void auto_activate(const json& entry);
  void notify(const json& entry);
  void configurar_motor(const json& entry);
  json listo(const json& entry);
  void autoregister();
};

}  // namespace nx::models
