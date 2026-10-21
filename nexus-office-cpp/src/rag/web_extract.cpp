// Extraccion estructurada de paginas web (rag/web_extract.py) sin dependencias: metadatos, secciones por
// encabezados con bloques (parrafo, lista, tabla, codigo, cita), imagenes y enlaces.
#include <set>

#include "rag/rag.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag {

static const std::set<std::string> SKIP = {"script", "style",  "noscript", "svg",    "nav",    "footer",
                                           "aside",  "form",   "iframe",   "template", "button", "select",
                                           "canvas", "object", "dialog"};
static const std::set<std::string> BLOCK = {"p",  "div", "section", "article", "main", "header", "li", "ul",
                                            "ol", "table", "tr",    "td",      "th",   "pre",    "blockquote",
                                            "h1", "h2",  "h3",      "h4",      "h5",   "h6",     "br", "hr",
                                            "figure", "figcaption", "dd", "dt", "dl"};
static const std::set<std::string> VOID_TAGS = {"br",   "hr",    "img", "meta",  "link",  "input", "source",
                                           "wbr",  "area",  "base", "col",  "embed", "param", "track"};

static std::string clean(const std::string& t) {
  static const Rx ws(R"(\s+)");
  return trim(ws.sub(html_unescape(t), " "));
}

// ---------------------------------------------------------------- URLs
std::string url_host(const std::string& url) {
  static const Rx r(R"(^[A-Za-z][A-Za-z0-9+.-]*://([^/?#]*))");
  auto m = r.search(url);
  return m ? m->str(1) : "";
}

static std::string remove_dots(const std::string& path) {
  std::vector<std::string> out;
  auto segs = split(path, "/");
  for (size_t i = 0; i < segs.size(); ++i) {
    const auto& s = segs[i];
    if (s == ".") {
      if (i + 1 == segs.size()) out.push_back("");
      continue;
    }
    if (s == "..") {
      if (out.size() > 1) out.pop_back();
      if (i + 1 == segs.size()) out.push_back("");
      continue;
    }
    out.push_back(s);
  }
  return join(out, "/");
}

std::string urljoin(const std::string& base, const std::string& href) {
  if (base.empty()) return href;
  if (href.empty()) return base;
  static const Rx scheme(R"(^([A-Za-z][A-Za-z0-9+.-]*):)");
  if (auto m = scheme.search(href)) {
    std::string s = lower(m->str(1));
    if (s != "http" && s != "https") return href;
    if (href.size() > m->str().size() + 1 && href.substr(m->str().size(), 2) == "//") return href;
  }
  auto bs = scheme.search(base);
  std::string bscheme = bs ? bs->str(1) : "";
  if (starts_with(href, "//")) return bscheme + ":" + href;
  std::string host = url_host(base);
  std::string prefix = bscheme.empty() ? "" : bscheme + "://" + host;
  std::string bpath = base.substr(std::min(base.size(), prefix.size()));
  size_t q = bpath.find_first_of("?#");
  std::string bquery = q == std::string::npos ? "" : bpath.substr(q);
  if (q != std::string::npos) bpath = bpath.substr(0, q);
  if (bpath.empty()) bpath = "/";
  if (href[0] == '#') {
    size_t h = bquery.find('#');
    return prefix + bpath + (h == std::string::npos ? bquery : bquery.substr(0, h)) + href;
  }
  if (href[0] == '?') return prefix + bpath + href;
  std::string hpath = href, tail;
  size_t hq = hpath.find_first_of("?#");
  if (hq != std::string::npos) {
    tail = hpath.substr(hq);
    hpath = hpath.substr(0, hq);
  }
  std::string merged = hpath[0] == '/' ? hpath : bpath.substr(0, bpath.rfind('/') + 1) + hpath;
  return prefix + remove_dots(merged) + tail;
}

// ---------------------------------------------------------------- analizador (html.parser.HTMLParser)
namespace {
using Attrs = std::vector<std::pair<std::string, std::string>>;

struct Parser {
  std::string base;
  json meta = json::object();
  std::string title, lang;
  json blocks = json::array(), images = json::array(), links = json::array();
  std::vector<std::string> stack;
  int skip = 0;
  std::string skip_tag;
  int main = 0;
  std::string buf;
  bool in_title = false;
  int pre = 0;
  std::vector<std::vector<std::string>> list_stack;
  std::optional<json> table;
  std::optional<std::vector<std::string>> row;
  std::optional<std::string> cell;
  std::optional<std::pair<std::string, std::string>> link;   // href, texto
  int quote = 0;

  static std::string attr(const Attrs& a, const std::string& k) {
    for (auto& [n, v] : a)
      if (n == k) return v;
    return "";
  }
  static bool has_attr(const Attrs& a, const std::string& k) {
    for (auto& [n, v] : a)
      if (n == k) return true;
    return false;
  }

  void flush(const std::string& kind = "p", int level = 0) {
    std::string text = pre ? buf : clean(buf);
    buf.clear();
    if (trim(text).empty()) return;
    if (cell) {
      *cell += (cell->empty() ? "" : " ") + text;
      return;
    }
    if (!list_stack.empty() && kind == "p") {
      list_stack.back().push_back(text);
      return;
    }
    json b = {{"type", quote && kind == "p" ? "quote" : kind}, {"text", text}, {"main", main > 0}};
    if (kind == "heading") b["level"] = level;
    blocks.push_back(b);
  }

  void start(const std::string& tag, const Attrs& a) {
    static const Rx noise(
        "(cookie|newsletter|subscribe|suscr|share|compart|related|sidebar|menu|banner|advert|promo|comment|breadcrumb|"
        "popup|modal)",
        Rx::I);
    if (tag == "html") lang = attr(a, "lang");
    if (tag == "meta") {
      std::string key = attr(a, "property");
      if (key.empty()) key = attr(a, "name");
      key = lower(key);
      if (!key.empty() && !attr(a, "content").empty()) meta[key] = clean(attr(a, "content"));
      return;
    }
    if (tag == "title") {
      in_title = true;
      return;
    }
    if (skip) {   // dentro de una zona omitida: solo se cuenta el anidamiento de la etiqueta que la abrio
      if (tag == skip_tag) ++skip;
      return;
    }
    if (SKIP.count(tag) || ((tag == "div" || tag == "section" || tag == "ul" || tag == "header") &&
                            noise.test(attr(a, "class") + " " + attr(a, "id") + " " + attr(a, "role")))) {
      if (!VOID_TAGS.count(tag)) {
        skip = 1;
        skip_tag = tag;
      }
      return;
    }
    if (!VOID_TAGS.count(tag)) stack.push_back(tag);
    if (tag == "article" || tag == "main" || attr(a, "role") == "main") ++main;
    if (BLOCK.count(tag) && tag != "li" && tag != "td" && tag != "th" && tag != "tr") flush();
    if (tag == "ul" || tag == "ol") {
      list_stack.emplace_back();
    } else if (tag == "li") {
      flush();
    } else if (tag == "table") {
      table = json::array();
    } else if (tag == "tr" && table) {
      row = std::vector<std::string>{};
    } else if ((tag == "td" || tag == "th") && row) {
      flush();
      cell = std::string();
    } else if (tag == "pre") {
      ++pre;
    } else if (tag == "blockquote") {
      ++quote;
    } else if (tag == "img") {
      std::string src = attr(a, "src");
      if (src.empty()) src = attr(a, "data-src");
      if (src.empty()) src = split(attr(a, "srcset"), " ")[0];
      if (!src.empty() && !starts_with(src, "data:"))
        images.push_back({{"src", urljoin(base, src)}, {"alt", clean(attr(a, "alt"))}, {"main", main > 0}});
    } else if (tag == "a" && !attr(a, "href").empty()) {
      std::string href = urljoin(base, attr(a, "href"));
      if (starts_with(href, "http")) link = std::make_pair(href, std::string());
    } else if (tag == "br") {
      buf += pre ? "\n" : " ";
    }
  }

  void end(const std::string& tag) {
    if (tag == "title") {
      in_title = false;
      return;
    }
    if (skip) {
      if (tag == skip_tag) --skip;
      return;
    }
    if (VOID_TAGS.count(tag)) return;
    if (std::find(stack.begin(), stack.end(), tag) != stack.end())
      while (!stack.empty()) {
        std::string t = stack.back();
        stack.pop_back();
        if (t == tag) break;
      }
    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
      flush("heading", tag[1] - '0');
    } else if (tag == "pre") {
      flush("code");
      pre = std::max(0, pre - 1);
    } else if (tag == "li") {
      flush();
    } else if ((tag == "ul" || tag == "ol") && !list_stack.empty()) {
      flush();
      auto items = list_stack.back();
      list_stack.pop_back();
      if (!items.empty()) {
        if (!list_stack.empty()) {   // lista anidada: se aplana con sangria
          for (auto& i : items) list_stack.back().push_back("  " + i);
        } else {
          blocks.push_back({{"type", "list"}, {"items", items}, {"ordered", tag == "ol"}, {"main", main > 0}});
        }
      }
    } else if ((tag == "td" || tag == "th") && row && cell) {
      flush();
      row->push_back(clean(*cell));
      cell.reset();
    } else if (tag == "tr" && table && row) {
      bool any = false;
      for (auto& c : *row)
        if (!c.empty()) any = true;
      if (any) table->push_back(*row);
      row.reset();
    } else if (tag == "table" && table) {
      if (!table->empty()) blocks.push_back({{"type", "table"}, {"rows", *table}, {"main", main > 0}});
      table.reset();
    } else if (tag == "blockquote") {
      flush();
      quote = std::max(0, quote - 1);
    } else if (tag == "a" && link) {
      if (!trim(link->second).empty()) links.push_back({{"href", link->first}, {"text", uleft(clean(link->second), 120)}});
      link.reset();
    } else if (BLOCK.count(tag)) {
      flush();
    }
    if (tag == "article" || tag == "main") main = std::max(0, main - 1);
  }

  void data(const std::string& d) {
    if (in_title) {
      title += d;
      return;
    }
    if (skip) return;
    buf += d;
    if (link) link->second += d;
  }

  void feed(const std::string& s) {
    static const Rx tagname(R"(^<(/?)([A-Za-z][^\t\n\r\f />\x00]*))");
    static const Rx attr_rx(
        R"rx(((?<=['"\s/])[^\s/>][^\s/=>]*)(\s*=+\s*('[^']*'|"[^"]*"|(?!['"])[^>\s]*))?(?:\s|/(?!>))*)rx");
    size_t i = 0, n = s.size();
    std::string cdata_tag;   // script/style: texto crudo hasta su cierre
    while (i < n) {
      if (!cdata_tag.empty()) {
        std::string low = lower(s.substr(i));
        size_t e = low.find("</" + cdata_tag);
        if (e == std::string::npos) {
          data(s.substr(i));
          return;
        }
        if (e) data(s.substr(i, e));
        i += e;
        size_t gt = s.find('>', i);
        end(cdata_tag);
        cdata_tag.clear();
        i = gt == std::string::npos ? n : gt + 1;
        continue;
      }
      size_t lt = s.find('<', i);
      if (lt == std::string::npos) {
        data(html_unescape(s.substr(i)));
        break;
      }
      if (lt > i) data(html_unescape(s.substr(i, lt - i)));
      i = lt;
      if (s.compare(i, 4, "<!--") == 0) {
        size_t e = s.find("-->", i + 4);
        i = e == std::string::npos ? n : e + 3;
        continue;
      }
      if (s.compare(i, 2, "<!") == 0 || s.compare(i, 2, "<?") == 0) {
        size_t e = s.find('>', i);
        i = e == std::string::npos ? n : e + 1;
        continue;
      }
      auto m = tagname.match(std::string_view(s).substr(i, 256));
      if (!m) {
        data("<");
        ++i;
        continue;
      }
      // fin de la etiqueta respetando comillas
      size_t j = i + 1;
      char q = 0;
      while (j < n) {
        char c = s[j];
        if (q) {
          if (c == q) q = 0;
        } else if (c == '"' || c == '\'') {
          q = c;
        } else if (c == '>') {
          break;
        }
        ++j;
      }
      if (j >= n) {
        data(html_unescape(s.substr(i)));
        break;
      }
      std::string raw = s.substr(i, j - i + 1);
      i = j + 1;
      bool closing = m->str(1) == "/";
      std::string tag = lower(m->str(2));
      if (closing) {
        end(tag);
        continue;
      }
      Attrs attrs;
      std::string rest = raw.substr(m->str().size());
      std::string spaced = " " + rest;
      for (auto& am : attr_rx.finditer(spaced)) {
        std::string k = lower(am.str(1));
        std::string v = am.has(3) ? am.str(3) : "";
        if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
        if (k == ">" || k.empty()) continue;
        attrs.emplace_back(k, html_unescape(v));
      }
      bool selfclose = ends_with(rtrim(raw.substr(0, raw.size() - 1)), "/");
      start(tag, attrs);
      if (selfclose)
        end(tag);
      else if (tag == "script" || tag == "style")
        cdata_tag = tag;
    }
  }
};

std::string text_of(const json& b) {
  std::string t = jstr(b, "type");
  if (t == "list") {
    std::vector<std::string> l;
    for (auto& i : b["items"]) l.push_back("- " + i.get<std::string>());
    return join(l, "\n");
  }
  if (t == "table") {
    std::vector<std::string> l;
    for (auto& r : b["rows"]) {
      std::vector<std::string> cells;
      for (auto& c : r) cells.push_back(c.get<std::string>());
      l.push_back(join(cells, " | "));
    }
    return join(l, "\n");
  }
  return jstr(b, "text");
}

json sections_of(const json& blocks, const std::string& title) {
  json sections = json::array({{{"heading", "Introducción"}, {"level", 1}, {"blocks", json::array()}}});
  for (auto& b : blocks) {
    if (jstr(b, "type") == "heading") {
      if (lower(clean(jstr(b, "text"))) == lower(clean(title)) && sections.back()["blocks"].empty() &&
          sections.size() == 1)
        continue;   // el h1 repite el titulo de la pagina
      sections.push_back({{"heading", uleft(jstr(b, "text"), 200)},
                          {"level", std::min(3, b.value("level", 1))},
                          {"blocks", json::array()}});
    } else {
      sections.back()["blocks"].push_back(b);
    }
  }
  json out = json::array();
  for (auto& s : sections)
    if (!s["blocks"].empty() || s["level"].get<int>() < 3) out.push_back(s);
  return out;
}
}  // namespace

json extract_html(const std::string& raw, const std::string& url) {
  Parser p;
  p.base = url;
  p.feed(raw);
  p.flush();
  json blocks = p.blocks;
  json main_blocks = json::array();
  size_t main_len = 0;
  for (auto& b : blocks)
    if (b.value("main", false)) {
      main_blocks.push_back(b);
      main_len += ulen(text_of(b));
    }
  if (main_len > 400) blocks = main_blocks;
  // descarta parrafos de 1-2 palabras sueltos (restos de menus) salvo encabezados
  json kept = json::array();
  for (auto& b : blocks) {
    if (jstr(b, "type") != "p") {
      kept.push_back(b);
      continue;
    }
    std::string t = jstr(b, "text");
    if (split_ws(t).size() > 3 || ends_with(t, ".") || ends_with(t, ":")) kept.push_back(b);
  }
  blocks = kept;
  json& m = p.meta;
  std::string title = clean(jstr(m, "og:title").empty() ? p.title : jstr(m, "og:title"));
  for (auto& b : blocks)
    if (jstr(b, "type") == "heading" && b.value("level", 0) == 1 && title.empty()) title = jstr(b, "text");
  std::string host = url_host(url);
  if (title.empty()) title = host.empty() ? "Documento" : host;
  json sections = sections_of(blocks, title);
  std::vector<std::string> parts;
  for (auto& s : sections) {
    std::vector<std::string> bt;
    for (auto& b : s["blocks"]) bt.push_back(text_of(b));
    parts.push_back(repeat("#", s["level"].get<int>()) + " " + jstr(s, "heading") + "\n\n" + join(bt, "\n\n"));
  }
  std::string text = join(parts, "\n\n");
  std::set<std::string> seen;
  json images = json::array();
  bool any_main = false;
  for (auto& i : p.images)
    if (seen.insert(jstr(i, "src")).second) {
      images.push_back(i);
      if (i.value("main", false)) any_main = true;
    }
  if (any_main) {
    json only = json::array();
    for (auto& i : images)
      if (i.value("main", false)) only.push_back(i);
    images = only;
  }
  std::set<std::string> seen_l;
  json links = json::array();
  for (auto& l : p.links)
    if (seen_l.insert(jstr(l, "href")).second) links.push_back(l);
  auto first = [&](std::initializer_list<const char*> keys) {
    for (auto k : keys)
      if (!jstr(m, k).empty()) return jstr(m, k);
    return std::string();
  };
  json headings = json::array();
  for (auto& s : sections)
    if (!jstr(s, "heading").empty()) headings.push_back(s["heading"]);
  json img40 = json::array(), l200 = json::array();
  for (size_t i = 0; i < images.size() && i < 40; ++i) img40.push_back(images[i]);
  for (size_t i = 0; i < links.size() && i < 200; ++i) l200.push_back(links[i]);
  std::string site = first({"og:site_name"});
  std::string t = trim(text);
  return {{"url", url},
          {"title", uleft(title, 250)},
          {"description", first({"og:description", "description"})},
          {"site", site.empty() ? host : site},
          {"author", first({"author", "article:author"})},
          {"published", first({"article:published_time", "date", "dc.date"})},
          {"keywords", jstr(m, "keywords")},
          {"lang", p.lang},
          {"cover_image", jstr(m, "og:image")},
          {"sections", sections},
          {"images", img40},
          {"links", l200},
          {"text", t},
          {"words", split_ws(text).size()},
          {"headings", headings}};
}

}  // namespace nx::rag
