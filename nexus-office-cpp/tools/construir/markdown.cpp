// Markdown a HTML para los manuales en PDF: titulos, parrafos, listas anidadas, citas, tablas, bloques de codigo
// (``` y sangrados), reglas, enlaces, imagenes, codigo en linea, negrita y cursiva. El HTML en bruto se respeta.
#include <functional>

#include "comun.h"
#include "util/rx.h"
#include "util/text.h"

namespace nc {

using namespace nx;

namespace {

std::string esc(std::string_view s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

// & y < sueltos se escapan; las etiquetas HTML y las entidades se dejan
std::string esc_text(std::string_view s) {
  static const Rx tag(R"(^</?[A-Za-z][^<>]*>|^<!--.*?-->)");
  static const Rx ent(R"(^&(#\d+|#x[0-9a-fA-F]+|[A-Za-z]+);)");
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '<') {
      auto m = tag.match(s.substr(i));
      if (m) { o += m->str(); i += m->end() - 1; continue; }
      o += "&lt;";
    } else if (c == '&') {
      o += ent.match(s.substr(i)) ? "&" : "&amp;";
    } else if (c == '>') {
      o += "&gt;";
    } else {
      o += c;
    }
  }
  return o;
}

std::string inline_md(const std::string& s);

// enfasis sobre texto ya sin codigo ni enlaces
std::string emphasis(const std::string& s) {
  static const Rx strong(R"(\*\*(?=\S)(.+?)(?<=\S)\*\*|__(?=\S)(.+?)(?<=\S)__)", Rx::S);
  static const Rx em(R"((?<![\w*])\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?![\w*])|(?<!\w)_(?=[^\s_])(.+?)(?<=[^\s_])_(?!\w))", Rx::S);
  static const Rx strike(R"(~~(?=\S)(.+?)(?<=\S)~~)", Rx::S);
  std::string o = strong.sub(s, [](const Rx::Match& m) { return "<strong>" + m.str(m.has(1) ? 1 : 2) + "</strong>"; });
  o = em.sub(o, [](const Rx::Match& m) { return "<em>" + m.str(m.has(1) ? 1 : 2) + "</em>"; });
  return strike.sub(o, [](const Rx::Match& m) { return "<del>" + m.str(1) + "</del>"; });
}

std::string inline_md(const std::string& s) {
  // trozos protegidos (codigo en linea, imagenes, enlaces, autoenlaces y escapes) se cambian por marcas; el resto pasa
  // entero por esc_text + enfasis (la negrita puede abarcar codigo o varias lineas) y luego vuelven los trozos
  static const Rx tok(R"((`+)(.+?)\1|!\[([^\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)|\[((?:[^\[\]]|\[[^\]]*\])+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)|<(https?://[^>\s]+)>|\\([\\`*_{}\[\]()#+\-.!|>]))");
  static const Rx mark("\x01(\\d+)\x02");
  std::vector<std::string> saved;
  std::string text;
  size_t pos = 0;
  for (auto& m : tok.finditer(s)) {
    text += s.substr(pos, m.start() - pos);
    pos = m.end();
    std::string html;
    if (m.has(1)) html = "<code>" + esc(trim(m.str(2))) + "</code>";
    else if (m.has(4)) html = "<img alt=\"" + esc(m.str(3)) + "\" src=\"" + esc(m.str(4)) + "\" />";
    else if (m.has(6)) html = "<a href=\"" + esc(m.str(6)) + "\">" + inline_md(m.str(5)) + "</a>";
    else if (m.has(7)) html = "<a href=\"" + esc(m.str(7)) + "\">" + esc(m.str(7)) + "</a>";
    else html = esc(m.str(8));
    text += "\x01" + std::to_string(saved.size()) + "\x02";
    saved.push_back(html);
  }
  text += s.substr(pos);
  std::string out = emphasis(esc_text(text));
  return mark.sub(out, [&](const Rx::Match& m) { return saved[std::stoul(m.str(1))]; });
}

size_t indent_of(const std::string& l) {
  size_t n = 0;
  for (char c : l) {
    if (c == ' ') ++n;
    else if (c == '\t') n += 4;
    else break;
  }
  return n;
}

bool blank(const std::string& l) { return trim(l).empty(); }

std::vector<std::string> cells(std::string row) {
  row = trim(row);
  if (starts_with(row, "|")) row = row.substr(1);
  if (ends_with(row, "|") && !ends_with(row, "\\|")) row.pop_back();
  std::vector<std::string> out;
  std::string cur;
  bool code = false;
  for (size_t i = 0; i < row.size(); ++i) {
    char c = row[i];
    if (c == '`') code = !code;
    if (c == '\\' && i + 1 < row.size() && row[i + 1] == '|') { cur += '|'; ++i; continue; }
    if (c == '|' && !code) { out.push_back(trim(cur)); cur.clear(); continue; }
    cur += c;
  }
  out.push_back(trim(cur));
  return out;
}

class Parser {
 public:
  explicit Parser(std::vector<std::string> lines) : l_(std::move(lines)) {}

  std::string blocks() {
    std::string out;
    while (i_ < l_.size()) out += block();
    return out;
  }

 private:
  std::vector<std::string> l_;
  size_t i_ = 0;

  static bool is_hr(const std::string& t) {
    static const Rx re(R"(^ {0,3}((\*\s*){3,}|(-\s*){3,}|(_\s*){3,})$)");
    return re.test(t);
  }
  static std::optional<Rx::Match> list_item(const std::string& t) {
    static const Rx re(R"(^( *)([-*+]|\d+[.)])\s+(.*)$)");
    return re.match(t);
  }
  bool is_table_at(size_t i) const {
    static const Rx sep(R"(^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$)");
    return i + 1 < l_.size() && contains(l_[i], "|") && sep.test(l_[i + 1]) && contains(l_[i + 1], "-");
  }
  bool starts_block(size_t i) const {
    const std::string& t = l_[i];
    std::string s = trim(t);
    return starts_with(s, "#") || starts_with(s, "```") || starts_with(s, "~~~") || starts_with(s, ">") || is_hr(t) ||
           (list_item(t) && indent_of(t) < 4) || is_table_at(i);
  }

  std::string block() {
    const std::string& line = l_[i_];
    if (blank(line)) { ++i_; return ""; }
    std::string t = trim(line);
    static const Rx atx(R"(^(#{1,6})\s+(.*?)\s*#*\s*$)");
    if (auto m = atx.match(t)) {
      ++i_;
      std::string lvl = std::to_string(m->str(1).size());
      return "<h" + lvl + ">" + inline_md(m->str(2)) + "</h" + lvl + ">\n";
    }
    if (starts_with(t, "```") || starts_with(t, "~~~")) return fenced();
    if (is_hr(line)) { ++i_; return "<hr />\n"; }
    if (starts_with(t, ">")) return quote();
    if (is_table_at(i_)) return table();
    if (list_item(line) && indent_of(line) < 4) return list(indent_of(line));
    if (indent_of(line) >= 4) return indented_code();
    static const Rx html(R"(^<(div|table|pre|p|ul|ol|h[1-6]|blockquote|details|section|figure|img|br|hr|!--)\b)", Rx::I);
    if (html.test(t)) {
      std::string out;
      while (i_ < l_.size() && !blank(l_[i_])) out += l_[i_++] + "\n";
      return out;
    }
    return paragraph();
  }

  std::string fenced() {
    std::string t = trim(l_[i_]);
    std::string fence = t.substr(0, 3);
    std::string lang = trim(t.substr(3));
    size_t ind = indent_of(l_[i_]);
    ++i_;
    std::string code;
    while (i_ < l_.size() && !starts_with(trim(l_[i_]), fence)) {
      std::string s = l_[i_++];
      size_t k = 0;
      while (k < ind && k < s.size() && s[k] == ' ') ++k;
      code += s.substr(k) + "\n";
    }
    if (i_ < l_.size()) ++i_;
    std::string cls = lang.empty() ? "" : " class=\"language-" + esc(split_ws(lang)[0]) + "\"";
    return "<pre><code" + cls + ">" + esc(code) + "</code></pre>\n";
  }

  std::string indented_code() {
    std::string code;
    while (i_ < l_.size() && (indent_of(l_[i_]) >= 4 || blank(l_[i_]))) {
      std::string s = l_[i_];
      size_t k = 0, n = 0;
      while (k < s.size() && n < 4 && (s[k] == ' ' || s[k] == '\t')) { n += s[k] == '\t' ? 4 : 1; ++k; }
      code += s.substr(k) + "\n";
      ++i_;
    }
    while (ends_with(code, "\n\n")) code.pop_back();
    return "<pre><code>" + esc(code) + "</code></pre>\n";
  }

  std::string quote() {
    std::vector<std::string> inner;
    while (i_ < l_.size() && !blank(l_[i_])) {
      std::string t = ltrim(l_[i_]);
      if (starts_with(t, ">")) {
        t = t.substr(1);
        if (starts_with(t, " ")) t = t.substr(1);
      }
      inner.push_back(t);
      ++i_;
    }
    return "<blockquote>\n" + Parser(inner).blocks() + "</blockquote>\n";
  }

  std::string table() {
    auto head = cells(l_[i_]);
    auto sep = cells(l_[i_ + 1]);
    std::vector<std::string> align;
    for (auto& c : sep) {
      bool l = starts_with(c, ":"), r = ends_with(c, ":");
      align.push_back(l && r ? "center" : r ? "right" : l ? "left" : "");
    }
    i_ += 2;
    auto td = [&](const std::string& tag, const std::vector<std::string>& row) {
      std::string o = "<tr>\n";
      for (size_t k = 0; k < head.size(); ++k) {
        std::string a = k < align.size() && !align[k].empty() ? " style=\"text-align: " + align[k] + ";\"" : "";
        o += "<" + tag + a + ">" + (k < row.size() ? inline_md(row[k]) : "") + "</" + tag + ">\n";
      }
      return o + "</tr>\n";
    };
    std::string out = "<table>\n<thead>\n" + td("th", head) + "</thead>\n<tbody>\n";
    while (i_ < l_.size() && !blank(l_[i_]) && contains(l_[i_], "|")) out += td("td", cells(l_[i_++]));
    return out + "</tbody>\n</table>\n";
  }

  std::string list(size_t base) {
    auto first = list_item(l_[i_]);
    bool ordered = std::isdigit((unsigned char)first->str(2)[0]);
    std::string out = ordered ? "<ol>\n" : "<ul>\n";
    bool loose = false;
    std::vector<std::vector<std::string>> items;
    while (i_ < l_.size()) {
      auto m = list_item(l_[i_]);
      if (!m || indent_of(l_[i_]) != base) break;
      bool item_ordered = std::isdigit((unsigned char)m->str(2)[0]);
      if (item_ordered != ordered) break;   // sane_lists: otro tipo de lista empieza otra lista
      size_t content = m->start(3);
      std::vector<std::string> body = {m->str(3)};
      ++i_;
      while (i_ < l_.size()) {
        const std::string& s = l_[i_];
        if (blank(s)) {
          // sigue el elemento si lo siguiente esta sangrado; si hay otro elemento, la lista es «suelta»
          size_t j = i_;
          while (j < l_.size() && blank(l_[j])) ++j;
          if (j < l_.size() && indent_of(l_[j]) > base && !(list_item(l_[j]) && indent_of(l_[j]) == base)) {
            body.push_back("");
            i_ = j;
            continue;
          }
          if (j < l_.size() && list_item(l_[j]) && indent_of(l_[j]) == base) loose = true;
          i_ = j;
          break;
        }
        if (indent_of(s) <= base && (list_item(s) || starts_block(i_))) break;
        size_t cut = std::min(indent_of(s), std::max(content, base + 2));
        size_t k = 0, n = 0;
        while (k < s.size() && n < cut && (s[k] == ' ' || s[k] == '\t')) { n += s[k] == '\t' ? 4 : 1; ++k; }
        body.push_back(s.substr(k));
        ++i_;
      }
      items.push_back(body);
      if (i_ < l_.size() && blank(l_[i_])) break;
    }
    for (auto& body : items) {
      bool multi = std::any_of(body.begin(), body.end(), [](const std::string& s) { return blank(s); });
      std::string inner = Parser(body).blocks();
      if (!loose && !multi) {
        // un solo parrafo: sin <p> (lista «apretada»)
        if (starts_with(inner, "<p>")) {
          size_t e = inner.find("</p>\n");
          inner = inner.substr(3, e - 3) + inner.substr(e + 5);
        }
      }
      while (ends_with(inner, "\n")) inner.pop_back();
      out += "<li>" + inner + "</li>\n";
    }
    return out + (ordered ? "</ol>\n" : "</ul>\n");
  }

  std::string paragraph() {
    std::vector<std::string> lines;
    while (i_ < l_.size() && !blank(l_[i_])) {
      if (!lines.empty() && starts_block(i_)) break;
      lines.push_back(l_[i_++]);
    }
    // las lineas se unen antes del formato en linea: la negrita y los enlaces pueden partirse entre lineas
    std::string text;
    for (size_t k = 0; k < lines.size(); ++k) {
      std::string s = lines[k];
      bool br = ends_with(s, "  ") && k + 1 < lines.size();
      text += (k ? "\n" : "") + trim(s) + (br ? "\x03" : "");
    }
    return "<p>" + replace_all(inline_md(text), "\x03", "<br />") + "</p>\n";
  }
};

}  // namespace

std::string markdown_html(const std::string& md) {
  std::vector<std::string> lines = splitlines(replace_all(md, "\r\n", "\n"));
  return Parser(lines).blocks();
}

}  // namespace nc
