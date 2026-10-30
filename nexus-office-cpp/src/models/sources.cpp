// Los 5 repositorios de modelos (models/sources.py): Hugging Face, Ollama, ModelScope, Civitai y Kaggle.
#include <set>

#include "models/models.h"
#include "util/crypto.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::models {

static json take(const json& arr, size_t n) {
  json out = json::array();
  if (!arr.is_array()) return out;
  for (size_t i = 0; i < arr.size() && i < n; ++i) out.push_back(arr[i]);
  return out;
}

static http::Headers bearer(const std::string& tok) {
  if (tok.empty()) return {};
  return {{"Authorization", "Bearer " + tok}};
}

static std::string get_text(const std::string& url, const std::map<std::string, std::string>& params = {}) {
  std::string u = url + (params.empty() ? "" : "?" + http::query_string(params));
  auto r = http::get(u, {}, 20);
  if (r.status == 0) throw http::HttpError(0, r.error);
  if (!r.ok()) throw http::HttpError(r.status, cat("HTTP ", r.status, " en ", url));
  return r.body;
}

static std::vector<std::string> unique(const std::vector<std::string>& v) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (auto& x : v)
    if (seen.insert(x).second) out.push_back(x);
  return out;
}

class HuggingFace : public Source {
 public:
  using Source::Source;
  std::string id() const override { return "huggingface"; }
  std::string name() const override { return "Hugging Face"; }
  http::Headers headers() const override { return bearer(config.str("models.huggingface_token")); }
  json search(const std::string& q, const std::string& kind) override {
    std::map<std::string, std::string> p = {{"search", q}, {"limit", "30"}, {"sort", "downloads"}, {"direction", "-1"}};
    static const std::map<std::string, std::string> HINT = {
        {"image", "text-to-image"}, {"stt", "automatic-speech-recognition"}, {"tts", "text-to-speech"}};
    // «fmt:<formato>» (filtro por formato de la vista Modelos): GGUF, ONNX, LiteRT-LM (Tensor/NPU) o Genie (QNN)
    if (starts_with(kind, "fmt:")) {
      std::string f = kind.substr(4);
      if (f == "gguf" || f == "onnx" || f == "safetensors") p["filter"] = f;
      else if (f == "litertlm") p["filter"] = "litert-lm";
      else if (f == "genie") p["author"] = "qualcomm";
    } else if (kind == "llm") p["filter"] = "gguf";
    else if (HINT.count(kind)) p["pipeline_tag"] = HINT.at(kind);
    json data = http::get_json("https://huggingface.co/api/models", headers(), p);
    // LiteRT-LM: no todos los repositorios llevan la etiqueta; se repite buscando «litert»
    if (kind == "fmt:litertlm" && data.is_array() && data.empty()) {
      p.erase("filter");
      p["search"] = q + " litert";
      data = http::get_json("https://huggingface.co/api/models", headers(), p);
    }
    json out = json::array();
    for (auto& m : data) {
      json tags = json::array();
      for (auto& t : m.value("tags", json::array()))
        if (t.is_string() && !contains(t.get<std::string>(), ":") && tags.size() < 8) tags.push_back(t);
      out.push_back({{"id", m["id"]}, {"name", m["id"]}, {"downloads", m.value("downloads", json(0))},
                     {"likes", m.value("likes", json(0))}, {"tags", tags}, {"task", jstr(m, "pipeline_tag")}});
    }
    return out;
  }
  json files(const std::string& repo) override {
    json data = http::get_json("https://huggingface.co/api/models/" + repo + "/tree/main", headers(), {{"recursive", "true"}});
    json out = json::array();
    for (auto& f : data) {
      if (jstr(f, "type") != "file") continue;
      json lfs = f.value("lfs", json::object());
      json size = lfs.is_object() && lfs.contains("size") ? lfs["size"] : f.value("size", json(0));
      out.push_back({{"path", f["path"]}, {"size", size}});
    }
    return out;
  }
  std::string download_url(const std::string& repo, const std::string& file) override {
    return "https://huggingface.co/" + repo + "/resolve/main/" + file;
  }
};

static const std::string OLLAMA_REG = "https://registry.ollama.ai/v2";

json ollama_layers(const std::string& ref) {
  std::string name = ref.substr(0, ref.find(':'));
  std::string tag = ref.find(':') == std::string::npos ? "latest" : ref.substr(ref.find(':') + 1);
  if (tag.empty()) tag = "latest";
  std::string ns = contains(name, "/") ? name : "library/" + name;
  json man = http::get_json(OLLAMA_REG + "/" + ns + "/manifests/" + tag,
                            {{"Accept", "application/vnd.docker.distribution.manifest.v2+json"}});
  json out = json::object();
  for (auto& l : man.value("layers", json::array())) {
    std::string mt = jstr(l, "mediaType");
    mt = mt.substr(mt.rfind('.') + 1);
    if (mt == "model" || mt == "projector")
      out[mt] = json::array({OLLAMA_REG + "/" + ns + "/blobs/" + jstr(l, "digest"), l.value("size", json(0))});
  }
  return out;
}

class Ollama : public Source {
 public:
  using Source::Source;
  std::string id() const override { return "ollama"; }
  std::string name() const override { return "Ollama"; }
  json search(const std::string& q, const std::string&) override {
    std::string html = get_text("https://ollama.com/search", {{"q", q}});
    static const Rx r(R"rx(href="/library/([\w.\-]+)")rx");
    json out = json::array();
    auto names = unique(r.findall(html));
    for (size_t i = 0; i < names.size() && i < 30; ++i)
      out.push_back({{"id", names[i]}, {"name", names[i]}, {"downloads", 0}, {"tags", json::array()}, {"task", "llm"}});
    return out;
  }
  json files(const std::string& repo) override {
    std::string name = repo.substr(0, repo.find(':'));
    std::string html = get_text("https://ollama.com/library/" + name + "/tags");
    Rx r("/library/" + re_escape(name) + R"(:([\w.\-]+))");
    json out = json::array();
    for (auto& t : unique(r.findall(html))) out.push_back({{"path", name + ":" + t}, {"size", 0}});
    return out;
  }
  std::string download_url(const std::string& repo, const std::string& file) override {
    return ollama_layers(file.empty() ? repo : file)["model"][0].get<std::string>();
  }
};

class ModelScope : public Source {
 public:
  using Source::Source;
  std::string id() const override { return "modelscope"; }
  std::string name() const override { return "ModelScope"; }
  http::Headers headers() const override { return bearer(config.str("models.modelscope_token")); }
  json search(const std::string& q, const std::string&) override {
    json body = {{"PageSize", 30}, {"PageNumber", 1}, {"SortBy", "Default"}, {"Target", ""},
                 {"Criterion", json::array()}, {"Name", q}};
    http::Request req;
    req.method = "PUT";
    req.url = "https://www.modelscope.cn/api/v1/dolphin/models";
    req.headers = headers();
    req.body = jdump(body);
    req.content_type = "application/json";
    req.timeout = 20;
    auto r = http::send(req);
    if (!r.ok()) throw http::HttpError(r.status, r.status ? cat("HTTP ", r.status) : r.error);
    json j = r.json_body();
    json models = j.is_object() ? j.value("Data", json::object()).value("Model", json::object()).value("Models", json::array())
                                : json::array();
    json out = json::array();
    if (!models.is_array()) return out;
    for (auto& m : models) {
      std::string id = jstr(m, "Path") + "/" + jstr(m, "Name");
      std::vector<std::string> tasks;
      for (auto& t : m.value("Tasks", json::array()))
        if (t.is_object()) tasks.push_back(jstr(t, "Name"));
      out.push_back({{"id", id}, {"name", id}, {"downloads", m.value("Downloads", json(0))},
                     {"tags", take(m.value("Tags", json::array()), 8)}, {"task", uleft(join(tasks, ", "), 60)}});
    }
    return out;
  }
  json files(const std::string& repo) override {
    json d = http::get_json("https://www.modelscope.cn/api/v1/models/" + repo + "/repo/files", headers(),
                            {{"Revision", "master"}, {"Recursive", "true"}});
    json out = json::array();
    for (auto& f : d.value("Data", json::object()).value("Files", json::array()))
      if (jstr(f, "Type") != "tree") out.push_back({{"path", f["Path"]}, {"size", f.value("Size", json(0))}});
    return out;
  }
  std::string download_url(const std::string& repo, const std::string& file) override {
    return "https://www.modelscope.cn/models/" + repo + "/resolve/master/" + file;
  }
};

class Civitai : public Source {
 public:
  using Source::Source;
  std::string id() const override { return "civitai"; }
  std::string name() const override { return "Civitai"; }
  http::Headers headers() const override { return bearer(config.str("models.civitai_token")); }
  json search(const std::string& q, const std::string& kind) override {
    std::map<std::string, std::string> p = {{"query", q}, {"limit", "30"}};
    if (kind.empty() || kind == "image") p["types"] = "Checkpoint";
    json d = http::get_json("https://civitai.com/api/v1/models", headers(), p);
    json out = json::array();
    for (auto& m : d.value("items", json::array()))
      out.push_back({{"id", std::to_string(m["id"].get<long long>())}, {"name", m["name"]},
                     {"downloads", m.value("stats", json::object()).value("downloadCount", json(0))},
                     {"tags", take(m.value("tags", json::array()), 8)}, {"task", jstr(m, "type")}});
    return out;
  }
  json files(const std::string& repo) override {
    json m = http::get_json("https://civitai.com/api/v1/models/" + repo, headers());
    json out = json::array();
    json vers = take(m.value("modelVersions", json::array()), 5);
    for (auto& v : vers)
      for (auto& f : v.value("files", json::array()))
        out.push_back({{"path", f["name"]}, {"size", (long long)(jnum(f.value("sizeKB", json(0))) * 1024)},
                       {"url", f["downloadUrl"]}, {"version", v.value("name", json())},
                       {"base_model", v.value("baseModel", json())}});
    return out;
  }
  std::string download_url(const std::string& repo, const std::string& file) override {
    for (auto& f : files(repo))
      if (jstr(f, "path") == file) return jstr(f, "url");
    throw std::runtime_error("no encontrado: " + file);
  }
};

class Kaggle : public Source {
 public:
  using Source::Source;
  std::string id() const override { return "kaggle"; }
  std::string name() const override { return "Kaggle Models"; }
  http::Headers headers() const override {
    std::string u = config.str("models.kaggle_username"), k = config.str("models.kaggle_key");
    if (!u.empty() && !k.empty()) return {{"Authorization", "Basic " + b64encode(u + ":" + k)}};
    return {};
  }
  json search(const std::string& q, const std::string&) override {
    json d = http::get_json("https://www.kaggle.com/api/v1/models/list", headers(), {{"search", q}, {"pageSize", "30"}});
    json out = json::array();
    for (auto& m : d.value("models", json::array()))
      out.push_back({{"id", m["ref"]}, {"name", m.value("title", m["ref"])},
                     {"downloads", m.value("voteCountNullable", json(0))}, {"tags", json::array()},
                     {"task", uleft(jstr(m, "subtitle"), 80)}});
    return out;
  }
  json files(const std::string& repo) override {
    auto parts = split(repo, "/", 1);
    if (parts.size() < 2) throw std::invalid_argument("referencia de Kaggle: propietario/modelo");
    json m = http::get_json("https://www.kaggle.com/api/v1/models/" + parts[0] + "/" + parts[1] + "/get", headers());
    json out = json::array();
    for (auto& inst : m.value("instances", json::array())) {
      std::string ref = jstr(inst, "framework") + "/" + jstr(inst, "slug") + "/" + jstr(inst, "versionNumber");
      json size = inst.value("totalUncompressedBytes", json(0));
      out.push_back({{"path", ref}, {"size", size.is_null() ? json(0) : size},
                     {"note", "archivo .tar.gz (requiere usuario/clave de Kaggle y aceptar licencia)"}});
    }
    return out;
  }
  std::string download_url(const std::string& repo, const std::string& file) override {
    return "https://www.kaggle.com/api/v1/models/" + repo + "/" + file + "/download";
  }
};

std::map<std::string, std::shared_ptr<Source>> all_sources(Config& c) {
  std::map<std::string, std::shared_ptr<Source>> m;
  for (std::shared_ptr<Source> s : std::vector<std::shared_ptr<Source>>{
           std::make_shared<HuggingFace>(c), std::make_shared<Ollama>(c), std::make_shared<ModelScope>(c),
           std::make_shared<Civitai>(c), std::make_shared<Kaggle>(c)})
    m[s->id()] = s;
  return m;
}

}  // namespace nx::models
