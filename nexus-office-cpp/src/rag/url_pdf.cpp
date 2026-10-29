// URL -> extraccion estructurada -> resumen -> PDF maquetado -> biblioteca RAG -> conocimiento (rag/url_pdf.py).
//
// 1. Descarga la pagina (solo direcciones publicas, maximo 8 MB).
// 2. Extrae metadatos, secciones, listas, tablas, codigo, imagenes y enlaces.
// 3. Resume y obtiene puntos clave (con el LLM si lo hay; si no, resumen extractivo).
// 4. Maqueta el PDF y lo publica en data/outputs/docs (enlace para el usuario).
// 5. Guarda una copia en la biblioteca RAG (troceada e indexada).
// 6. Asimila el contenido: resumen y puntos clave a la memoria y entidades/relaciones al grafo.
#include <algorithm>
#include <map>

#include "core/config.h"
#include "core/services.h"
#include "llm/llm.h"
#include "memory/memory.h"
#include "rag/browser.h"
#include "rag/rag.h"
#include "rag/web.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag {

namespace {

auto L = log::get("url_pdf");
const http::Headers UA = {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Nexus-Office/1.7"},
                          {"Accept-Language", "es,en;q=0.8"}};

std::string charset_of(const std::string& ctype) {
  static const Rx CS(R"(charset=["']?([\w-]+))", Rx::I);
  auto m = CS.search(ctype);
  return m ? lower(m->str(1)) : "";
}

}  // namespace

std::string decode_body(const std::string& raw, const std::string& ctype) {
  std::string enc = charset_of(ctype);
  if (enc == "iso-8859-1" || enc == "latin-1" || enc == "latin1") return latin1_to_utf8(raw);
  if (enc == "windows-1252" || enc == "cp1252") return cp1252_to_utf8(raw);
  if (enc == "utf-16" || enc == "utf-16le") return utf16_to_utf8(raw, true);
  if (enc == "utf-16be") return utf16_to_utf8(raw, false);
  return valid_utf8(raw);
}

std::string check_public_url(const std::string& url) {
  auto u = http::parse_url(url);
  if ((u.scheme != "http" && u.scheme != "https") || u.host.empty())
    throw std::invalid_argument("solo se permiten URLs http(s)");
  if (truthy(get_config().get("rag.allow_private_urls", false))) return url;   // intranet permitida por el administrador
  auto ips = net::resolve(u.host);
  if (ips.empty()) throw std::invalid_argument("no se pudo resolver " + u.host);
  for (auto& ip : ips)
    if (net::is_private_ip(ip)) throw std::invalid_argument("no se permite acceder a direcciones de red locales");
  return url;
}

// GET con redirecciones a mano (cada destino se comprueba) y tope de bytes: (estado, tipo, cuerpo, url final)
std::tuple<int, std::string, std::string, std::string> fetch_limited(const std::string& url_in, size_t cap,
                                                                     const http::Headers& headers, double timeout) {
  std::string url = check_public_url(url_in);
  http::Response r;
  std::string data;
  for (int hop = 0; hop < 10; ++hop) {
    data.clear();
    http::Request req;
    req.url = url;
    req.headers = headers;
    req.timeout = timeout;
    req.follow = false;
    req.on_data = [&](const char* p, size_t n) {
      if (r.status && r.status != 200) return true;
      data.append(p, n);
      return data.size() <= cap;
    };
    r = http::send(req);
    if (r.status == 0 && data.size() <= cap) throw std::runtime_error(r.error.empty() ? "error de red" : r.error);
    if (r.status >= 300 && r.status < 400 && !r.header("location").empty()) {
      url = check_public_url(urljoin(url, r.header("location")));
      continue;
    }
    break;
  }
  if (data.size() > cap) data.resize(cap);
  return {r.status, r.header("content-type"), data, url};
}

std::pair<std::string, std::vector<std::string>> extractive_summary(const std::string& text, size_t n) {
  static const Rx HEAD(R"(#+ .*\n)"), SPLIT(R"((?<=[.!?])\s+)");
  std::vector<std::string> sents;
  for (auto& s : SPLIT.split(HEAD.sub(text, ""))) {
    std::string t = trim(s);
    if (ulen(t) > 40 && ulen(t) < 400) sents.push_back(t);
  }
  if (sents.empty()) return {uleft(text, 600), {}};
  std::map<std::string, int> freq;
  for (auto& t : tokens(text))
    if (!STOP_WORDS.count(t) && ulen(t) > 3) ++freq[t];
  std::vector<double> score(sents.size());
  for (size_t i = 0; i < sents.size(); ++i) {
    auto tk = tokens(sents[i]);
    double sum = 0;
    for (auto& t : tk) {
      auto it = freq.find(t);
      if (it != freq.end()) sum += it->second;
    }
    score[i] = -sum / (tk.size() + 5);
  }
  std::vector<size_t> idx(sents.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return score[a] < score[b]; });
  std::vector<size_t> top(idx.begin(), idx.begin() + std::min(n, idx.size()));
  std::sort(top.begin(), top.end());
  std::vector<std::string> sel, points;
  for (size_t i : top) sel.push_back(sents[i]);
  for (size_t i = n; i < idx.size() && i < n + 5; ++i) points.push_back(sents[idx[i]]);
  return {join(sel, " "), points};
}

std::optional<std::pair<std::string, std::vector<std::string>>> llm_summary(Services& s, const json& doc) {
  if (!s.llm->has_real_llm()) return std::nullopt;
  std::string raw;
  try {
    llm::Opts o;
    o.o = {{"max_tokens", 380}, {"temperature", 0.2}};
    raw = s.llm->complete("Titulo: " + jstr(doc, "title") + "\n\nContenido:\n" + uleft(jstr(doc, "text"), 3500) +
                              "\n\nEscribe en espanol:\nRESUMEN: un parrafo de 3-4 frases.\nPUNTOS:\n- 5 puntos clave, uno por linea.",
                          "default", o);
  } catch (const std::exception& e) {
    L.warning("Resumen con LLM fallido: ", e.what());
    return std::nullopt;
  }
  raw = replace_all(replace_all(raw, "**", ""), "__", "");
  static const Rx PUNTOS(R"(\bPUNTOS(?: CLAVE)?\s*:?)", Rx::I), RES(R"(^\s*RESUMEN\s*:?\s*)", Rx::I),
      ITEMS(R"((?:^|\n|\s)(?:[-•*]\s*)?\d+[.)]\s+|(?:^|\n)\s*[-•*]\s+|\s-\s)");
  auto parts = PUNTOS.split(raw, 1);
  std::string summary = trim(RES.sub(parts[0], ""));
  std::vector<std::string> points;
  if (parts.size() > 1)
    for (auto& p : ITEMS.split(parts[1])) {
      std::string t = strip_u(p, U" .-•*\n");
      if (ulen(t) > 8 && points.size() < 7) points.push_back(t);
    }
  if (ulen(summary) <= 40) return std::nullopt;
  return std::make_pair(summary, points);
}

int assimilate(Services& s, const json& doc, const std::string& summary, const std::vector<std::string>& points,
               long long doc_id, const std::string& url, const std::string& agent) {
  auto& mem = *s.memory;
  auto& graph = s.graph->for_agent(agent);
  std::string src = "rag:" + std::to_string(doc_id), dtitle = jstr(doc, "title");
  int n = 0;
  mem.remember("Documento «" + dtitle + "» (" + url + "): " + summary, "summary", 0.75, src, agent);
  ++n;
  for (auto& p : points) {
    mem.remember(p + " (según «" + uleft(dtitle, 80) + "»)", "fact", 0.6, src, agent);
    ++n;
  }
  std::string title = uleft(dtitle, 60);
  graph.upsert_entity(title, "documento", 2);
  graph.relate(graph.self_name, "ha_leido", title, graph.self_type, "documento", 1.0);
  if (!jstr(doc, "site").empty()) graph.relate(title, "publicado_en", jstr(doc, "site"), "documento", "organizacion", 0.8);
  if (!jstr(doc, "author").empty())
    graph.relate(title, "escrito_por", uleft(jstr(doc, "author"), 60), "documento", "persona", 0.8);
  std::vector<std::string> heads;
  for (auto& h : doc.value("headings", json::array())) heads.push_back(h.get<std::string>());
  std::vector<std::string> h15(heads.begin(), heads.begin() + std::min<size_t>(15, heads.size()));
  for (auto& kw : mem::KnowledgeGraph::keywords(dtitle + ". " + summary + " " + join(h15, " "), 8)) {
    graph.relate(title, "trata_de", kw, "documento", "concepto", 0.6);
    ++n;
  }
  for (size_t i = 0; i < heads.size() && i < 10; ++i)
    if (ulen(heads[i]) > 3 && ulen(heads[i]) < 60) graph.relate(title, "contiene", heads[i], "documento", "concepto", 0.3);
  graph.extract(summary, "assistant");
  return n;
}

json process_url(Services& s, const std::string& url, const Emit& emit, int max_images, const std::string& agent) {
  double t0 = now();
  auto step = [&](const std::string& t) {
    if (emit) emit({{"type", "progress"}, {"text", t}});
  };
  step("Descargando la pagina");
  auto [status, ctype, body, final_url] = fetch_limited(url, 8000000, UA, 30);
  if (status < 200 || status >= 300) throw std::invalid_argument("la pagina respondio HTTP " + std::to_string(status));
  std::string lc = lower(ctype);
  if (!contains(lc, "html") && !contains(lc, "text") && !contains(lc, "xml"))
    throw std::invalid_argument("la URL no es una pagina web (" + (ctype.empty() ? std::string("tipo desconocido") : ctype) + ")");
  std::string raw = decode_body(body, ctype);
  if (contains(lc, "html")) raw = browser::maybe_render(final_url, raw).first;   // webs que se pintan con JavaScript
  step("Extrayendo estructura");
  json doc = extract_html(raw, final_url);
  if (doc.value("words", 0) < 30)
    throw std::invalid_argument(!browser::find().empty()
                                    ? "la pagina apenas contiene texto (puede requerir inicio de sesion)"
                                    : "la pagina apenas contiene texto: necesita JavaScript e instala Chrome, Edge o "
                                      "Chromium para leerla");
  step("Resumiendo");
  auto summ = llm_summary(s, doc);
  auto [summary, points] = summ ? *summ : extractive_summary(jstr(doc, "text"));
  step("Maquetando el PDF");
  std::string slug = trim(uleft(re_sub("[^a-z0-9]+", "-", lower(jstr(doc, "title"))), 50), "-");
  if (slug.empty()) slug = "documento";
  fs::path out = paths::outputs_dir() / "docs" / P(std::to_string((long long)now()) + "_" + slug + ".pdf");
  json info = build_pdf(doc, summary, points, out, max_images);
  step("Archivando en la biblioteca RAG");
  json rag_doc = s.rag->ingest_file(out, jstr(doc, "title"), final_url,
                                    {{"origin", "url_to_pdf"}, {"site", jstr(doc, "site")}, {"summary", summary}}, true, agent,
                                    false);
  step("Incorporando al conocimiento");
  int learned = assimilate(s, doc, summary, points, rag_doc["id"].get<long long>(), final_url, agent);
  return {{"title", jstr(doc, "title")},
          {"url", "/outputs/docs/" + S(out.filename())},
          {"source", final_url},
          {"pages", info["pages"]},
          {"sections", doc["sections"].size()},
          {"words", doc["words"]},
          {"images", doc["images"].size()},
          {"images_embedded", info["images_embedded"]},
          {"links", doc["links"].size()},
          {"summary", summary},
          {"key_points", points},
          {"rag_doc_id", rag_doc["id"]},
          {"rag_chunks", rag_doc["chunks"]},
          {"learned", learned},
          {"seconds", round_to(now() - t0, 1)},
          {"agent", agent}};
}

}  // namespace nx::rag
