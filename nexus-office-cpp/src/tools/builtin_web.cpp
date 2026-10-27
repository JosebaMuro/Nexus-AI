// Acceso a internet (tools/builtin/web_tools.py): riesgo medio, requiere aprobacion segun la politica.
#include "core/services.h"
#include "memory/memory.h"
#include "rag/browser.h"
#include "rag/rag.h"
#include "rag/web.h"
#include "tools/tools.h"
#include "util/http.h"
#include "util/net.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::tools {

namespace {

const http::Headers UA = {{"User-Agent", "Mozilla/5.0 (Nexus-Office agent)"}};
constexpr size_t MAX_FETCH_BYTES = 5000000;

bool allow_private() { return truthy(get_config().get("rag.allow_private_urls", false)); }

std::string public_url(const std::string& url) {
  auto u = http::parse_url(url);
  if ((u.scheme != "http" && u.scheme != "https") || u.host.empty())
    throw ToolError("ValueError", "solo se permiten URLs http(s)");
  if (allow_private()) return url;   // el administrador lo ha permitido (documentacion de la intranet)
  auto ips = net::resolve(u.host);
  if (ips.empty()) throw ToolError("ConnectError", "no se pudo resolver " + u.host);
  for (auto& ip : ips)
    if (net::is_private_ip(ip)) throw ToolError("ValueError", "no se permite acceder a direcciones de red locales");
  return url;
}

std::string charset_of(const std::string& ctype) {
  static const Rx CS(R"(charset=["']?([\w-]+))", Rx::I);
  auto m = CS.search(ctype);
  return m ? lower(m->str(1)) : "";
}

std::string decode(const std::string& raw, const std::string& enc) {
  if (enc == "iso-8859-1" || enc == "latin-1" || enc == "latin1") return latin1_to_utf8(raw);
  if (enc == "windows-1252" || enc == "cp1252") return cp1252_to_utf8(raw);
  if (enc == "utf-16" || enc == "utf-16le") return utf16_to_utf8(raw, true);
  if (enc == "utf-16be") return utf16_to_utf8(raw, false);
  return valid_utf8(raw);
}

}  // namespace

std::string html_to_text(std::string raw) {
  static const Rx SCRIPTS(R"(<(script|style|noscript|svg|head).*?</\1>)", Rx::I | Rx::S),
      BREAKS(R"(<br\s*/?>|</p>|</h\d>|</li>|</div>)", Rx::I), TAGS(R"(<[^>]+>)"), SPACES(R"([ \t]+)"),
      BLANKS(R"(\n\s*\n+)");
  raw = SCRIPTS.sub(raw, " ");
  raw = BREAKS.sub(raw, "\n");
  std::string text = html_unescape(TAGS.sub(raw, " "));
  return trim(BLANKS.sub(SPACES.sub(text, " "), "\n\n"));
}

namespace {

json web_fetch(const json& args, ToolContext&) {
  std::string url = public_url(jstr(args, "url"));
  http::Response r;
  std::string data;
  for (int hop = 0; hop < 10; ++hop) {   // redirecciones a mano: tambien se comprueba cada destino
    data.clear();
    http::Request req;
    req.url = url;
    req.headers = UA;
    req.timeout = 20;
    req.follow = false;
    req.on_data = [&](const char* p, size_t n) {
      data.append(p, n);
      return data.size() <= MAX_FETCH_BYTES;
    };
    r = http::send(req);
    if (r.status == 0) throw ToolError("ConnectError", r.error.empty() ? "error de red" : r.error);
    if (r.status >= 300 && r.status < 400 && !r.header("location").empty()) {
      url = public_url(rag::urljoin(url, r.header("location")));
      continue;
    }
    break;
  }
  if (data.size() > MAX_FETCH_BYTES) data.resize(MAX_FETCH_BYTES);
  std::string ctype = r.header("content-type");
  std::string raw = decode(data, charset_of(ctype));
  bool rendered = false;
  if (r.status == 200 && contains(ctype, "html"))   // como un navegador: ejecuta el JavaScript si la pagina lo necesita
    std::tie(raw, rendered) = rag::browser::maybe_render(url, raw);
  std::string head = lower(uleft(ltrim(raw), 15));
  std::string text = contains(ctype, "html") || starts_with(head, "<!doctype") || starts_with(head, "<html") ? html_to_text(raw) : raw;
  long limit = std::max(200L, (long)jnum(args.value("max_chars", json(4000)), 4000));
  long offset = std::max(0L, (long)jnum(args.value("offset", json(0))));
  static const Rx TITLE(R"(<title>(.*?)</title>)", Rx::I | Rx::S);
  auto title = TITLE.search(raw);
  long total = (long)ulen(text);
  json out = {{"url", url},
              {"status", r.status},
              {"title", title ? html_unescape(trim(title->str(1))) : ""},
              {"text", usub(text, offset, offset + limit)},
              {"offset", offset},
              {"total_chars", total},
              {"navegador", rendered}};
  if (offset + limit < total) out["next_offset"] = offset + limit;
  return out;
}

json web_search(const json& args, ToolContext& ctx) {
  std::string q = jstr(args, "query");
  int maxr = (int)jnum(args.value("max_results", json(6)), 6);
  http::Request req;
  req.method = "POST";
  req.url = "https://html.duckduckgo.com/html/";
  req.headers = UA;
  req.body = "q=" + url_encode(q, true);
  req.content_type = "application/x-www-form-urlencoded";
  req.timeout = 20;
  auto r = http::send(req);
  if (r.status == 0) throw ToolError("ConnectError", r.error.empty() ? "error de red" : r.error);
  static const Rx RES(R"rx(<a rel="nofollow" class="result__a" href="([^"]+)">(.*?)</a>.*?class="result__snippet"[^>]*>(.*?)</a>)rx",
                      Rx::S);
  static const Rx UDDG(R"([?&]uddg=([^&]+))");
  json results = json::array();
  for (auto& m : RES.finditer(r.body)) {
    std::string href = m.str(1);
    if (contains(href, "uddg=")) {
      std::string unesc = html_unescape(href);
      auto u = UDDG.search(unesc);
      if (u) href = url_decode(u->str(1));
    }
    results.push_back({{"title", html_to_text(m.str(2))}, {"url", href}, {"snippet", html_to_text(m.str(3))}});
    if ((int)results.size() >= 2 * maxr) break;
  }
  // IA de apoyo: se piden el doble y se quedan los mas relevantes por significado (en cualquier idioma)
  if (ctx.services && ctx.services->assist && !results.empty()) {
    std::vector<std::string> texts;
    for (auto& x : results) texts.push_back(jstr(x, "title") + ". " + jstr(x, "snippet"));
    json ordered = json::array();
    for (size_t i : ctx.services->assist->rerank(q, texts)) ordered.push_back(results[i]);
    results = ordered;
  }
  json top = json::array();
  for (size_t i = 0; i < results.size() && (int)i < maxr; ++i) top.push_back(results[i]);
  return {{"query", q}, {"results", top}};
}

// destino "agente": solo el subgrafo del agente que llama; "general" (por defecto): para todos
std::string rag_dest(const json& args, ToolContext& ctx) {
  return jstr(args, "target") == "agente" && !ctx.agent.empty() ? ctx.agent : "";
}

template <class F>
json as_tool_error(F f) {
  try {
    return f();
  } catch (const ToolError&) {
    throw;
  } catch (const std::invalid_argument& e) {
    throw ToolError("ValueError", e.what());
  } catch (const std::exception& e) {
    throw ToolError("RuntimeError", e.what());
  }
}

json url_to_pdf(const json& args, ToolContext& ctx) {
  return as_tool_error([&] {
    json res = rag::process_url(*ctx.services, jstr(args, "url"), ctx.emit, (int)jnum(args.value("max_images", json(8)), 8),
                                rag_dest(args, ctx));
    if (ctx.emit) {
      ctx.emit({{"type", "media"}, {"kind", "document"}, {"url", res["url"]}, {"title", res["title"]}});
      ctx.emit({{"type", "graph_update"}});
    }
    return res;
  });
}

json crawl_docs(const json& args, ToolContext& ctx) {
  return as_tool_error([&] {
    Services& s = *ctx.services;
    std::string url = jstr(args, "url"), scope = jstr(args, "scope", "path"), dest = rag_dest(args, ctx);
    int max_pages = (int)jnum(args.value("max_pages", json(40)), 40), max_depth = (int)jnum(args.value("max_depth", json(3)), 3);
    std::string job = jstr(args, "job");
    if (job.empty() && truthy(args.value("continue", json(false)))) {   // el ultimo rastreo sin terminar de esa URL
      std::string n = rag::crawler::normalize(url);
      for (auto& j : rag::crawl_jobs::list_jobs())
        if (jstr(j, "status") != rag::crawl_jobs::DONE && jstr(j, "agent") == dest &&
            (rag::crawler::normalize(jstr(j, "url")) == n || jstr(j, "start") == n)) {
          job = jstr(j, "id");
          break;
        }
    }
    if (truthy(args.value("background", json(false))) || scope == "domain") {   // dominios: siguen solos, por lotes
      if (job.empty()) {
        auto [pages, chars] = rag::crawl_jobs::auto_batch(s.profile);
        job = jstr(rag::crawl_jobs::create(url, max_pages, max_depth, scope, dest, true, pages, chars), "id");
      }
      rag::crawl_jobs::run_background(s, job, ctx.emit);
      std::string note = "Rastreo por lotes en segundo plano. Cada lote aparece en la biblioteca RAG en cuanto termina; "
                         "consulta con rag_search.";
      if (scope == "domain")
        note += " Se lee todo el dominio sin limite (subdominios y documentos) y al terminar se crea su grafo de "
                "conocimiento multicapa, que se puede dar a un agente desde la Biblioteca RAG.";
      return json{{"job", job}, {"status", "en curso"}, {"note", note}};
    }
    json res = rag::crawler::crawl_site(s, url, max_pages, max_depth, scope, ctx.emit, dest, 0, job, 1500);
    if (jstr(res, "status") == rag::crawl_jobs::PAUSED)
      res["note"] = "Quedan " + res["pending"].dump() + " paginas por rastrear. Vuelve a llamar a crawl_docs con job=\"" +
                    jstr(res, "job") + "\" para seguir por el siguiente lote.";
    if (ctx.emit) ctx.emit({{"type", "graph_update"}});
    return res;
  });
}

Register r1("web_fetch", web_fetch), r2("web_search", web_search), r3("url_to_pdf", url_to_pdf), r4("crawl_docs", crawl_docs);

}  // namespace

void link_builtin_web() {}

}  // namespace nx::tools
