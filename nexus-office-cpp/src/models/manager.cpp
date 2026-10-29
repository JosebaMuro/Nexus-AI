// Gestor de modelos (models/manager.py).
#include <algorithm>
#include <set>
#include <thread>

#include "models/models.h"
#include "office/office.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::models {

static const auto L = log::get("models");

static const Rx& EMBED_HINT() {
  static const Rx r(R"((^|[\W_])(embed|embedding|bge|e5|gte|minilm|nomic-embed|arctic-embed)([\W_]|$))", Rx::I);
  return r;
}
static const Rx& IMAGE_HINT() {
  static const Rx r(R"((^|[\W_])(sd|sdxl|sdxs|sd3|flux|diffusion|turbo|dreamshaper|realistic|anime|z-image|lcm))", Rx::I);
  return r;
}
static const Rx& SPLIT_RE() {
  static const Rx r(R"(-\d{5}-of-\d{5}\.gguf$)", Rx::I);
  return r;
}
static const Rx& QUANT_RE() {
  static const Rx r(R"((IQ\d_[A-Z]+|Q\d_K_[SML]|Q\d_K|Q\d_\d|Q\d|F16|BF16|F32|FP16|FP8|INT8|INT4|AWQ|GPTQ|EXL2))", Rx::I);
  return r;
}
static const Rx& REPO_SKIP() {
  static const Rx r(R"((^|/)(\.gitattributes|README\.md|LICENSE.*|.*\.(png|jpg|jpeg|gif|webp|mp4|md|txt))$)", Rx::I);
  return r;
}
static const std::vector<std::string> QUANT_RANK = {"Q4_K_M", "Q4_K_S", "Q5_K_M", "IQ4_XS", "Q4_0", "Q5_K_S", "Q6_K",
                                                    "Q8_0",   "Q3_K_M", "Q3_K_L", "Q2_K",   "F16",  "BF16"};
static const std::map<std::string, std::string> EXT_FORMAT = {
    {".gguf", "gguf"}, {".safetensors", "safetensors"}, {".onnx", "onnx"}, {".ckpt", "ckpt"},
    {".bin", "bin"},   {".pt", "pt"},                   {".pth", "pt"},    {".xml", "openvino"},
    {".litertlm", "litertlm"}};

const std::map<std::string, std::string> ACTIVE_KEYS = {
    {"llm", "llm.default_model"}, {"image", "media.image_model"}, {"tts", "media.tts_model"}, {"embed", "memory.embed_model"}};

json best_mmproj(const json& listing) {
  json best;
  int best_rank = 99;
  double best_size = 0;
  for (auto& f : listing) {
    std::string p = lower(jstr(f, "path"));
    if (!contains(p, "mmproj") || !ends_with(p, ".gguf")) continue;
    int rank = contains(p, "q8") ? 0 : contains(p, "f16") ? 1 : 2;
    double size = jnum(f.value("size", json(0)));
    if (best.is_null() || rank < best_rank || (rank == best_rank && size < best_size)) {
      best = f;
      best_rank = rank;
      best_size = size;
    }
  }
  return best;
}

json annotate(json files, const json& profile, const std::string& source, const std::string& repo) {
  double mem = std::max(jnum(profile["llm"]["gpu_layers"]) != 0 ? jnum(profile.value("vram_gb", json(0))) * 0.9 : 0.0,
                        jnum(profile["ram_gb"]) * 0.6);
  std::string repo_l = lower(repo);
  bool has_mm = false, is_repo_model = false;
  for (auto& f : files) {
    std::string p = jstr(f, "path");
    if (contains(lower(p), "mmproj")) has_mm = true;
    if (ends_with(p, "config.json") || ends_with(p, "model_index.json")) is_repo_model = true;
  }
  static const Rx params(R"((\d+(?:\.\d+)?)\s*[bB](?![a-zA-Z]))");
  for (auto& f : files) {
    std::string p = jstr(f, "path"), low = lower(p);
    std::string ext = contains(low, ".") ? low.substr(low.rfind('.') + 1) : "";
    double size_gb = jnum(f.value("size", json(0))) / 1073741824.0;
    std::string fname = S(P(p).filename());
    auto q = QUANT_RE().search(fname);
    f["quant"] = q ? upper(q->str(1)) : "";
    std::map<std::string, std::string> FMT = {{"gguf", "gguf"}, {"safetensors", "safetensors"}, {"onnx", "onnx"},
                                              {"bin", "bin"},   {"ckpt", "ckpt"},               {"pt", "pt"},
                                              {"pth", "pt"},    {"xml", "openvino"},  {"litertlm", "litertlm"}};
    f["format"] = FMT.count(ext) ? FMT[ext] : ext;
    bool image_like = IMAGE_HINT().test(repo_l + " " + low);
    auto set = [&](const char* kind, std::vector<std::string> eng) {
      f["kind"] = kind;
      f["engines"] = eng;
    };
    bool has_json_sidecar = false;
    for (auto& x : files)
      if (jstr(x, "path") == p + ".json") has_json_sidecar = true;
    if (contains(low, "mmproj")) set("proyector", {"llama-server (vision)"});
    else if (ext == "litertlm") set("llm", {"LiteRT-LM (CPU, GPU y NPU)"});
    else if (ends_with(low, "genai_config.json")) set("llm", {"ONNX Runtime GenAI (DirectML, QNN, OpenVINO...)"});
    else if (ext == "gguf" && EMBED_HINT().test(repo_l + " " + low) && !has_mm)
      set("embed", {"llama-server (--embedding)", "llama-cpp-python"});
    else if (ext == "gguf") {
      if (image_like && !has_mm) set("image", {"sd.cpp"});
      else set("llm", {"llama-server", "llama-cpp-python", "Ollama (importar)"});
    } else if ((ext == "safetensors" || ext == "ckpt") && (image_like || source == "civitai"))
      set("image", {"sd.cpp", "diffusers"});
    else if (ext == "onnx" && (contains(repo_l, "piper") || (ends_with(low, ".onnx") && has_json_sidecar)))
      set("tts", {"Piper"});
    else if ((ext == "safetensors" || ext == "bin" || ext == "pt") && is_repo_model)
      set("llm", {"Transformers (repositorio completo)"});
    else if (source == "ollama") set("llm", {"Ollama", "llama-server (GGUF del registro)"});
    else set("otro", {});
    f["split"] = SPLIT_RE().test(low);
    std::string repo_file = repo + " " + fname;
    auto pm = params.search(repo_file);
    if (pm && f["kind"] == "llm" && std::atof(pm->str(1).c_str()) > jnum(profile["llm"]["max_model_b"])) f["slow"] = true;
    f["fits"] = size_gb ? json(size_gb < mem) : json(nullptr);
    f["fit"] = !size_gb ? "?" : size_gb < mem * 0.6 ? "ideal" : size_gb < mem ? "ok" : "pesado";
  }
  for (auto& qn : QUANT_RANK) {
    bool done = false;
    for (auto& f : files)
      if (f["format"] == "gguf" && f["kind"] == "llm" && f["quant"] == qn && (f["fit"] == "ideal" || f["fit"] == "ok")) {
        f["recommended"] = true;
        done = true;
        break;
      }
    if (done) break;
  }
  return files;
}

std::string slug(const std::string& text) {
  return uleft(trim(Rx("[^a-z0-9]+").sub(lower(text), "-"), "-"), 60);
}

json classify(const fs::path& p, const std::string& hint) {
  json info = {{"format", "unknown"}, {"kind", "other"}};
  if (is_dir(p)) {
    // ONNX Runtime GenAI antes que Transformers: sus carpetas tambien traen config.json
    if (is_file(p / "genai_config.json")) info = {{"format", "onnx-genai"}, {"kind", "llm"}};
    else if (is_file(p / "config.json")) info = {{"format", "transformers"}, {"kind", "llm"}};
    else if (is_file(p / "model_index.json")) info = {{"format", "diffusers"}, {"kind", "image"}};
    else if (!glob(p, "*.xml").empty()) info = {{"format", "openvino"}, {"kind", "llm"}};
    return info;
  }
  std::string ext = suffix_lower(p);
  info["format"] = EXT_FORMAT.count(ext) ? EXT_FORMAT.at(ext) : "unknown";
  std::string text = hint + " " + S(p.filename());
  if (info["format"] == "gguf") {
    json meta = gguf_summarize(p);
    info["meta"] = meta;
    json dif = truthy(meta.value("is_projector", json(false))) ? json() : diffusion_info(p);
    if (truthy(meta.value("is_projector", json(false)))) info["kind"] = "projector";
    else if (dif.is_object()) {
      // GGUF de imagen (stable-diffusion.cpp): Flux, Z-Image, SD3... a menudo solo el modelo de difusion
      info["kind"] = "image";
      info["difusion"] = dif;
    } else if (truthy(meta.value("is_embedding", json(false))) || EMBED_HINT().test(text)) info["kind"] = "embed";
    else if (truthy(meta.value("architecture", json("")))) info["kind"] = "llm";
    else if (IMAGE_HINT().test(text)) info["kind"] = "image";
  } else if ((info["format"] == "safetensors" || info["format"] == "ckpt") && IMAGE_HINT().test(text)) {
    info["kind"] = "image";
  } else if ((info["format"] == "onnx" && contains(lower(text), "piper")) ||
             (ends_with(S(p.filename()), ".onnx") && is_file(P(S(p) + ".json")))) {
    info["kind"] = "tts";
  }
  return info;
}

// ---------------------------------------------------------------- catalogo
Catalog::Catalog() : file_(paths::models_dir() / "catalog.json") {
  json j = jparse(read_text(file_));
  items_ = j.is_object() ? j : json::object();
}

void Catalog::save() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  write_file(file_, jdump(items_, 1));
}

json Catalog::get(const std::string& mid) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!items_.contains(mid)) return nullptr;
  json it = items_[mid];
  return path_exists(P(jstr(it, "path"))) ? it : json(nullptr);
}

json Catalog::list() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json out = json::array();
  for (auto& [k, v] : items_.items())
    if (path_exists(P(jstr(v, "path")))) out.push_back(v);
  return out;
}

json Catalog::add(const json& e) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  items_[jstr(e, "id")] = e;
  save();
  return e;
}

json Catalog::remove(const std::string& mid) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json it = items_.value(mid, json());
  items_.erase(mid);
  save();
  return it;
}

json Catalog::items() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return items_;
}

// ---------------------------------------------------------------- gestor
ModelManager::ModelManager(Config& config, std::function<json()> pg)
    : sources(all_sources(config)), config_(config), profile_(std::move(pg)) {
  autoregister();
}

json ModelManager::search(const std::string& source, const std::string& q, const std::string& kind) {
  if (!sources.count(source)) throw std::out_of_range(source);
  return sources[source]->search(q, kind);
}

json ModelManager::files(const std::string& source, const std::string& repo) {
  if (!sources.count(source)) throw std::out_of_range(source);
  json f = sources[source]->files(repo);
  {
    std::lock_guard<std::mutex> lk(mu_);
    files_cache_[source + "\n" + repo] = f;
  }
  return annotate(f, profile_(), source, repo);
}

json ModelManager::recommended() {
  json sector = json::object();
  try {
    sector = office::sector_models(config_.str("office.profile"));
  } catch (const std::exception& e) {
    L.debug("Sin sugerencias de sector: ", e.what());
  }
  json recs = recommend(profile_(), sector);
  for (auto& r : recs) r["installed"] = !catalog.get(jstr(r, "id")).is_null();
  return recs;
}

fs::path ModelManager::dest(const std::string& source, const std::string& repo, const std::string& file) {
  std::string name = source != "ollama" ? S(P(file).filename()) : slug(file.empty() ? repo : file) + ".gguf";
  return paths::models_dir() / source / P(replace_all(replace_all(repo, "/", "__"), ":", "_")) / P(name);
}

std::string ModelManager::new_job(const std::string& title) {
  std::string jid = uuid_hex().substr(0, 8);
  std::lock_guard<std::mutex> lk(mu_);
  jobs_[jid] = {{"id", jid}, {"title", title}, {"status", "en cola"}, {"done", 0}, {"total", 0}, {"started", now()}};
  return jid;
}

void ModelManager::job_update(const std::string& jid, const json& patch) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [k, v] : patch.items()) jobs_[jid][k] = v;
}

json ModelManager::jobs() {
  std::lock_guard<std::mutex> lk(mu_);
  return jobs_;
}

json ModelManager::job(const std::string& jid) {
  std::lock_guard<std::mutex> lk(mu_);
  return jobs_.value(jid, json());
}

void ModelManager::run(const std::string& jid, std::vector<File> files, const std::function<json()>& on_done) {
  try {
    uint64_t total = 0;
    for (auto& f : files) total += f.size;
    job_update(jid, {{"total", total}});
    uint64_t base = 0;
    for (auto& f : files) {
      job_update(jid, {{"status", "descargando " + S(f.dest.filename())}});
      if (!path_exists(f.dest)) {
        http::download(
            f.url, f.dest, f.headers,
            [&](uint64_t d, uint64_t t) {
              std::lock_guard<std::mutex> lk(mu_);
              jobs_[jid]["done"] = base + d;
              jobs_[jid]["total"] = std::max<uint64_t>(jobs_[jid]["total"].get<uint64_t>(), base + t);
            },
            [&] {
              std::lock_guard<std::mutex> lk(mu_);
              return cancel_.count(jid) > 0;
            });
      }
      base += fsize(f.dest);
      job_update(jid, {{"done", base}});
    }
    json entry = on_done();
    job_update(jid, {{"entry", entry}, {"status", "completado"}});
    L.info("Descarga completada: ", jstr(job(jid), "title"));
  } catch (const http::Cancelled&) {
    job_update(jid, {{"status", "cancelado"}});
  } catch (const std::exception& e) {
    job_update(jid, {{"status", "error"}, {"error", e.what()}});
    L.error("Descarga ", jstr(job(jid), "title"), " fallida: ", e.what());
  }
}

std::string ModelManager::download(const std::string& source, const std::string& repo, const std::string& file,
                                   const std::string& kind, bool with_extras) {
  if (!sources.count(source)) throw std::out_of_range(source);
  auto src = sources[source];
  std::string jid = new_job(src->name() + ": " + repo + " / " + file);
  std::thread([=, this] {
    if (source == "ollama" && ollama_pull) {
      try {
        pull_ollama(jid, file.empty() ? repo : file);
        return;
      } catch (const std::exception& e) {
        L.warning("ollama pull no disponible (", e.what(), "); descarga directa del registro");
      }
    }
    std::vector<File> files;
    fs::path main_dest, mm_dest;
    try {
      if (source == "ollama") {
        json layers = ollama_layers(file.empty() ? repo : file);
        main_dest = dest(source, repo, file);
        files.push_back({layers["model"][0], main_dest, {}, layers["model"][1].get<uint64_t>()});
        if (layers.contains("projector")) {
          mm_dest = main_dest.parent_path() / P(stem(main_dest) + "-mmproj.gguf");
          files.push_back({layers["projector"][0], mm_dest, {}, layers["projector"][1].get<uint64_t>()});
        }
      } else {
        json listing;
        {
          std::lock_guard<std::mutex> lk(mu_);
          listing = files_cache_.count(source + "\n" + repo) ? files_cache_[source + "\n" + repo] : json::array();
        }
        std::vector<std::string> parts = {file};
        auto m = SPLIT_RE().search(file);
        if (m && with_extras) {
          std::string st = file.substr(0, m->start());
          std::vector<std::string> ps;
          for (auto& f : listing) {
            std::string p = jstr(f, "path");
            if (starts_with(p, st) && SPLIT_RE().test(p)) ps.push_back(p);
          }
          std::sort(ps.begin(), ps.end());
          if (!ps.empty()) parts = ps;
        }
        for (auto& part : parts) {
          uint64_t size = 0;
          for (auto& f : listing)
            if (jstr(f, "path") == part) size = (uint64_t)jnum(f.value("size", json(0)));
          files.push_back({src->download_url(repo, part), dest(source, repo, part), src->headers(), size});
        }
        main_dest = files[0].dest;
        if (with_extras && ends_with(lower(file), ".gguf") && !contains(lower(file), "mmproj")) {
          json mm = best_mmproj(listing);
          if (!mm.is_null()) {
            mm_dest = dest(source, repo, jstr(mm, "path"));
            files.push_back({src->download_url(repo, jstr(mm, "path")), mm_dest, src->headers(),
                             (uint64_t)jnum(mm.value("size", json(0)))});
            std::lock_guard<std::mutex> lk(mu_);
            jobs_[jid]["title"] = jobs_[jid]["title"].get<std::string>() + " + proyector de vision";
          }
        }
      }
    } catch (const std::exception& e) {
      job_update(jid, {{"status", "error"}, {"error", e.what()}});
      return;
    }
    run(jid, files, [=, this] { return register_model(main_dest, source, repo, kind, mm_dest.empty() ? "" : S(mm_dest)); });
  }).detach();
  return jid;
}

std::string ModelManager::download_repo(const std::string& source, const std::string& repo, const std::string& kind) {
  if (!sources.count(source)) throw std::out_of_range(source);
  auto src = sources[source];
  std::string jid = new_job(src->name() + ": " + repo + " (repositorio completo)");
  std::thread([=, this] {
    std::vector<File> files;
    fs::path root = paths::models_dir() / source / P(replace_all(repo, "/", "__"));
    try {
      json listing;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (files_cache_.count(source + "\n" + repo)) listing = files_cache_[source + "\n" + repo];
      }
      if (listing.is_null()) listing = src->files(repo);
      std::vector<json> keep;
      bool has_st = false;
      for (auto& f : listing)
        if (!REPO_SKIP().test(jstr(f, "path"))) {
          keep.push_back(f);
          if (ends_with(jstr(f, "path"), ".safetensors")) has_st = true;
        }
      if (has_st)
        std::erase_if(keep, [](const json& f) {
          std::string p = jstr(f, "path");
          for (const char* e : {".bin", ".pt", ".pth", ".ckpt", ".msgpack", ".h5"})
            if (ends_with(p, e)) return !contains(p, "tokenizer");
          return false;
        });
      for (auto& f : keep)
        files.push_back({src->download_url(repo, jstr(f, "path")), root / P(jstr(f, "path")), src->headers(),
                         (uint64_t)jnum(f.value("size", json(0)))});
    } catch (const std::exception& e) {
      job_update(jid, {{"status", "error"}, {"error", e.what()}});
      return;
    }
    run(jid, files, [=, this] { return register_model(root, source, repo, kind); });
  }).detach();
  return jid;
}

void ModelManager::pull_ollama(const std::string& jid, const std::string& name) {
  job_update(jid, {{"status", "ollama pull"}});
  bool cancelled = false;
  ollama_pull(name, [&](const json& ev) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (cancel_.count(jid)) {
        cancelled = true;
        return false;
      }
    }
    if (truthy(ev.value("total", json(0))))
      job_update(jid, {{"total", ev["total"]}, {"done", ev.value("completed", json(0))}});
    job_update(jid, {{"status", uleft("ollama: " + jstr(ev, "status"), 60)}});
    if (truthy(ev.value("error", json(""))))
      throw std::runtime_error(jstr(ev, "error"));
    return true;
  });
  if (cancelled) {
    job_update(jid, {{"status", "cancelado"}});
    return;
  }
  job_update(jid, {{"status", "completado"}, {"entry", {{"id", "ollama:" + name}, {"kind", "llm"}, {"backend", "ollama"}}}});
  L.info("Modelo de Ollama instalado: ", name);
}

// fichero de un repositorio: el del catalogo si sigue ahi; si no (los repositorios cambian de nombres), el que mas se
// le parece: misma extension, mismo comienzo y la cuantizacion preferida
static std::string closest_file(const std::vector<std::string>& names, const std::string& file,
                                const std::vector<std::string>& prefs) {
  if (names.empty() || std::find(names.begin(), names.end(), file) != names.end()) return file;
  std::string ext = lower(S(P(file).extension())), base = lower(S(P(file).filename()));
  std::string head = base.substr(0, std::min<size_t>(base.size(), std::max<size_t>(3, base.find_first_of("-_."))));
  std::vector<std::string> c;
  for (auto& n : names) {
    std::string ln = lower(S(P(n).filename()));
    if (lower(S(P(n).extension())) == ext && starts_with(ln, head) && !contains(ln, "mmproj")) c.push_back(n);
  }
  std::sort(c.begin(), c.end());
  for (auto& p : prefs)
    for (auto& x : c)
      if (contains(upper(x), p)) return x;
  return c.empty() ? file : c[0];
}

json ModelManager::resolve_recommended(const json& rec) {
  std::string kind = jstr(rec, "kind");
  if (kind == "image") {
    json out = rec;
    auto list = [&](const std::string& repo) {
      std::vector<std::string> names;
      try {
        for (auto& f : sources["huggingface"]->files(repo)) names.push_back(jstr(f, "path"));
      } catch (const std::exception& e) {
        L.info("No se pudo listar ", repo, " (", e.what(), "): se usan los nombres del catalogo");
      }
      return names;
    };
    out["file"] = closest_file(list(jstr(rec, "repo")), jstr(rec, "file"), {"Q4_K", "Q4_0", "Q5_K", "Q8_0", "F16"});
    json comps = rec.value("componentes", json::object());
    if (comps.is_object())
      for (auto& [k, v] : comps.items())
        out["componentes"][k]["file"] = closest_file(list(jstr(v, "repo")), jstr(v, "file"), {"Q4_K_M", "Q4_K", "Q8_0", "FP8", "F16"});
    return out;
  }
  if ((kind != "llm" && kind != "embed") || !ends_with(jstr(rec, "file"), ".gguf")) return rec;
  std::vector<std::string> names;
  try {
    for (auto& f : sources["huggingface"]->files(jstr(rec, "repo"))) names.push_back(jstr(f, "path"));
  } catch (const std::exception& e) {
    L.info("No se pudo listar ", jstr(rec, "repo"), " (", e.what(), "): se usan los nombres del catalogo");
    return rec;
  }
  auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  std::string mm = jstr(rec, "mmproj");
  if (names.empty() || (has(jstr(rec, "file")) && (mm.empty() || has(mm)))) return rec;
  std::vector<std::string> projs, mains;
  for (auto& n : names) {
    if (!ends_with(lower(n), ".gguf")) continue;
    if (contains(lower(n), "mmproj")) projs.push_back(n);
    else if (!SPLIT_RE().test(n)) mains.push_back(n);
  }
  auto pick = [](std::vector<std::string> c, std::vector<std::string> prefs) -> std::string {
    std::sort(c.begin(), c.end());
    for (auto& p : prefs)
      for (auto& x : c)
        if (contains(upper(x), p)) return x;
    return c.empty() ? "" : c[0];
  };
  json out = rec;
  if (!has(jstr(rec, "file"))) {
    std::string p = kind == "embed" ? pick(mains, {"Q8_0", "F16", "Q6_K", "Q5_K_M"})
                                    : pick(mains, {"Q4_K_M", "Q4_K_S", "Q4_0", "Q5_K_M", "Q8_0"});
    if (!p.empty()) out["file"] = p;
  }
  if (!mm.empty() && !has(mm)) {
    std::string p = pick(projs, {"Q8_0", "F16", "BF16", "F32"});
    if (!p.empty()) out["mmproj"] = p;
  }
  L.info("Catalogo ", jstr(rec, "id"), ": ficheros resueltos en el repositorio -> ", jstr(out, "file"), " / ",
         jstr(out, "mmproj"));
  return out;
}

std::string ModelManager::download_recommended(const std::string& rec_id) {
  json base;
  for (auto& r : recommended_list())
    if (jstr(r, "id") == rec_id) base = r;
  if (base.is_null()) throw std::out_of_range(rec_id);
  std::string jid = new_job(jstr(base, "name"));
  std::thread([=, this] {
    json rec = resolve_recommended(base);
    auto hf = sources["huggingface"];
    std::string repo = jstr(rec, "repo");
    std::vector<File> files = {{hf->download_url(repo, jstr(rec, "file")), dest("huggingface", repo, jstr(rec, "file")), {}, 0}};
    std::vector<std::string> extras;
    if (!jstr(rec, "mmproj").empty()) extras.push_back(jstr(rec, "mmproj"));
    for (auto& e : rec.value("extra", json::array())) extras.push_back(e.get<std::string>());
    for (auto& e : extras) files.push_back({hf->download_url(repo, e), dest("huggingface", repo, e), {}, 0});
    // modelos de imagen GGUF de solo difusion: VAE y codificadores de texto, que suelen estar en otros repositorios
    json comps = json::object();
    size_t n_main = files.size();
    const json componentes = rec.value("componentes", json::object());   // items() de un temporal queda colgando
    for (auto& [k, v] : componentes.items()) {
      fs::path d = dest("huggingface", jstr(v, "repo"), jstr(v, "file"));
      files.push_back({hf->download_url(jstr(v, "repo"), jstr(v, "file")), d, {}, (uint64_t)(jnum(v.value("size_mb", json(0))) * 1048576)});
      comps[k] = S(d);
    }
    for (size_t i = 0; i < n_main; ++i) {
      double comp_mb = 0;
      for (auto& [k, v] : componentes.items()) comp_mb += jnum(v.value("size_mb", json(0)));
      files[i].size = (uint64_t)(std::max(1.0, jnum(rec["size_mb"]) - comp_mb) * 1048576 / n_main);
    }
    fs::path main = files[0].dest;
    std::string mm = !jstr(rec, "mmproj").empty() ? S(files[1].dest) : "";
    run(jid, files, [=, this] {
      json entry = register_model(main, "huggingface", repo, jstr(rec, "kind"), mm, jstr(rec, "id"), jstr(rec, "name"),
                                  {{"gen", rec.value("gen", json())},
                                   {"vision", rec.value("vision", json(false))},
                                   {"reasoning", rec.value("reasoning", json(false))},
                                   {"componentes", comps.empty() ? json() : comps}});
      auto_activate(entry);
      return entry;
    });
  }).detach();
  return jid;
}

void ModelManager::cancel(const std::string& jid) {
  std::lock_guard<std::mutex> lk(mu_);
  cancel_.insert(jid);
}

json ModelManager::register_model(const fs::path& path, const std::string& source, const std::string& repo,
                                  const std::string& kind, const std::string& mmproj_in, const std::string& mid,
                                  const std::string& name, const json& extra) {
  json info = classify(path, repo);
  std::string k = jstr(info, "kind");
  if (!kind.empty() && (k == "other" || k == "llm") && kind != "llm") info["kind"] = kind;
  std::string mmproj = mmproj_in;
  if (mmproj.empty() && info["format"] == "gguf" && info["kind"] == "llm") {
    for (auto& c : glob(path.parent_path(), "*.gguf"))
      if (c != path && contains(lower(S(c.filename())), "mmproj")) {
        mmproj = S(c);
        break;
      }
  }
  uint64_t size = 0;
  if (is_file(path)) size = fsize(path);
  else
    for (auto& f : glob(path, "*", true))
      if (is_file(f)) size += fsize(f);
  json entry = {{"id", !mid.empty() ? mid : slug(is_file(path) ? stem(path) : S(path.filename()))},
                {"name", !name.empty() ? name : stem(path)},
                {"kind", info["kind"]},
                {"format", info["format"]},
                {"path", S(path)},
                {"mmproj", mmproj.empty() ? json(nullptr) : json(mmproj)},
                {"vision", !mmproj.empty()},
                {"size", size},
                {"source", source},
                {"repo", repo},
                {"added", now()},
                {"meta", info.value("meta", json::object())}};
  if (info.contains("difusion")) {
    entry["difusion"] = info["difusion"];
    if (!truthy(entry.value("gen", json())) && truthy(info["difusion"].value("gen", json())))
      entry["gen"] = info["difusion"]["gen"];
  }
  for (auto& [ek, ev] : extra.items())
    if (!ev.is_null()) entry[ek] = ev;
  entry["vision"] = truthy(entry["vision"]) || !mmproj.empty();
  return catalog.add(entry);
}

json ModelManager::import_local(const std::string& path_in, bool copy, const std::string& kind) {
  std::string pin = path_in;
  if (starts_with(pin, "~")) pin = S(home_dir()) + pin.substr(1);
  fs::path p = P(pin);
  if (!path_exists(p)) throw std::runtime_error("No existe: " + path_in);
  if (copy) {
    fs::path d = paths::models_dir() / "local" / p.filename();
    mkdirs(d.parent_path());
    std::error_code ec;
    if (is_dir(p)) fs::copy(p, d, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    else fs::copy_file(p, d, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("No se pudo copiar: " + ec.message());
    p = d;
  }
  return register_model(p, "local", "", kind);
}

void ModelManager::autoregister() {
  std::set<std::string> known;
  for (json items_tmp_ = catalog.items(); auto& [k, m] : items_tmp_.items()) {
    known.insert(jstr(m, "path"));
    if (m.value("mmproj", json()).is_string()) known.insert(m["mmproj"].get<std::string>());
  }
  std::map<std::pair<std::string, std::string>, json> recs;
  for (auto& r : recommended_list())
    recs[{replace_all(jstr(r, "repo"), "/", "__"), S(P(jstr(r, "file")).filename())}] = r;
  for (auto& f : glob(paths::models_dir(), "*", true)) {
    std::string ext = suffix_lower(f);
    if ((ext != ".gguf" && ext != ".safetensors" && ext != ".onnx") || known.count(S(f)) ||
        contains(lower(S(f.filename())), "mmproj") || !is_file(f))
      continue;
    auto it = recs.find({S(f.parent_path().filename()), S(f.filename())});
    try {
      if (it != recs.end()) {
        const json& rec = it->second;
        std::string mm;
        if (!jstr(rec, "mmproj").empty()) {
          fs::path c = f.parent_path() / P(jstr(rec, "mmproj")).filename();
          if (path_exists(c)) mm = S(c);
        }
        auto_activate(register_model(f, "huggingface", jstr(rec, "repo"), jstr(rec, "kind"), mm, jstr(rec, "id"),
                                     jstr(rec, "name"),
                                     {{"gen", rec.value("gen", json())},
                                      {"vision", rec.value("vision", json(false))},
                                      {"reasoning", rec.value("reasoning", json(false))}}));
      } else if (classify(f)["kind"] != "projector") {
        register_model(f, "local");
      }
    } catch (const std::exception& e) {
      L.warning("No se pudo registrar ", S(f), ": ", e.what());
    }
  }
}

void ModelManager::remove(const std::string& mid, bool remove_files) {
  json item = catalog.remove(mid);
  if (!item.is_null() && remove_files && contains(jstr(item, "path"), S(paths::models_dir()))) {
    for (auto& p : {jstr(item, "path"), jstr(item, "mmproj")})
      if (!p.empty() && path_exists(P(p))) {
        if (is_dir(P(p))) rm_rf(P(p));
        else remove_file(P(p));
      }
  }
}

json ModelManager::activate(const std::string& mid) {
  json entry = catalog.get(mid);
  if (entry.is_null()) throw std::out_of_range(mid);
  std::string kind = jstr(entry, "kind");
  if (!ACTIVE_KEYS.count(kind)) throw std::invalid_argument("Tipo '" + kind + "' no activable");
  config_.set(ACTIVE_KEYS.at(kind), mid);
  notify(entry);
  return entry;
}

void ModelManager::auto_activate(const json& entry) {
  std::string kind = jstr(entry, "kind");
  if (!ACTIVE_KEYS.count(kind)) return;
  if (!truthy(config_.get(ACTIVE_KEYS.at(kind)))) {
    config_.set(ACTIVE_KEYS.at(kind), entry["id"]);
    notify(entry);
  }
}

void ModelManager::notify(const json& entry) {
  for (auto& fn : on_activate) {
    try {
      fn(entry);
    } catch (const std::exception& e) {
      L.warning("Activacion de ", jstr(entry, "id"), ": ", e.what());
    }
  }
}

json ModelManager::active() {
  return {{"llm", config_.get("llm.default_model")}, {"image", config_.get("media.image_model")},
          {"tts", config_.get("media.tts_model", "")}, {"embed", config_.get("memory.embed_model", "")}};
}

}  // namespace nx::models
