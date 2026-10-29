// Rastreo (crawling) de un sitio de documentacion -> biblioteca RAG -> conocimiento (rag/crawler.py).
//
// Parte de una URL y recorre en anchura las paginas enlazadas del mismo sitio:
// - Ambito: por defecto la "seccion" de la URL inicial (https://docs.x.org/3/library/ -> solo /3/library/...);
//   con scope="domain", TODO el dominio: el host, www. y sus subdominios, sin limite de paginas ni de profundidad,
//   leyendo tambien los documentos enlazados (PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, texto) y las
//   paginas cortas. Con el se construye ademas un grafo de conocimiento propio del dominio (domain_kg.cpp).
// - Respeta robots.txt (Disallow y Crawl-delay) y usa el sitemap.xml como semilla si existe.
// - Solo direcciones publicas (tambien tras redirecciones); no sale del ambito aunque redirija.
// - Limites: paginas, profundidad, tiempo, 3 MB por pagina y 6 millones de caracteres en total.
// - Rapido: varias descargas a la vez, aplicando los resultados en orden para que sea reproducible.
//
// Todo el sitio se indexa como un unico documento "web" (una pagina del documento por pagina rastreada, con la URL
// delante de cada fragmento); ademas se guarda una copia en Markdown y se asimila un resumen a la memoria y al grafo.
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/config.h"
#include "core/services.h"
#include "rag/rag.h"
#include "rag/browser.h"
#include "rag/web.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::rag::crawler {

namespace {

auto L = log::get("crawler");
const char* UA_NAME = "Nexus-Office";
const http::Headers UA = {{"User-Agent", "Mozilla/5.0 (compatible; Nexus-Office/1.0; documentation reader)"},
                          {"Accept-Language", "es,en;q=0.8"}};
constexpr size_t MAX_PAGE_BYTES = 3000000, MAX_DOC_BYTES = 60000000;
constexpr long long MAX_TOTAL_CHARS = 6000000;
constexpr double MAX_CRAWL_DELAY = 5.0;
constexpr int MIN_WORDS = 30, MIN_WORDS_DOMAIN = 5;
const std::set<std::string> DOC_EXT = {".pdf", ".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx",
                                       ".epub", ".rtf", ".txt", ".csv", ".md"};
const std::vector<std::pair<std::string, std::string>> DOC_CTYPES = {
    {"application/pdf", ".pdf"},          {"wordprocessingml", ".docx"},          {"spreadsheetml", ".xlsx"},
    {"presentationml", ".pptx"},          {"opendocument.text", ".odt"},          {"opendocument.spreadsheet", ".ods"},
    {"opendocument.presentation", ".odp"}, {"epub", ".epub"},                     {"rtf", ".rtf"},
    {"text/plain", ".txt"},               {"text/csv", ".csv"},                   {"text/markdown", ".md"}};
const std::set<std::string> SLD = {"co", "com", "org", "net", "gov", "gob", "edu", "ac", "nhs", "ltd", "plc", "or", "ne", "go"};
const std::set<std::string> SKIP_EXT = {
    ".png", ".jpg", ".jpeg", ".gif",  ".webp", ".svg", ".ico", ".bmp",  ".tif", ".tiff", ".avif", ".css", ".js",
    ".mjs", ".map", ".json", ".xml",  ".rss",  ".atom", ".woff", ".woff2", ".ttf", ".otf", ".eot", ".zip", ".gz",
    ".tgz", ".bz2", ".xz",   ".7z",   ".rar",  ".tar", ".whl", ".exe",  ".msi", ".dmg", ".pkg", ".deb", ".rpm",
    ".apk", ".jar", ".iso",  ".bin",  ".pdf",  ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".mp3",
    ".mp4", ".wav", ".ogg",  ".webm", ".avi",  ".mov", ".mkv", ".txt", ".csv", ".epub"};
const Rx SKIP_PATH(R"((^|/)(_sources|_static|_images|_downloads|static|assets|search|genindex|login|logout|signin|)"
                   R"(signup|register|account|cart|edit|print|feed)(/|\.html?$|$))",
                   Rx::I);
const Rx SKIP_QUERY(R"((^|&)(action=(edit|history|raw)|q=|query=|search=|share=|print=|replytocom=|oldid=|diff=))", Rx::I);
const Rx TRACKING(R"(^(utm_.*|fbclid|gclid|mc_cid|mc_eid|ref|ref_src|_ga|_gl)$)", Rx::I);

// ---------------------------------------------------------------- urlparse
struct U {
  std::string scheme, netloc, path, query, fragment;
};

U urlparse(const std::string& url) {
  U u;
  std::string rest = url;
  static const Rx SCH(R"(^([A-Za-z][A-Za-z0-9+.-]*):)");
  if (auto m = SCH.search(rest)) {
    u.scheme = lower(m->str(1));
    rest = rest.substr(m->end());
  }
  if (auto h = rest.find('#'); h != std::string::npos) {
    u.fragment = rest.substr(h + 1);
    rest = rest.substr(0, h);
  }
  if (auto q = rest.find('?'); q != std::string::npos) {
    u.query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  if (starts_with(rest, "//")) {
    size_t sl = rest.find('/', 2);
    u.netloc = rest.substr(2, sl == std::string::npos ? std::string::npos : sl - 2);
    u.path = sl == std::string::npos ? "" : rest.substr(sl);
  } else {
    u.path = rest;
  }
  return u;
}

std::string unparse(const U& u) {
  std::string s = u.scheme.empty() ? "" : u.scheme + ":";
  if (!u.netloc.empty() || u.scheme == "http" || u.scheme == "https") s += "//" + u.netloc;
  s += u.path;
  if (!u.query.empty()) s += "?" + u.query;
  return s;
}

std::string unquote_plus(const std::string& s) { return url_decode(replace_all(s, "+", " ")); }

std::string host_of(const std::string& netloc) {
  std::string h = lower(netloc);
  if (auto at = h.rfind('@'); at != std::string::npos) h = h.substr(at + 1);
  if (starts_with(h, "[")) {
    h = h.substr(1);
    h = h.substr(0, h.find(']'));
  } else {
    h = h.substr(0, h.find(':'));
  }
  return starts_with(h, "www.") ? h.substr(4) : h;
}

std::string suffix_of(const std::string& path) {   // PurePosixPath(path).suffix
  std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return "";
  return lower(name.substr(dot));
}

// ---------------------------------------------------------------- robots.txt (urllib.robotparser)
struct Robots {
  struct Entry {
    std::vector<std::string> agents;
    std::vector<std::pair<std::string, bool>> rules;   // (ruta, permitido)
    double delay = -1;
  };
  std::vector<Entry> entries;
  std::optional<Entry> def;

  void parse(const std::string& text) {
    Entry cur;
    int state = 0;   // 0 nada, 1 agentes, 2 reglas
    auto flush = [&] {
      if (!cur.agents.empty()) {
        if (std::find(cur.agents.begin(), cur.agents.end(), "*") != cur.agents.end()) {
          if (!def) def = cur;
        } else {
          entries.push_back(cur);
        }
      }
      cur = Entry();
    };
    for (auto line : splitlines(text)) {
      if (auto c = line.find('#'); c != std::string::npos) line = line.substr(0, c);
      line = trim(line);
      if (line.empty()) {
        if (state == 1) {
          cur = Entry();
          state = 0;
        } else if (state == 2) {
          flush();
          state = 0;
        }
        continue;
      }
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = lower(trim(line.substr(0, colon))), val = trim(line.substr(colon + 1));
      if (key == "user-agent") {
        if (state == 2) flush();
        cur.agents.push_back(val);
        state = 1;
      } else if (key == "disallow" || key == "allow") {
        if (state != 0) {
          std::string path = url_decode(val);
          cur.rules.push_back({path, key == "allow" || path.empty()});
          state = 2;
        }
      } else if (key == "crawl-delay") {
        if (state != 0) {
          try {
            cur.delay = std::stod(val);
          } catch (...) {
          }
          state = 2;
        }
      }
    }
    if (state == 2) flush();
  }

  static bool applies(const Entry& e, const std::string& ua) {
    std::string name = lower(split(ua, "/")[0]);
    for (auto& a : e.agents) {
      if (a == "*") return true;
      if (contains(name, lower(a))) return true;
    }
    return false;
  }
  static bool allowance(const Entry& e, const std::string& path) {
    for (auto& [p, allow] : e.rules)
      if (p == "*" || starts_with(path, p)) return allow;
    return true;
  }
  bool can_fetch(const std::string& ua, const std::string& url) const {
    U u = urlparse(url);
    std::string path = url_decode(u.path.empty() ? "/" : u.path);
    if (!u.query.empty()) path += "?" + u.query;
    for (auto& e : entries)
      if (applies(e, ua)) return allowance(e, path);
    if (def) return allowance(*def, path);
    return true;
  }
  double crawl_delay(const std::string& ua) const {
    for (auto& e : entries)
      if (applies(e, ua)) return e.delay;
    return def ? def->delay : -1;
  }
};

// ---------------------------------------------------------------- red
Fetcher http_fetcher() {
  return [](const std::string& url_in) -> std::tuple<int, std::string, std::string, std::string> {
    std::string url = check_public_url(url_in);
    for (int hop = 0; hop < 10; ++hop) {
      http::Request req;
      req.url = url;
      req.headers = UA;
      req.timeout = 25;
      req.follow = false;
      std::string data, ctype;
      size_t cap = MAX_PAGE_BYTES;
      int status = 0;
      req.on_headers = [&](const http::Response& r) {
        status = r.status;
        ctype = r.header("content-type");
        bool is_doc = !contains(lower(ctype), "html") && !doc_suffix(url, ctype).empty();
        cap = is_doc ? MAX_DOC_BYTES : MAX_PAGE_BYTES;
        return true;
      };
      req.on_data = [&](const char* p, size_t n) {
        if (status != 200) return true;
        data.append(p, n);
        return data.size() <= cap;
      };
      auto r = http::send(req);
      if (r.status == 0 && data.size() <= cap) throw std::runtime_error(r.error.empty() ? "error de red" : r.error);
      if (r.status >= 300 && r.status < 400 && !r.header("location").empty()) {
        url = check_public_url(urljoin(url, r.header("location")));
        continue;
      }
      ctype = r.header("content-type");
      if (data.size() > cap) data.resize(cap);
      bool is_doc = !contains(lower(ctype), "html") && !doc_suffix(url, ctype).empty();
      if (is_doc) return {r.status, ctype, data, url};
      std::string body = decode_body(data, ctype);
      if (r.status == 200 && contains(lower(ctype), "html"))
        body = browser::maybe_render(url, body).first;   // paginas que se pintan con JavaScript
      return {r.status, ctype, body, url};
    }
    throw std::runtime_error("demasiadas redirecciones");
  };
}

std::pair<std::optional<Robots>, std::vector<std::string>> robots_of(const Fetcher& fetch, const std::string& start) {
  U p = urlparse(start);
  try {
    auto [status, ctype, text, fin] = fetch(p.scheme + "://" + p.netloc + "/robots.txt");
    if (status != 200 || trim(text).empty()) return {std::nullopt, {}};
    Robots rp;
    rp.parse(valid_utf8(text));
    static const Rx SM(R"(^\s*sitemap:\s*(\S+))", Rx::I | Rx::M);
    return {rp, SM.findall(text)};
  } catch (...) {
    return {std::nullopt, {}};
  }
}

std::pair<std::vector<std::string>, std::vector<std::string>> sitemap_urls(const std::string& text) {
  static const Rx LOC(R"(<loc>\s*(.*?)\s*</loc>)", Rx::I | Rx::S), IDX(R"(<sitemapindex)", Rx::I), WS(R"(\s+)");
  std::vector<std::string> locs;
  for (auto& m : LOC.findall(text)) locs.push_back(trim(WS.sub(html_unescape(m), " ")));
  if (IDX.test(text)) return {{}, locs};
  return {locs, {}};
}

std::vector<std::string> sitemap_seeds(const Fetcher& fetch, const std::string& start, const std::vector<std::string>& declared,
                                       size_t limit, int max_files) {
  U p = urlparse(start);
  std::deque<std::string> queue(declared.begin(), declared.end());
  if (queue.empty()) queue.push_back(p.scheme + "://" + p.netloc + "/sitemap.xml");
  std::vector<std::string> pages;
  int read = 0;
  while (!queue.empty() && read < max_files && pages.size() < limit) {
    std::string sm = queue.front();
    queue.pop_front();
    ++read;
    try {
      auto [status, ctype, text, fin] = fetch(sm);
      if (status != 200 || uleft(text, 200).find('<') == std::string::npos) continue;
      auto [locs, children] = sitemap_urls(valid_utf8(text));
      pages.insert(pages.end(), locs.begin(), locs.end());
      queue.insert(queue.end(), children.begin(), children.end());
    } catch (...) {
    }
  }
  if (pages.size() > limit) pages.resize(limit);
  return pages;
}

// ---------------------------------------------------------------- enlaces
struct LinkInfo {
  std::vector<std::string> links;
  std::string canonical;
  bool noindex = false, nofollow = false;
};

LinkInfo parse_links(const std::string& raw, const std::string& url) {
  static const Rx TAG(R"(<(a|link|base|meta)\b([^>]*)>)", Rx::I),
      ATTR(R"x(([A-Za-z_:][-A-Za-z0-9_:.]*)\s*(?:=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))?)x"),
      COMMENTS(R"(<!--.*?-->|<script\b.*?</script>|<style\b.*?</style>)", Rx::I | Rx::S);
  std::string html = COMMENTS.sub(raw, " ");
  LinkInfo out;
  std::vector<std::string> hrefs;
  std::string base;
  for (auto& m : TAG.finditer(html)) {
    std::string tag = lower(m.str(1));
    std::string attrs_s = m.str(2);
    std::map<std::string, std::string> a;
    for (auto& am : ATTR.finditer(attrs_s)) {
      std::string k = lower(am.str(1));
      std::string v = am.has(2) ? am.str(2) : am.has(3) ? am.str(3) : am.str(4);
      if (!a.count(k)) a[k] = html_unescape(v);
    }
    std::string rel = lower(a["rel"]);
    if (tag == "a" && !a["href"].empty() && !contains(rel, "nofollow")) hrefs.push_back(a["href"]);
    else if (tag == "link" && contains(rel, "canonical") && !a["href"].empty()) out.canonical = a["href"];
    else if (tag == "link" && rel == "next" && !a["href"].empty()) hrefs.push_back(a["href"]);
    else if (tag == "base" && !a["href"].empty()) base = a["href"];
    else if (tag == "meta") {
      std::string name = lower(a["name"]);
      if (name == "robots" || name == lower(UA_NAME)) {
        std::string c = lower(a["content"]);
        out.noindex |= contains(c, "noindex") || contains(c, "none");
        out.nofollow |= contains(c, "nofollow") || contains(c, "none");
      }
    }
  }
  std::string b = base.empty() ? url : urljoin(url, base);
  std::set<std::string> seen;
  for (auto h : hrefs) {
    h = trim(h);
    if (h.empty() || starts_with(h, "#") || starts_with(h, "mailto:") || starts_with(h, "javascript:") ||
        starts_with(h, "tel:") || starts_with(h, "data:"))
      continue;
    std::string n = normalize(urljoin(b, h));
    if (seen.insert(n).second) out.links.push_back(n);
  }
  if (!out.canonical.empty()) out.canonical = normalize(urljoin(b, out.canonical));
  return out;
}

std::string fingerprint(const std::string& text) { return sha256_hex(text).substr(0, 40); }

json analyze_page(const std::string& raw, const std::string& url) {
  auto li = parse_links(raw, url);
  json out = {{"links", li.links}, {"canonical", li.canonical}, {"noindex", li.noindex}, {"nofollow", li.nofollow}};
  if (!li.noindex) {
    json doc = extract_html(raw, url);
    json d = json::object();
    for (auto* k : {"title", "text", "words", "headings", "description", "site", "author", "lang"}) d[k] = doc[k];
    out["doc"] = d;
    out["hash"] = fingerprint(jstr(doc, "text"));
  }
  return out;
}

json analyze_document(const std::string& data, const std::string& url, const std::string& ctype) {
  std::string suf = doc_suffix(url, ctype);
  if (suf.empty()) suf = ".txt";
  fs::path tmp = paths::sub({"cache"}) / P("crawl_" + token_hex(6) + suf);
  if (!write_file(tmp, data)) throw std::runtime_error("no se pudo escribir el temporal");
  Doc d;
  try {
    d = read_document(tmp);
  } catch (...) {
    remove_file(tmp);
    throw;
  }
  remove_file(tmp);
  static const Rx WS(R"(\s+)"), URL_IN_TEXT(R"(https?://[^\s<>"'\)\]]+)");
  std::vector<std::string> parts;
  for (auto& p : d.pages)
    if (!trim(p).empty()) parts.push_back(p);
  std::string text = trim(WS.sub(html_unescape(join(parts, "\n\n")), " "));
  U u = urlparse(url);
  std::string name = u.path.substr(u.path.rfind('/') == std::string::npos ? 0 : u.path.rfind('/') + 1);
  std::string title = d.title;
  if (title.empty() || starts_with(title, "tmp") || starts_with(title, "crawl_")) title = name.empty() ? url : name;
  std::vector<std::string> links;
  for (auto& m : URL_IN_TEXT.findall(text)) {
    if (links.size() >= 500) break;
    links.push_back(normalize(rtrim(m, ".,;:")));
  }
  return {{"links", links},
          {"canonical", ""},
          {"noindex", false},
          {"nofollow", false},
          {"doc",
           {{"title", title + " (" + upper(suf.substr(1)) + ")"},
            {"text", text},
            {"words", split_ws(text).size()},
            {"headings", json::array()},
            {"description", ""},
            {"site", ""},
            {"author", ""},
            {"lang", ""}}},
          {"hash", fingerprint(text)},
          {"document", suf}};
}

int default_connections() {
  int n = (int)get_config().num("performance.crawl_connections", 0);
  int cpus = (int)std::max(1u, std::thread::hardware_concurrency());
  return n > 0 ? std::clamp(n, 1, 32) : std::clamp(cpus, 4, 8);
}

}  // namespace

// ---------------------------------------------------------------- URLs y ambito
std::string normalize(const std::string& url) {
  U p = urlparse(trim(url));
  std::vector<std::string> q;
  for (auto& part : split(p.query, "&")) {
    if (part.empty()) continue;
    auto eq = part.find('=');
    std::string k = unquote_plus(part.substr(0, eq)), v = eq == std::string::npos ? "" : unquote_plus(part.substr(eq + 1));
    if (TRACKING.test(k)) continue;
    q.push_back(url_encode(k, true) + "=" + url_encode(v, true));
  }
  static const Rx SLASHES("/{2,}"), INDEX(R"(/index\.html?$)");
  std::string path = SLASHES.sub(p.path.empty() ? "/" : p.path, "/");
  path = INDEX.sub(path, "/");
  U o{lower(p.scheme), lower(p.netloc), path, join(q, "&"), ""};
  return unparse(o);
}

std::string site_root(const std::string& netloc) {
  std::string host = host_of(netloc);
  if (contains(host, ":") || is_digit_str(replace_all(host, ".", ""))) return host;
  auto parts = split(host, ".");
  size_t n = parts.size();
  if (n >= 3 && SLD.count(parts[n - 2]) && parts[n - 1].size() == 2) return parts[n - 3] + "." + parts[n - 2] + "." + parts[n - 1];
  if (n >= 2) return parts[n - 2] + "." + parts[n - 1];
  return host;
}

std::string doc_suffix(const std::string& url, const std::string& ctype) {
  std::string suf = suffix_of(urlparse(url).path);
  if (DOC_EXT.count(suf)) return suf;
  std::string c = lower(ctype);
  for (auto& [k, v] : DOC_CTYPES)
    if (contains(c, k)) return v;
  return "";
}

std::string scope_prefix(const std::string& start, const std::string& scope) {
  if (scope == "domain") return "/";
  std::string path = urlparse(start).path;
  if (path.empty()) path = "/";
  if (!ends_with(path, "/")) {
    std::string last = path.substr(path.rfind('/') + 1);
    path = contains(last, ".") ? path.substr(0, path.rfind('/')) + "/" : path + "/";
  }
  return path;
}

bool in_scope(const std::string& url, const std::string& start, const std::string& prefix, bool domain) {
  U p = urlparse(url), s = urlparse(start);
  if (p.scheme != "http" && p.scheme != "https") return false;
  if (domain) {
    std::string host = host_of(p.netloc), root = site_root(s.netloc);
    if (host != root && !ends_with(host, "." + root)) return false;
  } else if (host_of(p.netloc) != host_of(s.netloc)) {
    return false;
  }
  std::string path = p.path.empty() ? "/" : p.path;
  if (!domain && !starts_with(path + "/", prefix) && !starts_with(path, prefix)) return false;
  std::string suf = suffix_of(path);
  if ((SKIP_EXT.count(suf) && !(domain && DOC_EXT.count(suf))) || SKIP_PATH.test(path) || SKIP_QUERY.test(p.query))
    return false;
  return true;
}

// ---------------------------------------------------------------- rastreo
json crawl(const std::string& start_url, CrawlOpts o) {
  if (!re_search(R"(^https?://)", start_url, Rx::I)) throw std::invalid_argument("la URL debe empezar por http:// o https://");
  int max_pages = std::clamp(o.max_pages, 1, MAX_PAGES_LIMIT);
  int max_depth = std::max(0, o.max_depth);
  long long max_chars = std::clamp<long long>(o.max_chars, 1, MAX_TOTAL_CHARS);
  std::string start, prefix, scope = o.scope;
  const json& state = o.state;
  bool has_state = state.is_object();
  if (has_state) {
    start = jstr(state, "start");
    prefix = jstr(state, "prefix");
    scope = jstr(state, "scope", scope);
  } else {
    start = normalize(start_url);
    prefix = scope_prefix(start, scope);
  }
  Fetcher fetch = o.fetch ? o.fetch : http_fetcher();
  auto step = [&](const std::string& t) {
    if (o.emit) o.emit({{"type", "progress"}, {"text", t}});
  };
  bool dom = scope == "domain";
  int min_words = dom ? MIN_WORDS_DOMAIN : MIN_WORDS;
  auto scoped = [&](const std::string& u) { return in_scope(u, start, prefix, dom); };
  double t0 = now();

  std::optional<Robots> robots;
  std::vector<std::string> declared;
  if (o.respect_robots) std::tie(robots, declared) = robots_of(fetch, start);
  double wait = std::max(0.0, o.delay);
  int conns = std::max(1, o.connections > 0 ? o.connections : default_connections());
  if (robots) {
    double cd = robots->crawl_delay(UA_NAME);
    if (cd > 0) {
      wait = std::max(wait, std::min(cd, MAX_CRAWL_DELAY));
      conns = 1;
    }
  }
  std::map<std::string, std::optional<Robots>> host_robots{{urlparse(start).netloc, robots}};
  auto allowed = [&](const std::string& u) {
    if (!o.respect_robots) return true;
    std::string h = urlparse(u).netloc;
    if (!host_robots.count(h)) host_robots[h] = robots_of(fetch, u).first;
    auto& rp = host_robots[h];
    return rp ? rp->can_fetch(UA_NAME, u) : true;
  };

  std::deque<std::pair<std::string, long long>> queue;
  std::set<std::string> queued;
  if (has_state) {
    for (auto& e : state.value("queue", json::array())) queue.push_back({e[0].get<std::string>(), e[1].get<long long>()});
    for (auto& e : state.value("queued", json::array())) queued.insert(e.get<std::string>());
  } else {
    queue.push_back({start, 0});
    queued.insert(start);
  }
  if (o.use_sitemap && !has_state) {
    step("Leyendo el sitemap");
    for (auto& u : sitemap_seeds(fetch, start, declared, o.seed_limit ? o.seed_limit : (size_t)max_pages * 5, dom ? 2000 : 6)) {
      std::string n = normalize(u);
      if (!queued.count(n) && scoped(n)) {
        queued.insert(n);
        queue.push_back({n, 1});
      }
    }
  }

  std::mutex pace;
  double slot = 0;
  std::atomic<long long> counter{0};
  auto task = [&, fetch](const std::string& url) -> json {
    if (wait > 0) {
      double at;
      {
        std::lock_guard<std::mutex> lk(pace);   // separacion minima entre inicios de peticion (cortesia con el servidor)
        double n = now();
        at = std::max(n, slot);
        slot = at + wait;
      }
      double d = at - now();
      if (d > 0) std::this_thread::sleep_for(std::chrono::milliseconds((long long)(d * 1000)));
    }
    ++counter;
    std::tuple<int, std::string, std::string, std::string> got;
    try {
      got = fetch(url);
    } catch (const std::exception& e) {
      return {{"error", uleft(std::string("error: ") + e.what(), 160)}};
    }
    auto& [status, ctype, raw, fin] = got;
    json res = {{"status", status}, {"ctype", ctype}, {"final", normalize(fin.empty() ? url : fin)}};
    std::string fin_n = res["final"];
    std::string lct = lower(ctype);
    if (status == 200 && dom && !doc_suffix(fin_n, ctype).empty() && !contains(lct, "html")) {
      try {
        res["page"] = analyze_document(raw, fin_n, ctype);
      } catch (const std::exception& e) {   // PDF cifrado o roto
        return {{"error", uleft(std::string("documento ilegible: ") + e.what(), 160)}};
      }
    } else if (status == 200 && (ctype.empty() || contains(lct, "html"))) {
      res["page"] = analyze_page(raw, fin_n);
    }
    return res;
  };

  struct Slot {
    std::string url;
    long long depth;
    std::shared_future<json> fut;
  };
  std::deque<Slot> window;
  json pages = json::array();
  std::vector<std::pair<std::string, std::string>> skipped;
  std::set<std::string> hashes, visited;
  if (has_state) {
    for (auto& h : state.value("hashes", json::array())) hashes.insert(h.get<std::string>());
    for (auto& v : state.value("visited", json::array())) visited.insert(v.get<std::string>());
  }
  long long total_chars = 0, committed = has_state ? 1 : 0;
  std::string site = has_state ? jstr(state, "site") : "", stopped;
  auto ready = [](json v) {
    std::promise<json> p;
    p.set_value(std::move(v));
    return p.get_future().share();
  };
  while (true) {
    while (!queue.empty() && (int)window.size() < conns && (long long)(pages.size() + window.size()) < max_pages &&
           (committed || window.empty())) {
      auto [url, depth] = queue.front();
      queue.pop_front();
      if (visited.count(url) || ((!pages.empty() || !skipped.empty() || has_state) && !scoped(url))) continue;
      visited.insert(url);
      if (!allowed(url)) window.push_back({url, depth, ready({{"robots", true}})});
      else window.push_back({url, depth, std::async(std::launch::async, task, url).share()});
    }
    if (window.empty()) break;
    if (now() - t0 > o.max_seconds) {
      stopped = "tiempo agotado";
      break;
    }
    if (total_chars >= max_chars) {
      stopped = "limite de texto alcanzado";
      break;
    }
    if (o.should_stop && o.should_stop()) {
      stopped = "detenido";
      break;
    }
    Slot sl = window.front();
    window.pop_front();
    json res = sl.fut.get();
    std::string url = sl.url;
    ++committed;
    if (res.value("robots", false)) {
      skipped.push_back({url, "robots.txt"});
      continue;
    }
    long long tot = o.total ? o.total : max_pages;
    std::string of = dom && tot >= 1000000000LL ? "?" : std::to_string(tot);
    step("Rastreando " + std::to_string(o.offset + (long long)pages.size() + 1) + "/" + of + ": " + url);
    if (res.contains("error")) {
      skipped.push_back({url, jstr(res, "error")});
      continue;
    }
    int status = res["status"];
    std::string ctype = res["ctype"], fin = res["final"];
    if (status != 200) {
      skipped.push_back({url, "HTTP " + std::to_string(status)});
      continue;
    }
    if (!ctype.empty() && !contains(lower(ctype), "html") && !res.contains("page")) {
      skipped.push_back({url, "no es HTML (" + split(ctype, ";")[0] + ")"});
      continue;
    }
    if (fin != url && url == start && pages.empty() && skipped.empty() && !has_state) {
      // la portada redirige (http -> https, /docs -> /docs/en/latest/...): el ambito se toma del destino
      start = fin;
      prefix = scope_prefix(fin, scope);
      visited.insert(fin);
    } else if (fin != url) {
      if (!scoped(fin) && fin != start) {
        skipped.push_back({url, "redirige fuera del ambito (" + fin + ")"});
        continue;
      }
      if (visited.count(fin) && url != start) continue;
      visited.insert(fin);
    }
    json& info = res["page"];
    std::vector<std::string> inside;
    if ((dom || sl.depth < max_depth) && !info.value("nofollow", false))
      for (auto& lk : info["links"]) {
        std::string link = lk;
        if (scoped(link)) {
          inside.push_back(link);
          if (!queued.count(link)) {
            queued.insert(link);
            queue.push_back({link, sl.depth + 1});
          }
        }
      }
    std::string canonical = jstr(info, "canonical");
    if (!canonical.empty() && canonical != fin && visited.count(canonical) && scoped(canonical)) {
      skipped.push_back({url, "duplicado (canonica)"});
      continue;
    }
    if (info.value("noindex", false)) {
      skipped.push_back({url, "noindex"});
      continue;
    }
    json& doc = info["doc"];
    if (doc.value("words", 0) < min_words) {
      skipped.push_back({url, "apenas tiene texto"});
      continue;
    }
    std::string hash = jstr(info, "hash");
    if (hashes.count(hash)) {
      skipped.push_back({url, "contenido duplicado"});
      continue;
    }
    hashes.insert(hash);
    if (site.empty()) site = jstr(doc, "site");
    std::string text = uleft(jstr(doc, "text"), (size_t)std::max(0LL, MAX_TOTAL_CHARS - total_chars));
    total_chars += (long long)ulen(text);
    if (inside.size() > 200) inside.resize(200);
    pages.push_back({{"url", fin},
                     {"title", doc["title"]},
                     {"text", text},
                     {"depth", sl.depth},
                     {"words", doc["words"]},
                     {"headings", doc["headings"]},
                     {"description", doc["description"]},
                     {"site", doc["site"]},
                     {"author", doc["author"]},
                     {"lang", doc["lang"]},
                     {"links", inside},
                     {"document", jstr(info, "document")}});
    if ((long long)pages.size() >= max_pages) break;
  }
  // lo que estaba en vuelo vuelve a la cabeza de la cola, en su orden: el siguiente lote empieza por ahi
  for (auto it = window.rbegin(); it != window.rend(); ++it) {
    visited.erase(it->url);
    queue.push_front({it->url, it->depth});
  }
  json rest = json::array();
  for (auto& [u, d] : queue)
    if (!visited.count(u)) rest.push_back({u, d});
  if (!stopped.empty() && !window.empty()) skipped.push_back({window.front().url, stopped});
  window.clear();   // espera a las descargas en vuelo (cada una con su tiempo limite)
  if (site.empty()) site = host_of(urlparse(start).netloc);
  json jskipped = json::array();
  for (auto& [u, r] : skipped) jskipped.push_back({u, r});
  json new_state = {{"start", start},   {"prefix", prefix}, {"scope", scope},       {"site", site},
                    {"queue", rest},    {"queued", queued}, {"visited", visited},   {"hashes", hashes},
                    {"done", rest.empty()}, {"stopped", stopped}};
  return {{"start", start},
          {"scope", scope},
          {"prefix", prefix},
          {"site", site},
          {"pages", pages},
          {"skipped", jskipped},
          {"fetched", counter.load()},
          {"pending", rest.size()},
          {"robots", robots.has_value()},
          {"seconds", round_to(now() - t0, 1)},
          {"connections", conns},
          {"state", new_state},
          {"stopped", stopped}};
}

// ---------------------------------------------------------------- a la biblioteca
std::string site_title(const json& res) {
  const json& pages = res["pages"];
  std::string first = pages.empty() ? "" : jstr(pages[0], "title");
  std::string site = jstr(res, "site");
  static const Rx SEP(R"(\s+[|—–·-]\s+)");
  std::vector<std::string> parts;
  for (auto& p : SEP.split(first))
    if (!trim(p).empty()) parts.push_back(trim(p));
  std::string name = parts.size() > 1 ? parts.back() : (site.empty() ? first : site);
  return uleft("Documentación: " + name, 200);
}

std::string to_markdown(const json& res, const std::string& title) {
  std::vector<std::string> lines = {"# " + title, "",
                                    "Rastreado desde " + jstr(res, "start") + " (ámbito " + jstr(res, "prefix") + ") el " +
                                        strftime_local("%Y-%m-%d %H:%M") + ". " + std::to_string(res["pages"].size()) + " páginas.",
                                    "", "## Índice", ""};
  int i = 1;
  for (auto& p : res["pages"]) lines.push_back(std::to_string(i++) + ". [" + jstr(p, "title") + "](" + jstr(p, "url") + ")");
  for (auto& p : res["pages"]) {
    for (auto& l : {std::string(""), std::string("---"), std::string(""), "<!-- " + jstr(p, "url") + " -->", std::string("")})
      lines.push_back(l);
    lines.push_back(jstr(p, "text"));
  }
  return join(lines, "\n") + "\n";
}

std::string slug_of(const json& res) {
  std::string s = lower(host_of(urlparse(jstr(res, "start")).netloc) + jstr(res, "prefix"));
  return uleft(trim(re_sub("[^a-z0-9]+", "-", s), "-"), 60);
}

json ingest_batch(Services& s, const json& res, const std::string& title, const std::string& agent, bool learn, bool use_llm,
                  const json& meta_extra) {
  const json& pages = res["pages"];
  auto& rag = *s.rag;
  std::string sfx = jstr(meta_extra, "suffix");
  std::string slug = slug_of(res);
  fs::path md_path = rag.files / P(std::to_string((long long)now()) + "_" + (slug.empty() ? "docs" : slug) + sfx + ".md");
  write_file(md_path, to_markdown(res, title));
  std::vector<std::string> first12, heads, texts, labels, urls;
  for (size_t i = 0; i < pages.size(); ++i) {
    if (i < 12) first12.push_back(jstr(pages[i], "text"));
    heads.push_back(jstr(pages[i], "title"));
    texts.push_back("# " + jstr(pages[i], "title") + "\n\n" + jstr(pages[i], "text"));
    labels.push_back("[" + jstr(pages[i], "url") + "]");
    urls.push_back(jstr(pages[i], "url"));
  }
  std::string combined = join(first12, "\n\n");
  json doc = {{"title", title}, {"text", combined}, {"site", res["site"]}, {"author", ""}, {"headings", heads}};
  std::optional<std::pair<std::string, std::vector<std::string>>> summ;
  if (learn && use_llm) summ = llm_summary(s, doc);
  auto [summary, points] = summ ? *summ : extractive_summary(combined);
  json meta = {{"origin", "crawl"},
               {"start", res["start"]},
               {"scope", res["scope"]},
               {"prefix", res["prefix"]},
               {"site", res["site"]},
               {"pages_indexed", pages.size()},
               {"fetched", res["fetched"]},
               {"skipped", res["skipped"].size()},
               {"pending", res["pending"]},
               {"urls", urls},
               {"summary", summary},
               {"crawled", now()}};
  for (auto& [k, v] : meta_extra.items())
    if (k != "suffix") meta[k] = v;
  json rag_doc;
  try {
    rag_doc = rag.ingest_pages(title, texts, jstr(res, "start"), "web", S(md_path), meta, labels, agent, !learn);
  } catch (...) {
    remove_file(md_path);
    throw;
  }
  int learned = learn ? assimilate(s, doc, summary, points, rag_doc["id"].get<long long>(), jstr(res, "start"), agent) : 0;
  return {{"doc", rag_doc}, {"summary", summary}, {"points", points}, {"learned", learned}};
}

json replace_old(Services& s, const std::string& start, const std::string& prefix, const std::string& agent,
                 const std::set<long long>& keep) {
  json out = json::array();
  for (auto& d : s.rag->list()) {
    const json& m = d.contains("meta") && d["meta"].is_object() ? d["meta"] : json::object();
    long long id = d["id"];
    if (!keep.count(id) && jstr(m, "origin") == "crawl" && jstr(m, "start") == start && jstr(m, "prefix") == prefix &&
        jstr(d, "agent") == agent) {
      s.rag->remove(id);
      out.push_back(id);
    }
  }
  return out;
}

json crawl_site(Services& s, const std::string& start_url, int max_pages, int max_depth, const std::string& scope,
                const Emit& emit, const std::string& agent, int batch_pages, const std::string& job, double max_seconds) {
  auto [auto_pages, auto_chars] = crawl_jobs::auto_batch(s.profile);
  int bpages = std::clamp(batch_pages > 0 ? batch_pages : auto_pages, 1, MAX_PAGES_LIMIT);
  if (!job.empty() || max_pages > bpages) {
    json j = !job.empty() ? crawl_jobs::get(job)
                          : crawl_jobs::create(start_url, max_pages, max_depth, scope, agent, true, bpages, auto_chars);
    if (j.is_null()) throw std::invalid_argument("no existe el rastreo " + job);
    std::string id = j["id"];
    auto ev = crawl_jobs::claim(id);
    try {
      j["status"] = crawl_jobs::RUNNING;
      j["reason"] = "";
      j["error"] = "";
      j = crawl_jobs::run_batches(s, j, emit, [ev] { return ev->load(); }, max_seconds);
    } catch (...) {
      crawl_jobs::release(id);
      throw;
    }
    crawl_jobs::release(id);
    if (j["batches"].empty())
      throw std::invalid_argument(!jstr(j, "error").empty() ? jstr(j, "error") : "no se ha podido leer ninguna pagina con texto");
    return crawl_jobs::job_result(j);
  }
  // una sola tanda: un unico documento, como siempre
  CrawlOpts o;
  o.max_pages = max_pages;
  o.max_depth = max_depth;
  o.scope = scope;
  o.emit = emit;
  o.max_seconds = max_seconds;
  json res = crawl(start_url, o);
  const json& pages = res["pages"];
  if (pages.empty()) {
    std::vector<std::string> why;
    for (size_t i = 0; i < res["skipped"].size() && i < 3; ++i)
      why.push_back(res["skipped"][i][0].get<std::string>() + ": " + res["skipped"][i][1].get<std::string>());
    throw std::invalid_argument("no se ha podido leer ninguna pagina con texto (" +
                                (why.empty() ? std::string("sin enlaces en el ambito") : join(why, "; ")) + ")");
  }
  std::string title = site_title(res);
  if (emit) {
    emit({{"type", "progress"}, {"text", "Indexando " + std::to_string(pages.size()) + " páginas en la biblioteca RAG"}});
    emit({{"type", "progress"}, {"text", "Incorporando al conocimiento"}});
  }
  json out = ingest_batch(s, res, title, agent, true, true, {{"max_pages", max_pages}, {"max_depth", max_depth}});
  json rag_doc = out["doc"];
  json replaced = replace_old(s, jstr(res, "start"), jstr(res, "prefix"), agent, {rag_doc["id"].get<long long>()});
  L.info("Rastreo de ", jstr(res, "start"), ": ", pages.size(), " paginas, ", res["skipped"].size(), " omitidas, ",
         rag_doc["chunks"].dump(), " fragmentos");
  long long words = 0;
  json titles = json::array(), skipped = json::array();
  for (size_t i = 0; i < pages.size(); ++i) {
    words += pages[i].value("words", 0LL);
    if (i < 40) titles.push_back(pages[i]["title"]);
  }
  for (size_t i = 0; i < res["skipped"].size() && i < 30; ++i)
    skipped.push_back({{"url", res["skipped"][i][0]}, {"reason", res["skipped"][i][1]}});
  return {{"title", title},
          {"start", res["start"]},
          {"scope", res["scope"]},
          {"prefix", res["prefix"]},
          {"site", res["site"]},
          {"pages", pages.size()},
          {"fetched", res["fetched"]},
          {"pending", res["pending"]},
          {"robots", res["robots"]},
          {"words", words},
          {"page_titles", titles},
          {"skipped", skipped},
          {"skipped_total", res["skipped"].size()},
          {"rag_doc_id", rag_doc["id"]},
          {"rag_chunks", rag_doc["chunks"]},
          {"replaced", replaced},
          {"summary", out["summary"]},
          {"key_points", out["points"]},
          {"learned", out["learned"]},
          {"url", "/api/rag/" + rag_doc["id"].dump() + "/file"},
          {"seconds", res["seconds"]},
          {"connections", res["connections"]},
          {"agent", agent},
          {"batches", 1}};
}

}  // namespace nx::rag::crawler
