// Rastreos por lotes (rag/crawl_jobs.py y crawl_batches/job_result de crawler.py): dominios enteros que no caben de
// una vez se procesan en lotes correlativos y reanudables.
//
// Un trabajo guarda en data/rag/crawls/<id>.json todo lo necesario para seguir donde se quedo: la cola de URLs
// pendientes (en orden de descubrimiento), las visitadas, las huellas del contenido ya leido y la lista de lotes ya
// indexados. Cada lote es un documento propio de la biblioteca («Documentación: X · lote 003 (págs. 401-600)»), asi
// una caida, un reinicio o una pausa no pierden nada. El tamano de lote se ajusta a la RAM del equipo
// (rag.crawl_batch_pages en config.yaml, 0 = automatico).
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "core/config.h"
#include "core/services.h"
#include "hardware/hardware.h"
#include "llm/llm.h"
#include "rag/rag.h"
#include "rag/web.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag::crawl_jobs {

const char *RUNNING = "en curso", *PAUSED = "pausado", *DONE = "completado", *INTERRUPTED = "interrumpido",
           *FAILED = "error", *QUEUED = "en cola";

namespace {

auto L = log::get("crawl_jobs");
constexpr long long MAX_DOMAIN_PAGES = 100000;
constexpr long long UNLIMITED = 1000000000000LL;   // dominio completo: hasta agotar todos los enlaces del dominio
std::mutex g_mu;
std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_running;
std::mutex g_qmu;                 // cola de rastreos (listas de URLs)
std::deque<std::string> g_queue;
int g_workers = 0;

fs::path dir() { return paths::sub({"rag", "crawls"}); }

fs::path path_of(const std::string& id) {
  static const Rx ID(R"([a-z0-9-]{6,40})");
  if (!ID.fullmatch(id)) throw std::out_of_range(id);
  return dir() / P(id + ".json");
}

}  // namespace

std::pair<int, long long> auto_batch(const json& profile) {
  int n = (int)get_config().num("rag.crawl_batch_pages", 0);
  double ram = profile.is_object() && profile.contains("ram_gb") && profile["ram_gb"].is_number() ? profile["ram_gb"].get<double>() : 8;
  if (ram <= 0) ram = 8;
  bool mobile = hw::android();
  int pages = ram < 4 ? 50 : (ram < 8 || mobile) ? 100 : ram < 16 ? 200 : 300;
  long long chars = (ram < 8 || mobile) ? 2000000 : 6000000;
  if (n > 0) pages = n;
  return {std::clamp(pages, 5, crawler::MAX_PAGES_LIMIT), chars};
}

void save(json& job) {
  job["updated"] = now();
  fs::path p = path_of(jstr(job, "id"));
  fs::path tmp = p;
  tmp.replace_extension(".tmp");
  write_file(tmp, job.dump());
  std::error_code ec;
  fs::rename(tmp, p, ec);   // atomico: un corte a mitad no deja el trabajo corrupto
}

json get(const std::string& id) {
  fs::path p;
  try {
    p = path_of(id);
  } catch (...) {
    return nullptr;
  }
  auto txt = read_file(p);
  if (!txt) return nullptr;
  json job = jparse(*txt);
  if (!job.is_object()) return nullptr;
  if (jstr(job, "status") == RUNNING && !is_running(id)) job["status"] = INTERRUPTED;   // se cerro a mitad: se puede reanudar
  return job;
}

json create(const std::string& url, long long max_pages, long long max_depth, const std::string& scope,
            const std::string& agent, bool learn, int batch_pages, long long batch_chars,
            bool queued) {
  bool complete = scope == "domain";
  json job = {{"id", strftime_local("%Y%m%d%H%M%S-") + token_hex(3)},
              {"url", url},
              {"start", ""},
              {"prefix", ""},
              {"scope", scope},
              {"agent", agent},
              {"learn", learn},
              {"max_pages", complete ? UNLIMITED : std::clamp<long long>(max_pages, 1, MAX_DOMAIN_PAGES)},
              {"max_depth", complete ? 1000000LL : std::max<long long>(0, max_depth)},
              {"complete", complete},
              {"batch_pages", batch_pages},
              {"batch_chars", batch_chars},
              {"status", queued ? QUEUED : RUNNING},
              {"reason", ""},
              {"pages_done", 0},
              {"fetched", 0},
              {"skipped", 0},
              {"pending", 0},
              {"batches", json::array()},
              {"title", ""},
              {"site", ""},
              {"state", nullptr},
              {"created", now()},
              {"seconds", 0.0},
              {"error", ""}};
  save(job);
  return job;
}

json pub(const json& job) {
  json out = json::object();
  for (auto& [k, v] : job.items())
    if (k != "state") out[k] = v;
  const json& st = job.value("state", json());
  out["pending"] = st.is_object() ? json(st.value("queue", json::array()).size()) : job.value("pending", json(0));
  out["running"] = is_running(jstr(job, "id"));
  return out;
}

json list_jobs() {
  std::vector<fs::path> files = glob(dir(), "*.json");
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return S(a) > S(b); });
  json out = json::array();
  for (auto& p : files) {
    json j = get(S(p.stem()));
    if (!j.is_null()) out.push_back(pub(j));
  }
  return out;
}

fs::path kg_path(const std::string& id) {
  fs::path p = path_of(id);
  p.replace_extension(".kg.sqlite");
  return p;
}

fs::path knowledge_path(const std::string& id) {
  fs::path p = path_of(id);
  p.replace_extension(".conocimiento.zip");
  return p;
}

bool stop(const std::string& id) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_running.find(id);
    if (it != g_running.end()) {
      it->second->store(true);
      return true;
    }
  }
  json job = get(id);   // aun en la cola: se saca de ella y queda en pausa (REANUDAR lo rastrea enseguida)
  if (job.is_null() || jstr(job, "status") != QUEUED) return false;
  {
    std::lock_guard<std::mutex> lk(g_qmu);
    g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), id), g_queue.end());
  }
  job["status"] = PAUSED;
  job["reason"] = "sacado de la cola";
  save(job);
  return true;
}

bool is_running(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_running.count(id) > 0;
}

std::shared_ptr<std::atomic<bool>> claim(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_running.count(id)) throw std::runtime_error("ese rastreo ya esta en marcha");
  auto ev = std::make_shared<std::atomic<bool>>(false);
  g_running[id] = ev;
  return ev;
}

void release(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_running.erase(id);
}

bool remove(Services& s, const std::string& id, bool docs) {
  stop(id);
  json job = get(id);
  if (job.is_null()) return false;
  if (docs)
    for (auto& b : job["batches"]) s.rag->remove(b["doc_id"].get<long long>());
  remove_file(path_of(id));
  for (auto& extra : {kg_path(id), knowledge_path(id)})
    for (auto& f : {extra, fs::path(S(extra) + "-wal"), fs::path(S(extra) + "-shm")}) remove_file(f);
  return true;
}

json job_result(const json& job) {
  const json& b = job["batches"];
  json first = b.empty() ? json::object() : b[0];
  long long words = 0, chunks = 0, learned = 0;
  json titles = json::array(), ids = json::array(), lotes = json::array();
  for (size_t i = 0; i < b.size(); ++i) {
    words += b[i].value("words", 0LL);
    chunks += b[i].value("chunks", 0LL);
    learned += b[i].value("learned", 0LL);
    if (i < 40) titles.push_back(b[i]["title"]);
    ids.push_back(b[i]["doc_id"]);
    json l = json::object();
    for (auto* k : {"n", "doc_id", "title", "pages", "page_from", "page_to"}) l[k] = b[i][k];
    lotes.push_back(l);
  }
  const json& st = job.value("state", json());
  return {{"title", job["title"]},
          {"start", job["start"]},
          {"scope", job["scope"]},
          {"prefix", job["prefix"]},
          {"site", job["site"]},
          {"pages", job["pages_done"]},
          {"fetched", job["fetched"]},
          {"pending", st.is_object() ? st.value("queue", json::array()).size() : 0},
          {"robots", true},
          {"words", words},
          {"page_titles", titles},
          {"skipped", json::array()},
          {"skipped_total", job["skipped"]},
          {"rag_doc_id", first.value("doc_id", json())},
          {"rag_doc_ids", ids},
          {"rag_chunks", chunks},
          {"replaced", job.value("replaced", json::array())},
          {"summary", first.value("summary", "")},
          {"key_points", first.value("points", json::array())},
          {"learned", learned},
          {"url", b.empty() ? "" : "/api/rag/" + first["doc_id"].dump() + "/file"},
          {"seconds", job["seconds"]},
          {"agent", jstr(job, "agent")},
          {"batches", b.size()},
          {"job", job["id"]},
          {"status", job["status"]},
          {"reason", jstr(job, "reason")},
          {"max_pages", job["max_pages"]},
          {"batch_pages", job["batch_pages"]},
          {"lotes", lotes}};
}

json run_batches(Services& s, json job, const Emit& emit, const std::function<bool()>& should_stop, double max_seconds,
                 bool replace) {
  auto step = [&](const std::string& t) {
    if (emit) emit({{"type", "progress"}, {"text", t}});
  };
  const double batch_seconds = 900;
  double t0 = now() - job.value("seconds", 0.0), began = now();
  std::string agent = jstr(job, "agent");
  auto stopf = should_stop ? should_stop : [] { return false; };
  std::string id = job["id"];
  while (job["pages_done"].get<long long>() < job["max_pages"].get<long long>()) {
    if (stopf()) {
      job["status"] = PAUSED;
      job["reason"] = "pausado por el usuario";
      break;
    }
    double left = max_seconds < 0 ? -1 : max_seconds - (now() - began);
    if (max_seconds >= 0 && left <= 5) {
      job["status"] = PAUSED;
      job["reason"] = "tiempo agotado: reanudalo para seguir";
      break;
    }
    int n = (int)job["batches"].size() + 1;
    long long done = job["pages_done"], maxp = job["max_pages"];
    long long want = std::min<long long>(job["batch_pages"].get<long long>(), maxp - done);
    step("Lote " + std::to_string(n) + ": rastreando hasta " + std::to_string(want) + " páginas (llevas " +
         std::to_string(done) + (job.value("complete", false) ? " de todo el dominio)" : " de " + std::to_string(maxp) + ")"));
    crawler::CrawlOpts o;
    o.max_pages = (int)want;
    o.max_depth = (int)std::min<long long>(job["max_depth"].get<long long>(), 1000000);
    o.scope = jstr(job, "scope");
    o.emit = emit;
    o.max_seconds = left >= 0 ? std::min(batch_seconds, left) : batch_seconds;
    o.state = job["state"];
    o.max_chars = job["batch_chars"];
    o.should_stop = stopf;
    o.offset = done;
    o.total = maxp;
    o.seed_limit = std::min<long long>(maxp * 2, 1000000);
    json res = crawler::crawl(jstr(job, "url"), o);
    job["start"] = res["start"];
    job["prefix"] = res["prefix"];
    job["site"] = res["site"];
    job["fetched"] = job["fetched"].get<long long>() + res["fetched"].get<long long>();
    long long sk = 0;
    for (auto& e : res["skipped"]) {
      std::string r = e[1];
      if (r != "tiempo agotado" && r != "detenido" && r != "limite de texto alcanzado") ++sk;
    }
    job["skipped"] = job["skipped"].get<long long>() + sk;
    const json& pages = res["pages"];
    if (!pages.empty()) {
      if (jstr(job, "title").empty()) job["title"] = crawler::site_title(res);
      long long first = done + 1, last = done + (long long)pages.size();
      char nn[8];
      std::snprintf(nn, sizeof nn, "%03d", n);
      std::string title = (maxp > job["batch_pages"].get<long long>() || n > 1)
                              ? uleft(jstr(job, "title") + " · lote " + nn + " (págs. " + std::to_string(first) + "-" +
                                          std::to_string(last) + ")",
                                      200)
                              : jstr(job, "title");
      step("Lote " + std::to_string(n) + ": indexando " + std::to_string(pages.size()) + " páginas en la biblioteca RAG");
      json out = crawler::ingest_batch(s, res, title, agent, job.value("learn", true), n == 1,
                                       {{"job", id},
                                        {"batch", n},
                                        {"page_from", first},
                                        {"page_to", last},
                                        {"max_pages", maxp},
                                        {"max_depth", job["max_depth"]},
                                        {"suffix", std::string("_lote") + nn}});
      if (job.value("complete", false)) {   // dominio completo: cada lote alimenta su grafo multicapa (en disco)
        step("Lote " + std::to_string(n) + ": añadiendo al grafo del dominio");
        domain_kg::DomainGraph(kg_path(id)).add_pages(pages);
      }
      long long words = 0;
      for (auto& p : pages) words += p.value("words", 0LL);
      job["batches"].push_back({{"n", n},
                                {"doc_id", out["doc"]["id"]},
                                {"title", title},
                                {"pages", pages.size()},
                                {"page_from", first},
                                {"page_to", last},
                                {"chunks", out["doc"]["chunks"]},
                                {"words", words},
                                {"learned", out["learned"]},
                                {"summary", n == 1 ? out["summary"] : json("")},
                                {"first_url", pages.front()["url"]},
                                {"last_url", pages.back()["url"]},
                                {"points", n == 1 ? out["points"] : json::array()}});
      job["pages_done"] = last;
      if (n == 1 && replace) {
        job["replaced"] = crawler::replace_old(s, jstr(res, "start"), jstr(res, "prefix"), agent,
                                               {out["doc"]["id"].get<long long>()});
        for (auto& other : list_jobs())   // trabajos anteriores de la misma documentacion
          if (jstr(other, "id") != id && jstr(other, "start") == jstr(res, "start") &&
              jstr(other, "prefix") == jstr(res, "prefix") && jstr(other, "agent") == agent && !other.value("running", false))
            remove(s, other["id"], false);
      }
    }
    job["state"] = res["state"];
    job["pending"] = res["pending"];
    job["seconds"] = round_to(now() - t0, 1);
    if (res["state"].value("done", false)) break;
    std::string stopped = res["stopped"];
    if (pages.empty() && stopped != "tiempo agotado" && stopped != "detenido") break;   // nada nuevo en el ambito
    save(job);
    if (emit && !pages.empty()) emit({{"type", "crawl_job"}, {"job", id}, {"status", job["status"]}, {"batch", n}});
  }
  const json& st = job["state"].is_object() ? job["state"] : json::object();
  bool queue_empty = st.value("queue", json::array()).empty();
  if (jstr(job, "status") == RUNNING) {
    if (st.value("done", false) || job["pages_done"].get<long long>() >= job["max_pages"].get<long long>() || queue_empty) {
      job["status"] = DONE;
      job["reason"] = st.value("done", false) || queue_empty
                          ? "sitio completo"
                          : "alcanzado el máximo de " + job["max_pages"].dump() + " páginas";
    } else {
      job["status"] = PAUSED;
      if (jstr(job, "reason").empty()) job["reason"] = "detenido";
    }
  }
  if (job["batches"].empty() && jstr(job, "status") == DONE) {
    job["status"] = FAILED;
    job["error"] = "no se ha podido leer ninguna pagina con texto";
  } else if (jstr(job, "status") == DONE && job.value("complete", false)) {
    try {
      build_knowledge(s, job, emit);
    } catch (const std::exception& e) {
      L.warning("Grafo del dominio ", id, " no construido: ", e.what());
      job["knowledge_error"] = uleft(e.what(), 300);
    }
  }
  job["seconds"] = round_to(now() - t0, 1);
  save(job);
  L.info("Rastreo por lotes ", id, " (", jstr(job, "start").empty() ? jstr(job, "url") : jstr(job, "start"), "): ",
         job["batches"].size(), " lotes, ", job["pages_done"].dump(), " paginas, ", jstr(job, "status"));
  if (emit) emit({{"type", "crawl_job"}, {"job", id}, {"status", job["status"]}});
  return job;
}

namespace {

// rastrea un trabajo ya reservado con claim() en el hilo que llama; libera la reserva al terminar
void run_claimed(Services& s, const std::string& id, const Emit& emit, const std::shared_ptr<std::atomic<bool>>& ev) {
  try {
    json job = get(id);
    if (job.is_null()) throw std::out_of_range(id);
    if (jstr(job, "status") != DONE) {
      job["status"] = RUNNING;
      job["reason"] = "";
      job["error"] = "";
      save(job);
      run_batches(s, job, emit, [ev] { return ev->load(); });
    }
  } catch (const std::exception& e) {
    L.warning("Rastreo ", id, " fallido: ", e.what());
    json job = get(id);
    if (!job.is_null()) {
      job["status"] = FAILED;
      job["error"] = uleft(e.what(), 300);
      save(job);
    }
  }
  release(id);
  if (emit) {
    emit({{"type", "crawl_job"}, {"job", id}});
    emit({{"type", "graph_update"}});
  }
}

int max_workers() { return std::clamp((int)get_config().num("rag.crawl_parallel", 1), 1, 4); }

// hilo de la cola: saca trabajos por orden hasta vaciarla
void queue_worker(Services* sp, Emit emit) {
  for (;;) {
    std::string id;
    {
      std::lock_guard<std::mutex> lk(g_qmu);
      if (g_queue.empty()) {
        --g_workers;
        return;
      }
      id = g_queue.front();
      g_queue.pop_front();
    }
    json job = get(id);
    if (job.is_null() || jstr(job, "status") != QUEUED) continue;   // borrado, sacado de la cola o reanudado a mano
    std::shared_ptr<std::atomic<bool>> ev;
    try {
      ev = claim(id);
    } catch (...) {
      continue;   // ya en marcha por otro camino
    }
    job = get(id);
    if (job.is_null() || jstr(job, "status") != QUEUED) {   // lo sacaron de la cola justo ahora
      release(id);
      continue;
    }
    run_claimed(*sp, id, emit, ev);
  }
}

}  // namespace

void run_background(Services& s, const std::string& id, const Emit& emit) {
  auto ev = claim(id);   // antes de arrancar el hilo: la lista ya lo muestra en curso
  Services* sp = &s;
  std::thread([sp, id, emit, ev] { run_claimed(*sp, id, emit, ev); }).detach();
}

void enqueue(Services& s, const std::vector<std::string>& ids, const Emit& emit) {
  std::lock_guard<std::mutex> lk(g_qmu);
  for (auto& id : ids)
    if (std::find(g_queue.begin(), g_queue.end(), id) == g_queue.end()) g_queue.push_back(id);
  int limit = max_workers();
  while (g_workers < limit && (size_t)g_workers < g_queue.size()) {
    ++g_workers;
    std::thread(queue_worker, &s, emit).detach();
  }
}

size_t queued_count() {
  std::lock_guard<std::mutex> lk(g_qmu);
  return g_queue.size();
}

std::vector<std::string> parse_url_list(const std::string& text, std::vector<std::string>* invalid) {
  static const Rx URL(R"(^https?://[^\s/?#]+\.[^\s/?#]*[^\s]*$)", Rx::I),
      HOST(R"(^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*\.[a-z]{2,}(?::\d+)?(?:[/?#]\S*)?$)", Rx::I),
      LOCAL(R"(^https?://(?:localhost|\[[0-9a-f:]+\])(?::\d+)?(?:[/?#]\S*)?$)", Rx::I);
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (auto& raw : splitlines(text)) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
    for (char& ch : line)
      if (ch == ',' || ch == ';' || ch == '\t') ch = ' ';
    for (auto& tok0 : split_ws(line)) {
      std::string tok = trim(tok0, "<>\"'()[]{}");
      if (tok.empty()) continue;
      std::string url = tok;
      if (!re_search(R"(^[a-z][a-z0-9+.-]*://)", url, Rx::I) && HOST.fullmatch(url)) url = "https://" + url;
      if (!URL.fullmatch(url) && !LOCAL.fullmatch(url)) {
        if (invalid) invalid->push_back(tok);
        continue;
      }
      std::string key = crawler::normalize(url);   // «/guia» y «/guia/» son la misma entrada de la lista
      if (key.find('?') == std::string::npos) key = rtrim(key, "/");
      if (seen.insert(key).second) out.push_back(url);
    }
  }
  return out;
}

json build_knowledge(Services& s, json& job, const Emit& emit) {
  auto step = [&](const std::string& t) {
    if (emit) emit({{"type", "progress"}, {"text", t}});
  };
  std::string id = job["id"];
  domain_kg::DomainGraph g(kg_path(id));
  std::function<std::string(const std::string&)> llm;
  try {
    if (s.llm->has_real_llm())
      llm = [&s](const std::string& prompt) {
        llm::Opts o;
        o.o = {{"max_tokens", 700}, {"temperature", 0.3}};
        return s.llm->complete(prompt, "default", o);
      };
  } catch (...) {
  }
  std::vector<long long> exclude;
  for (auto& b : job.value("batches", json::array())) exclude.push_back(b["doc_id"].get<long long>());
  json built = g.build(jstr(job, "title"), jstr(job, "site"), llm, &s, exclude, step,
                       (int)s.config.num("rag.domain_kg_llm_topics", 40));
  g.package(knowledge_path(id), built);
  json kn = built["manifest"];
  kn["file"] = S(knowledge_path(id).filename());
  kn["built"] = now();
  job["knowledge"] = kn;
  save(job);
  step("Grafo del dominio listo: " + built["manifest"]["nodes"].dump() + " nodos, " + built["manifest"]["facts"].dump() +
       " afirmaciones");
  return kn;
}

std::vector<std::string> resume_interrupted(Services& s, const Emit& emit) {
  std::vector<std::string> out;
  try {
    if (!truthy(s.config.get("rag.auto_resume_crawls", true))) return out;
    std::vector<std::string> queue;   // listas de URLs: lo que quedo en la cola (y el que estaba a medias) vuelve a ella
    json jobs = list_jobs();
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {   // los mas antiguos primero: el orden de la lista
      json& j = *it;
      if (j.value("running", false)) continue;
      std::string st = jstr(j, "status");
      if (st == QUEUED || (st == INTERRUPTED && !jstr(j, "list").empty())) {
        if (st == INTERRUPTED) {
          json full = get(j["id"]);
          full["status"] = QUEUED;
          save(full);
        }
        queue.push_back(j["id"]);
      } else if (j.value("complete", false) && st == INTERRUPTED) {
        run_background(s, j["id"], emit);
        out.push_back(j["id"]);
      }
    }
    if (!queue.empty()) {   // orden de la lista: (lista, posicion)
      std::map<std::string, std::pair<std::string, long long>> key;
      for (auto& id : queue) {
        json j = get(id);
        key[id] = {jstr(j, "list"), j.value("list_pos", 0LL)};
      }
      std::stable_sort(queue.begin(), queue.end(), [&](const std::string& a, const std::string& b) { return key[a] < key[b]; });
      enqueue(s, queue, emit);
    }
    out.insert(out.end(), queue.begin(), queue.end());
  } catch (const std::exception& e) {
    L.warning("No se pudieron reanudar los rastreos: ", e.what());
  }
  return out;
}

}  // namespace nx::rag::crawl_jobs
