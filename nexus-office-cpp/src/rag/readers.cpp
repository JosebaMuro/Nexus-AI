// Lectura de documentos sueltos (rag/readers.py): PDF, Word, OpenDocument, PowerPoint, Excel, EPUB, RTF, HTML y texto.
#include <memory>
#include <algorithm>
#include <set>

#include "rag/rag.h"
#include "util/archive.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag {

static const size_t MAX_TEXT_BYTES = 20 * 1024 * 1024;
static const std::set<std::string> OFFICE = {".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx", ".epub", ".rtf"};
static const std::set<std::string> MARKUP = {".html", ".htm", ".xhtml"};
static const std::set<std::string> TEXT = {".txt", ".md",  ".markdown", ".rst", ".adoc", ".org",  ".tex",
                                           ".log", ".csv", ".tsv",      ".json", ".jsonl", ".yaml", ".yml",
                                           ".toml", ".ini", ".cfg",     ".conf", ".xml",  ".srt",  ".vtt"};
static const std::set<std::string> CODE = {
    ".py",  ".pyi",  ".ipynb", ".js",   ".mjs",   ".cjs",    ".ts",     ".tsx",   ".jsx",    ".vue",  ".svelte",
    ".java", ".kt",  ".kts",   ".scala", ".groovy", ".gradle", ".c",    ".h",     ".cc",     ".cpp",  ".hpp",
    ".cs",  ".go",   ".rs",    ".rb",   ".php",   ".swift",  ".m",      ".mm",    ".dart",   ".lua",  ".r",
    ".jl",  ".pl",   ".sh",    ".bash", ".zsh",   ".ps1",    ".bat",    ".cmd",   ".sql",    ".graphql", ".proto",
    ".properties", ".css", ".scss", ".sass", ".less", ".el", ".clj",   ".ex",    ".exs",    ".erl",  ".hs",
    ".ml",  ".fs",   ".vb",    ".asm",  ".cmake", ".tf",     ".nix"};
static const std::vector<std::string> ARCHIVES = {".tar.bz2", ".tar.gz", ".tar.xz", ".tbz2", ".tgz", ".txz",
                                                  ".zip",     ".rar",    ".tar",    ".7z"};

std::string archive_suffix(const std::string& name) {
  std::string low = lower(name);
  std::vector<std::string> sorted = ARCHIVES;
  std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
  for (auto& s : sorted)
    if (ends_with(low, s)) return s;
  return "";
}

bool supported(const std::string& name) {
  if (!archive_suffix(name).empty()) return true;
  std::string s = suffix_lower(P(name));
  return s == ".pdf" || OFFICE.count(s) || MARKUP.count(s) || TEXT.count(s) || CODE.count(s);
}

static std::string strip_tags(const std::string& s) {
  static const Rx tag("<[^>]+>");
  return tag.sub(s, "");
}

static std::string xml_text(std::string xml,
                            const std::string& para = R"(</(?:w:p|text:p|text:h|a:p|p|h\d|li|tr|div|br)\s*/?>)") {
  static const Rx tab(R"(<(?:w:tab|text:tab)\s*/>)");
  static const Rx sp(R"(<text:s\b[^>]*/>)");
  static const Rx sp_nl(R"([ \t]+\n)");
  static const Rx many_nl(R"(\n{3,})");
  xml = tab.sub(xml, "\t");
  xml = sp.sub(xml, " ");
  xml = Rx(para).sub(xml, "\n");
  std::string text = html_unescape(strip_tags(xml));
  return trim(many_nl.sub(sp_nl.sub(text, "\n"), "\n\n"));
}

// orden natural: slide2 antes que slide10
static bool natural_less(const std::string& a, const std::string& b) {
  static const Rx num(R"((\d+))");
  auto parts = [](const std::string& s) {
    std::vector<std::pair<bool, std::string>> out;
    size_t i = 0;
    while (i < s.size()) {
      size_t j = i;
      bool d = isdigit((unsigned char)s[i]);
      while (j < s.size() && (bool)isdigit((unsigned char)s[j]) == d) ++j;
      out.emplace_back(d, s.substr(i, j - i));
      i = j;
    }
    return out;
  };
  auto pa = parts(a), pb = parts(b);
  for (size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
    if (pa[i].first && pb[i].first) {
      long long x = std::stoll(pa[i].second.substr(0, 18)), y = std::stoll(pb[i].second.substr(0, 18));
      if (x != y) return x < y;
    } else if (pa[i].second != pb[i].second) {
      return pa[i].second < pb[i].second;
    }
  }
  return pa.size() < pb.size();
}

static std::unique_ptr<archive::Zip> open_zip(const fs::path& p) {
  auto z = std::make_unique<archive::Zip>(p);
  if (!z->ok()) throw std::runtime_error("BadZipFile: el fichero no es un ZIP valido");
  return z;
}

static std::string zread(const archive::Zip& z, const std::string& name) {
  auto d = z.read(name);
  if (!d) throw std::runtime_error("KeyError: falta " + name);
  return valid_utf8(*d);
}

static Doc read_docx(const fs::path& path) {
  auto zp = open_zip(path);
  auto& z = *zp;
  std::string xml = zread(z, "word/document.xml");
  static const Rx para(R"(<w:p[ >].*?</w:p>)", Rx::S);
  std::string text = para.sub(xml, [](const Rx::Match& m) { return strip_tags(m.str()) + "\n\n"; });
  return {stem(path), {html_unescape(strip_tags(text))}};
}

static Doc read_opendocument(const fs::path& path) {
  auto zp = open_zip(path);
  auto& z = *zp;
  std::string xml = zread(z, "content.xml");
  std::string meta = z.has("meta.xml") ? zread(z, "meta.xml") : "";
  static const Rx title_rx(R"(<dc:title>(.*?)</dc:title>)", Rx::S);
  auto title = title_rx.search(meta);
  std::vector<std::string> pages;
  std::string suf = suffix_lower(path);
  if (suf == ".odp") {   // una pagina por diapositiva
    static const Rx page(R"(<draw:page\b.*?</draw:page>)", Rx::S);
    for (auto& m : page.finditer(xml)) pages.push_back(xml_text(m.str()));
  } else if (suf == ".ods") {   // una pagina por hoja, celdas separadas por tabuladores
    static const Rx table(R"(<table:table\b.*?</table:table>)", Rx::S);
    static const Rx row(R"(<table:table-row\b.*?</table:table-row>)", Rx::S);
    static const Rx cell(R"(<table:table-cell\b[^>]*>(.*?)</table:table-cell>)", Rx::S);
    for (auto& sh : table.finditer(xml)) {
      std::vector<std::string> rows;
      std::string sheet = sh.str();
      for (auto& r : row.finditer(sheet)) {
        std::vector<std::string> cells;
        bool any = false;
        for (auto& c : cell.findall(r.str())) {
          cells.push_back(xml_text(c));
          if (!cells.back().empty()) any = true;
        }
        if (any) rows.push_back(rtrim(join(cells, "\t")));
      }
      pages.push_back(join(rows, "\n"));
    }
  } else {
    pages.push_back(xml_text(xml));
  }
  std::vector<std::string> nonempty;
  for (auto& p : pages)
    if (!trim(p).empty()) nonempty.push_back(p);
  if (nonempty.empty()) nonempty.push_back("");
  std::string t = title ? trim(html_unescape(title->str(1))) : "";
  return {t.empty() ? stem(path) : t, nonempty};
}

static Doc read_pptx(const fs::path& path) {
  auto zp = open_zip(path);
  auto& z = *zp;
  static const Rx slide(R"(^ppt/slides/slide\d+\.xml$)");
  std::vector<std::string> names;
  for (auto& e : z.entries())
    if (slide.test(e.name)) names.push_back(e.name);
  std::sort(names.begin(), names.end(), natural_less);
  std::vector<std::string> pages;
  for (auto& n : names) pages.push_back(xml_text(zread(z, n)));
  if (pages.empty()) pages.push_back("");
  return {stem(path), pages};
}

static Doc read_xlsx(const fs::path& path) {
  auto zp = open_zip(path);
  auto& z = *zp;
  std::vector<std::string> shared;
  if (z.has("xl/sharedStrings.xml")) {
    static const Rx si(R"(<si>(.*?)</si>)", Rx::S);
    for (auto& s : si.findall(zread(z, "xl/sharedStrings.xml"))) shared.push_back(html_unescape(strip_tags(s)));
  }
  static const Rx sheet(R"(^xl/worksheets/sheet\d+\.xml$)");
  std::vector<std::string> names;
  for (auto& e : z.entries())
    if (sheet.test(e.name)) names.push_back(e.name);
  std::sort(names.begin(), names.end(), natural_less);
  static const Rx row(R"(<row\b.*?</row>)", Rx::S);
  static const Rx cell(R"(<c\b([^>]*)>(.*?)</c>)", Rx::S);
  static const Rx vrx(R"(<v>(.*?)</v>)", Rx::S);
  static const Rx trx(R"rx(\bt="(\w+)")rx");
  std::vector<std::string> pages;
  for (auto& n : names) {
    std::string xml = zread(z, n);
    std::vector<std::string> rows;
    for (auto& r : row.finditer(xml)) {
      std::vector<std::string> cells;
      bool any = false;
      std::string rs = r.str();
      for (auto& c : cell.finditer(rs)) {
        std::string attrs = c.str(1), body = c.str(2);
        auto v = vrx.search(body);
        auto t = trx.search(attrs);
        std::string val;
        if (t && t->str(1) == "s" && v) {
          size_t i = (size_t)std::stoll("0" + trim(v->str(1)));
          val = i < shared.size() ? shared[i] : "";
        } else if (t && t->str(1) == "inlineStr") {
          val = html_unescape(strip_tags(body));
        } else {
          val = v ? html_unescape(v->str(1)) : "";
        }
        if (!trim(val).empty()) any = true;
        cells.push_back(val);
      }
      if (any) rows.push_back(rtrim(join(cells, "\t")));
    }
    pages.push_back(join(rows, "\n"));
  }
  if (pages.empty()) pages.push_back("");
  return {stem(path), pages};
}

static std::string posix_join(const std::string& base, const std::string& rel) {
  if (base.empty() || base == ".") return rel;
  return base + "/" + rel;
}

static Doc read_epub(const fs::path& path) {
  auto zp = open_zip(path);
  auto& z = *zp;
  std::vector<std::string> names;
  for (auto& e : z.entries()) names.push_back(e.name);
  auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  std::string container = has("META-INF/container.xml") ? zread(z, "META-INF/container.xml") : "";
  static const Rx full(R"rx(full-path="([^"]+)")rx");
  std::string opf_path;
  if (auto m = full.search(container))
    opf_path = m->str(1);
  else
    for (auto& n : names)
      if (ends_with(n, ".opf")) {
        opf_path = n;
        break;
      }
  std::vector<std::string> order;
  std::string title;
  if (!opf_path.empty() && has(opf_path)) {
    std::string opf = zread(z, opf_path);
    static const Rx t(R"(<dc:title[^>]*>(.*?)</dc:title>)", Rx::S);
    if (auto m = t.search(opf)) title = trim(html_unescape(m->str(1)));
    std::map<std::string, std::string> items;
    static const Rx a(R"rx(<item\b[^>]*\bid="([^"]+)"[^>]*\bhref="([^"]+)")rx");
    static const Rx b(R"rx(<item\b[^>]*\bhref="([^"]+)"[^>]*\bid="([^"]+)")rx");
    for (auto& m : a.finditer(opf)) items[m.str(1)] = m.str(2);
    for (auto& m : b.finditer(opf)) items[m.str(2)] = m.str(1);
    std::string base = S(P(opf_path).parent_path());
    static const Rx ref(R"rx(<itemref\b[^>]*\bidref="([^"]+)")rx");
    for (auto& m : ref.finditer(opf)) {
      auto it = items.find(m.str(1));
      if (it != items.end()) order.push_back(posix_join(base, it->second));
    }
  }
  if (order.empty()) {
    for (auto& n : names) {
      std::string l = lower(n);
      if (ends_with(l, ".xhtml") || ends_with(l, ".html") || ends_with(l, ".htm")) order.push_back(n);
    }
    std::sort(order.begin(), order.end(), natural_less);
  }
  std::vector<std::string> pages;
  for (auto& n : order) {
    if (!has(n)) continue;
    json doc = extract_html(zread(z, n), "");
    std::string text = jstr(doc, "text");
    if (!trim(text).empty()) pages.push_back(text);
  }
  if (pages.empty()) pages.push_back("");
  return {title.empty() ? stem(path) : title, pages};
}

static Doc read_rtf(const fs::path& path) {
  std::string raw = latin1_to_utf8(read_file(path, MAX_TEXT_BYTES).value_or(""));
  static const Rx hex(R"(\\'([0-9a-fA-F]{2}))");
  std::string text = hex.sub(raw, [](const Rx::Match& m) {
    std::string b(1, (char)std::stoi(m.str(1), nullptr, 16));
    return cp1252_to_utf8(b);
  });
  static const Rx uni(R"(\\u(-?\d+)\??)");
  text = uni.sub(text, [](const Rx::Match& m) {
    long v = std::stol(m.str(1));
    long c = ((v % 65536) + 65536) % 65536;
    return u8((char32_t)c);
  });
  text = Rx(R"(\{\\\*[^{}]*\})").sub(text, "");
  text = Rx(R"(\{\\(fonttbl|colortbl|stylesheet|info)[^{}]*(\{[^{}]*\}[^{}]*)*\})").sub(text, "");
  text = Rx(R"(\\(par|line)\b ?)").sub(text, "\n");
  text = Rx(R"(\\tab\b ?)").sub(text, "\t");
  text = Rx(R"(\\[a-zA-Z]+-?\d* ?)").sub(text, "");
  text = replace_all(replace_all(replace_all(text, "\\{", "{"), "\\}", "}"), "\\\\", "\\");
  return {stem(path), {trim(Rx("[{}]").sub(text, ""))}};
}

Doc read_text_file(const fs::path& path) {
  std::string data = read_file(path, MAX_TEXT_BYTES).value_or("");
  if (data.substr(0, 4096).find('\0') != std::string::npos) {
    bool utf16 = data.size() >= 2 && (((unsigned char)data[0] == 0xff && (unsigned char)data[1] == 0xfe) ||
                                      ((unsigned char)data[0] == 0xfe && (unsigned char)data[1] == 0xff));
    if (!utf16) return {stem(path), {}};   // binario con extension de texto
  }
  if (data.size() >= 2 && (unsigned char)data[0] == 0xff && (unsigned char)data[1] == 0xfe)
    return {stem(path), {utf16_to_utf8(data.substr(2), true)}};
  if (data.size() >= 2 && (unsigned char)data[0] == 0xfe && (unsigned char)data[1] == 0xff)
    return {stem(path), {utf16_to_utf8(data.substr(2), false)}};
  if (starts_with(data, "\xEF\xBB\xBF")) data = data.substr(3);
  if (valid_utf8(data) == data) return {stem(path), {data}};
  return {stem(path), {cp1252_to_utf8(data)}};
}

Doc read_document(const fs::path& path) {
  std::string suf = suffix_lower(path);
  if (suf == ".pdf") {
    auto data = read_file(path);
    if (!data) throw std::runtime_error("no se pudo leer " + S(path.filename()));
    return read_pdf(*data, stem(path));
  }
  if (MARKUP.count(suf)) {
    json doc = extract_html(valid_utf8(read_text(path)), "");
    std::string t = jstr(doc, "title");
    return {t.empty() ? stem(path) : t, {jstr(doc, "text")}};
  }
  if (suf == ".docx") return read_docx(path);
  if (suf == ".odt" || suf == ".ods" || suf == ".odp") return read_opendocument(path);
  if (suf == ".pptx") return read_pptx(path);
  if (suf == ".xlsx") return read_xlsx(path);
  if (suf == ".epub") return read_epub(path);
  if (suf == ".rtf") return read_rtf(path);
  return read_text_file(path);
}

std::tuple<std::string, std::vector<std::string>, std::string> read_safe(const fs::path& path) {
  try {
    Doc d = read_document(path);
    return {d.title, d.pages, ""};
  } catch (const std::exception& e) {
    return {stem(path), {}, uleft(std::string("Error: ") + e.what(), 200)};
  }
}

}  // namespace nx::rag
