// Documentacion de la web a la biblioteca (rag/url_pdf.py, pdfgen.py, crawler.py, crawl_jobs.py y domain_kg.py):
// una URL como PDF maquetado, rastreo de sitios de documentacion (por lotes reanudables) y el grafo multicapa de un
// dominio rastreado entero, empaquetado para darselo a un agente.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Services;
}

namespace nx::rag {

using Emit = std::function<void(const json&)>;

// ---------------------------------------------------------------- PDF maquetado (pdfgen.py)
// doc: lo que devuelve extract_html. Devuelve {"path", "pages", "images_embedded", "unicode_fonts"}
json build_pdf(const json& doc, const std::string& summary, const std::vector<std::string>& key_points,
               const fs::path& out, int max_images = 8);

// ---------------------------------------------------------------- URL -> PDF -> RAG (url_pdf.py)
std::string check_public_url(const std::string& url);   // lanza std::invalid_argument si no es publica
std::string decode_body(const std::string& raw, const std::string& ctype);   // segun el charset (UTF-8 si no hay)
// GET con redirecciones comprobadas una a una y tope de bytes: (estado, tipo, cuerpo, url final); lanza si la red falla
std::tuple<int, std::string, std::string, std::string> fetch_limited(const std::string& url, size_t cap,
                                                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                                                     double timeout);
std::pair<std::string, std::vector<std::string>> extractive_summary(const std::string& text, size_t n = 4);
std::optional<std::pair<std::string, std::vector<std::string>>> llm_summary(Services& s, const json& doc);
json process_url(Services& s, const std::string& url, const Emit& emit = nullptr, int max_images = 8,
                 const std::string& agent = "");
int assimilate(Services& s, const json& doc, const std::string& summary, const std::vector<std::string>& points,
               long long doc_id, const std::string& url, const std::string& agent = "");

// ---------------------------------------------------------------- rastreo (crawler.py)
namespace crawler {
constexpr int MAX_PAGES_LIMIT = 500;
// (estado HTTP, tipo, cuerpo, url final); las paginas llegan como texto UTF-8 y los documentos como bytes
using Fetcher = std::function<std::tuple<int, std::string, std::string, std::string>(const std::string&)>;

std::string normalize(const std::string& url);
std::string site_root(const std::string& netloc);
std::string doc_suffix(const std::string& url, const std::string& ctype = "");
std::string scope_prefix(const std::string& start, const std::string& scope);
bool in_scope(const std::string& url, const std::string& start, const std::string& prefix, bool domain = false);

struct CrawlOpts {
  int max_pages = 40, max_depth = 3;
  std::string scope = "path";
  Fetcher fetch;
  Emit emit;
  double delay = 0.1, max_seconds = 900;
  bool use_sitemap = true, respect_robots = true;
  int connections = 0;
  json state = nullptr;
  long long max_chars = 6000000;
  std::function<bool()> should_stop;
  long long offset = 0, total = 0, seed_limit = 0;
};
json crawl(const std::string& start_url, CrawlOpts o);
json crawl_site(Services& s, const std::string& start_url, int max_pages = 40, int max_depth = 3,
                const std::string& scope = "path", const Emit& emit = nullptr, const std::string& agent = "",
                int batch_pages = 0, const std::string& job = "", double max_seconds = 900);
// piezas que usan los rastreos por lotes
std::string site_title(const json& res);
json ingest_batch(Services& s, const json& res, const std::string& title, const std::string& agent, bool learn, bool use_llm,
                  const json& meta_extra);
json replace_old(Services& s, const std::string& start, const std::string& prefix, const std::string& agent,
                 const std::set<long long>& keep);
}  // namespace crawler

// ---------------------------------------------------------------- rastreos por lotes (crawl_jobs.py)
namespace crawl_jobs {
extern const char *RUNNING, *PAUSED, *DONE, *INTERRUPTED, *FAILED;
std::pair<int, long long> auto_batch(const json& profile);
json get(const std::string& id);   // null si no existe
void save(json& job);
json create(const std::string& url, long long max_pages, long long max_depth, const std::string& scope,
            const std::string& agent, bool learn, int batch_pages, long long batch_chars);
json pub(const json& job);
json list_jobs();
bool remove(Services& s, const std::string& id, bool docs);
bool stop(const std::string& id);
bool is_running(const std::string& id);
void run_background(Services& s, const std::string& id, const Emit& emit);
fs::path kg_path(const std::string& id);
fs::path knowledge_path(const std::string& id);
json build_knowledge(Services& s, json& job, const Emit& emit = nullptr);
std::vector<std::string> resume_interrupted(Services& s, const Emit& emit);
// ejecuta (o reanuda) los lotes que le faltan; usado por crawl_site y por los hilos de fondo
json run_batches(Services& s, json job, const Emit& emit, const std::function<bool()>& should_stop,
                 double max_seconds = -1, bool replace = true);
json job_result(const json& job);
std::shared_ptr<std::atomic<bool>> claim(const std::string& id);   // lanza std::runtime_error si ya esta en marcha
void release(const std::string& id);
}  // namespace crawl_jobs

// ---------------------------------------------------------------- grafo multicapa del dominio (domain_kg.py)
namespace domain_kg {
std::string section_of(const std::string& url);
std::vector<std::string> claims_of(const std::string& text, size_t limit = 14);
class DomainGraph {
 public:
  explicit DomainGraph(const fs::path& path);
  int add_pages(const json& pages);
  json stats();
  // {"graph", "markdown", "manifest"}
  json build(const std::string& title, const std::string& site, const std::function<std::string(const std::string&)>& llm,
             Services* s, const std::vector<long long>& exclude_docs, const std::function<void(const std::string&)>& step,
             int max_llm_topics = 40, long long max_claims = 300000);
  void package(const fs::path& out, const json& built);

 private:
  fs::path path_;
};
json read_package(const fs::path& path);   // lanza std::invalid_argument si no es un paquete
json import_package(Services& s, const json& data, const std::string& agent = "", const std::string& source = "");
}  // namespace domain_kg

}  // namespace nx::rag
