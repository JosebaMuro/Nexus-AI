// Biblioteca RAG (nexus/rag): lectores de documentos, extraccion web, archivos comprimidos y busqueda hibrida.
#pragma once
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/db.h"
#include "memory/memory.h"
#include "util/fs.h"
#include "util/json.h"

namespace nx::rag {

constexpr size_t CHUNK = 900, OVERLAP = 150;
extern const std::set<std::string> STOP_WORDS;   // rag/store.py STOP
std::vector<std::string> chunk_text(const std::string& text, size_t size = CHUNK, size_t overlap = OVERLAP);

// ---------------------------------------------------------------- lectores
struct Doc {
  std::string title;
  std::vector<std::string> pages;
};
std::string archive_suffix(const std::string& name);   // ".tar.gz", ".zip"... o ""
bool supported(const std::string& name);
json formats_info();                                    // GET /api/rag/formats
std::pair<std::string, std::string> find_extractor(const std::string& kind);   // (herramienta, ruta) para rar o 7z
Doc read_document(const fs::path& path);                // lanza si no se puede leer
Doc read_pdf(const std::string& bytes, const std::string& fallback_title);
Doc read_text_file(const fs::path& path);
// (titulo, paginas, error): nunca lanza
std::tuple<std::string, std::vector<std::string>, std::string> read_safe(const fs::path& path);

// ---------------------------------------------------------------- extraccion de HTML
json extract_html(const std::string& raw, const std::string& url);
std::string urljoin(const std::string& base, const std::string& href);
std::string url_host(const std::string& url);

// archivos comprimidos: extraccion segura (zip slip, bombas, enlaces) y lectura para indexar
struct ExtractResult {
  std::vector<std::string> files;   // rutas relativas con "/"
  std::vector<std::pair<std::string, std::string>> skipped;
  uint64_t bytes = 0;
  std::string tool;
};
extern const std::set<std::string> RICH_EXT;
std::string detect_kind(const fs::path& path, const std::string& name);   // zip, rar, 7z, tar o ""
ExtractResult extract_any(const fs::path& path, const fs::path& dest, const std::string& kind);
bool indexable(const std::string& rel);
std::pair<std::vector<std::string>, std::string> read_for_index(const fs::path& p);   // (paginas, error)

class RAGStore;
json ingest_archive(RAGStore& rag, const fs::path& src, std::string title, const std::string& source,
                    const json& meta, const std::string& agent);
std::string tree_text(const std::vector<std::string>& files, size_t limit = 400);

// ---------------------------------------------------------------- biblioteca
class RAGStore {
 public:
  RAGStore(DB& db, mem::EmbedderGetter emb);
  json ingest_pages(const std::string& title, const std::vector<std::string>& pages, const std::string& source = "",
                    const std::string& kind = "text", const std::string& path = "", const json& meta = json::object(),
                    const std::vector<std::string>& labels = {}, const std::string& agent = "", bool link = true);
  json ingest_file(const fs::path& src, const std::string& title = "", const std::string& source = "",
                   const json& meta = json::object(), bool copy = true, const std::string& agent = "", bool link = true);
  // [(ruta, nombre original)] -> [{"name", "doc"} | {"name", "error"}]
  json ingest_many(const std::vector<std::pair<fs::path, std::string>>& items,
                   const std::function<void(const json&)>& emit = nullptr, const std::string& agent = "");
  json search(const std::string& query, int k = 5, double min_score = 0.15, long long doc_id = 0,
              const std::optional<std::string>& agent = std::nullopt);
  json get(long long doc_id);
  json list(const std::optional<std::string>& agent = std::nullopt);
  json assign(long long doc_id, const std::string& agent);
  int reassign_agent(const std::string& agent, const std::string& to = "");
  void remove(long long doc_id);
  json stats();
  void invalidate();

  DB& db;
  mem::EmbedderGetter embedder;
  fs::path files;
  mem::KnowledgeGraph* graph = nullptr;
  std::function<void()> on_pending;
  mem::HashingEmbedder fast;

 private:
  struct Index {
    mem::Mat mat;
    std::vector<long long> ids;
    std::vector<std::string> owners;
  };
  struct Cache {
    std::string emb_id;
    Index sem, pend;
  };
  std::recursive_mutex mu_;
  std::optional<Cache> cache_;
  Cache& matrix();
  void link(const std::string& agent, const std::string& title, const std::string& text);
  void unlink(const json& d);
};

}  // namespace nx::rag
