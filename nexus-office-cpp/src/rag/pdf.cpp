// Extraccion de texto de PDF sin dependencias (en la version Python, pypdf): objetos y flujos de objetos,
// FlateDecode/ASCIIHex/ASCII85, arbol de paginas, fuentes con ToUnicode, codificaciones WinAnsi/MacRoman con
// Differences y fuentes compuestas (Identity-H). Los PDF cifrados no se admiten.
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <variant>

#include "rag/rag.h"
#include "util/archive.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag {

namespace {

struct Obj;
using ObjP = std::shared_ptr<Obj>;
struct Ref {
  int num = 0, gen = 0;
};
struct Obj {
  enum T { NUL, BOOLV, NUM, STR, NAME, ARR, DICT, REF, STREAM, OP } t = NUL;
  double n = 0;
  bool b = false;
  std::string s;                       // STR (bytes), NAME, OP
  std::vector<ObjP> a;                 // ARR
  std::map<std::string, ObjP> d;       // DICT y STREAM
  Ref r;
  std::string raw;                     // STREAM sin decodificar
  ObjP get(const std::string& k) const {
    auto it = d.find(k);
    return it == d.end() ? nullptr : it->second;
  }
};

ObjP mk(Obj::T t) {
  auto o = std::make_shared<Obj>();
  o->t = t;
  return o;
}

bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
bool is_delim(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '%';
}

// ---------------------------------------------------------------- lexer/parser
struct Lexer {
  const std::string& s;
  size_t i;
  Lexer(const std::string& src, size_t pos = 0) : s(src), i(pos) {}

  void skip_ws() {
    while (i < s.size()) {
      if (is_ws(s[i])) {
        ++i;
      } else if (s[i] == '%') {
        while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
      } else {
        break;
      }
    }
  }

  std::string literal() {   // tras '('
    std::string out;
    int depth = 1;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '\\' && i < s.size()) {
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case '\r':
            if (i < s.size() && s[i] == '\n') ++i;
            break;
          case '\n': break;
          default:
            if (e >= '0' && e <= '7') {
              int v = e - '0';
              for (int k = 0; k < 2 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++k) v = v * 8 + (s[i++] - '0');
              out += (char)v;
            } else {
              out += e;
            }
        }
      } else if (c == '(') {
        ++depth;
        out += c;
      } else if (c == ')') {
        if (--depth == 0) break;
        out += c;
      } else {
        out += c;
      }
    }
    return out;
  }

  std::string hexstr() {   // tras '<'
    std::string hex;
    while (i < s.size() && s[i] != '>') {
      if (isxdigit((unsigned char)s[i])) hex += s[i];
      ++i;
    }
    ++i;
    if (hex.size() % 2) hex += '0';
    std::string out;
    for (size_t k = 0; k + 1 < hex.size(); k += 2) out += (char)std::stoi(hex.substr(k, 2), nullptr, 16);
    return out;
  }

  std::string name() {   // tras '/'
    std::string out;
    while (i < s.size() && !is_ws(s[i]) && !is_delim(s[i])) {
      if (s[i] == '#' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
        out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
        i += 3;
      } else {
        out += s[i++];
      }
    }
    return out;
  }

  // un objeto; en contenidos devuelve OP para los operadores
  ObjP next(int depth = 0) {
    skip_ws();
    if (i >= s.size() || depth > 64) return nullptr;
    char c = s[i];
    if (c == '(') {
      ++i;
      auto o = mk(Obj::STR);
      o->s = literal();
      return o;
    }
    if (c == '<') {
      if (i + 1 < s.size() && s[i + 1] == '<') {
        i += 2;
        auto o = mk(Obj::DICT);
        while (true) {
          skip_ws();
          if (i >= s.size()) break;
          if (s[i] == '>' && i + 1 < s.size() && s[i + 1] == '>') {
            i += 2;
            break;
          }
          auto k = next(depth + 1);
          if (!k) break;
          if (k->t != Obj::NAME) continue;
          auto v = next(depth + 1);
          if (!v) break;
          o->d[k->s] = v;
        }
        return o;
      }
      ++i;
      auto o = mk(Obj::STR);
      o->s = hexstr();
      return o;
    }
    if (c == '[') {
      ++i;
      auto o = mk(Obj::ARR);
      while (true) {
        skip_ws();
        if (i >= s.size()) break;
        if (s[i] == ']') {
          ++i;
          break;
        }
        auto v = next(depth + 1);
        if (!v) break;
        o->a.push_back(v);
      }
      return o;
    }
    if (c == '/') {
      ++i;
      auto o = mk(Obj::NAME);
      o->s = name();
      return o;
    }
    if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
      ++i;
      auto o = mk(Obj::OP);
      o->s = std::string(1, c);
      return o;
    }
    size_t st = i;
    while (i < s.size() && !is_ws(s[i]) && !is_delim(s[i])) ++i;
    std::string tok = s.substr(st, i - st);
    if (tok.empty()) {
      ++i;
      return mk(Obj::NUL);
    }
    bool numeric = true;
    for (char ch : tok)
      if (!(isdigit((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+')) numeric = false;
    if (numeric) {
      auto o = mk(Obj::NUM);
      try {
        o->n = std::stod(tok);
      } catch (...) {
        o->n = 0;
      }
      // referencia "n g R"
      size_t save = i;
      if (tok.find('.') == std::string::npos && tok[0] != '-') {
        skip_ws();
        size_t g0 = i;
        while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        if (i > g0) {
          std::string g = s.substr(g0, i - g0);
          skip_ws();
          if (i < s.size() && s[i] == 'R' && (i + 1 >= s.size() || is_ws(s[i + 1]) || is_delim(s[i + 1]))) {
            ++i;
            auto r = mk(Obj::REF);
            r->r = {(int)o->n, std::stoi(g)};
            return r;
          }
        }
        i = save;
      }
      return o;
    }
    if (tok == "true" || tok == "false") {
      auto o = mk(Obj::BOOLV);
      o->b = tok == "true";
      return o;
    }
    if (tok == "null") return mk(Obj::NUL);
    auto o = mk(Obj::OP);
    o->s = tok;
    return o;
  }
};

// ---------------------------------------------------------------- filtros
std::string ascii_hex(const std::string& in) {
  std::string hex;
  for (char c : in) {
    if (c == '>') break;
    if (isxdigit((unsigned char)c)) hex += c;
  }
  if (hex.size() % 2) hex += '0';
  std::string out;
  for (size_t k = 0; k + 1 < hex.size(); k += 2) out += (char)std::stoi(hex.substr(k, 2), nullptr, 16);
  return out;
}

std::string ascii85(const std::string& in) {
  std::string out;
  uint32_t v = 0;
  int n = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '~') break;
    if (is_ws(c)) continue;
    if (c == 'z' && n == 0) {
      out.append(4, '\0');
      continue;
    }
    if (c < '!' || c > 'u') continue;
    v = v * 85 + (uint32_t)(c - '!');
    if (++n == 5) {
      for (int k = 3; k >= 0; --k) out += (char)((v >> (8 * k)) & 0xff);
      v = 0;
      n = 0;
    }
  }
  if (n > 1) {
    for (int k = n; k < 5; ++k) v = v * 85 + 84;
    for (int k = 3; k >= 5 - n; --k) out += (char)((v >> (8 * k)) & 0xff);
  }
  return out;
}

std::string png_unpredict(const std::string& data, int columns, int colors, int bpc) {
  int bpp = std::max(1, colors * bpc / 8);
  int row = (columns * colors * bpc + 7) / 8;
  std::string out, prev(row, '\0');
  for (size_t p = 0; p + row + 1 <= data.size(); p += row + 1) {
    unsigned char ft = (unsigned char)data[p];
    std::string cur = data.substr(p + 1, row);
    for (int i = 0; i < row; ++i) {
      int a = i >= bpp ? (unsigned char)cur[i - bpp] : 0, b = (unsigned char)prev[i],
          c = i >= bpp ? (unsigned char)prev[i - bpp] : 0;
      int x = (unsigned char)cur[i];
      switch (ft) {
        case 1: x += a; break;
        case 2: x += b; break;
        case 3: x += (a + b) / 2; break;
        case 4: {
          int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
          x += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
          break;
        }
        default: break;
      }
      cur[i] = (char)(x & 0xff);
    }
    out += cur;
    prev = cur;
  }
  return out;
}

// ---------------------------------------------------------------- documento
struct Pdf {
  std::string data;
  std::map<int, size_t> offsets;                       // objeto -> posicion de "n g obj"
  std::map<int, std::pair<int, int>> in_stream;        // objeto -> (flujo de objetos, indice)
  std::map<int, ObjP> cache;
  std::set<int> resolving;

  explicit Pdf(std::string d) : data(std::move(d)) { scan(); }

  void scan() {
    static const Rx objrx(R"((?<![0-9])(\d+)\s+(\d+)\s+obj\b)");
    for (auto& m : objrx.finditer(data)) offsets[std::stoi(m.str(1))] = m.start();
    // flujos de objetos (PDF 1.5+)
    std::vector<int> streams;
    for (auto& [num, off] : offsets) {
      size_t look = data.find("/ObjStm", off);
      size_t endobj = data.find("endobj", off);
      if (look != std::string::npos && (endobj == std::string::npos || look < endobj)) streams.push_back(num);
    }
    for (int sn : streams) {
      ObjP so = object(sn);
      if (!so || so->t != Obj::STREAM) continue;
      std::string body = decode(so);
      int n = (int)num(so->get("N")), first = (int)num(so->get("First"));
      Lexer lx(body);
      std::vector<std::pair<int, int>> heads;
      for (int k = 0; k < n; ++k) {
        auto a = lx.next(), b = lx.next();
        if (!a || !b) break;
        heads.emplace_back((int)a->n, (int)b->n);
      }
      for (size_t k = 0; k < heads.size(); ++k) {
        int on = heads[k].first;
        if (!offsets.count(on) && !in_stream.count(on)) in_stream[on] = {sn, (int)k};
        size_t pos = (size_t)first + heads[k].second;
        if (pos < body.size() && !cache.count(on) && !offsets.count(on)) {
          Lexer ol(body, pos);
          if (auto o = ol.next()) cache[on] = o;
        }
      }
    }
  }

  double num(ObjP o) {
    o = res(o);
    return o && o->t == Obj::NUM ? o->n : 0;
  }

  ObjP object(int n) {
    auto c = cache.find(n);
    if (c != cache.end()) return c->second;
    auto it = offsets.find(n);
    if (it == offsets.end() || resolving.count(n)) return nullptr;
    resolving.insert(n);
    Lexer lx(data, it->second);
    lx.next();
    lx.next();
    lx.next();   // "n g obj"
    ObjP o = lx.next();
    if (o && o->t == Obj::DICT) {
      size_t save = lx.i;
      lx.skip_ws();
      if (data.compare(lx.i, 6, "stream") == 0) {
        size_t p = lx.i + 6;
        if (p < data.size() && data[p] == '\r') ++p;
        if (p < data.size() && data[p] == '\n') ++p;
        o->t = Obj::STREAM;
        size_t len = (size_t)std::max(0.0, num(o->get("Length")));
        if (len) {   // /Length erronea: se busca endstream
          size_t after = p + len <= data.size() ? data.find("endstream", p + len) : std::string::npos;
          if (after == std::string::npos || after > p + len + 32) len = 0;
        }
        if (!len) {
          size_t e = data.find("endstream", p);
          len = e == std::string::npos ? data.size() - p : e - p;
          while (len && (data[p + len - 1] == '\n' || data[p + len - 1] == '\r')) --len;
        }
        o->raw = data.substr(p, std::min(len, data.size() - p));
      } else {
        lx.i = save;
      }
    }
    resolving.erase(n);
    cache[n] = o;
    return o;
  }

  ObjP res(ObjP o, int depth = 0) {
    while (o && o->t == Obj::REF && depth++ < 32) o = object(o->r.num);
    return o;
  }

  std::string decode(ObjP so) {
    std::string d = so->raw;
    ObjP f = res(so->get("Filter"));
    std::vector<std::string> filters;
    if (f && f->t == Obj::NAME) filters.push_back(f->s);
    if (f && f->t == Obj::ARR)
      for (auto& x : f->a)
        if (auto r = res(x); r && r->t == Obj::NAME) filters.push_back(r->s);
    ObjP parms = res(so->get("DecodeParms"));
    if (parms && parms->t == Obj::ARR) parms = parms->a.empty() ? nullptr : res(parms->a[0]);
    for (auto& name : filters) {
      if (name == "FlateDecode" || name == "Fl") {
        auto out = archive::inflate(d, true);
        if (!out) out = archive::inflate(d, false);
        if (!out) return "";
        d = *out;
        if (parms && parms->t == Obj::DICT) {
          int pred = (int)num(parms->get("Predictor"));
          if (pred >= 10) {
            int cols = parms->get("Columns") ? (int)num(parms->get("Columns")) : 1;
            int colors = parms->get("Colors") ? (int)num(parms->get("Colors")) : 1;
            int bpc = parms->get("BitsPerComponent") ? (int)num(parms->get("BitsPerComponent")) : 8;
            d = png_unpredict(d, cols, colors, bpc);
          }
        }
      } else if (name == "ASCIIHexDecode" || name == "AHx") {
        d = ascii_hex(d);
      } else if (name == "ASCII85Decode" || name == "A85") {
        d = ascii85(d);
      } else {
        return "";   // DCT, JBIG2, LZW...: no es texto
      }
    }
    return d;
  }

  ObjP trailer_value(const std::string& key) {
    size_t p = data.rfind("trailer");
    while (p != std::string::npos) {
      Lexer lx(data, p + 7);
      ObjP t = lx.next();
      if (t && t->t == Obj::DICT && t->get(key)) return t->get(key);
      if (p == 0) break;
      p = data.rfind("trailer", p - 1);
    }
    // xref en flujo: el diccionario del flujo XRef hace de trailer
    for (auto& [n, off] : offsets) {
      size_t endobj = data.find("endobj", off);
      size_t x = data.find("/XRef", off);
      if (x != std::string::npos && (endobj == std::string::npos || x < endobj)) {
        ObjP o = object(n);
        if (o && o->get(key)) return o->get(key);
      }
    }
    return nullptr;
  }
};

// ---------------------------------------------------------------- fuentes
const std::map<std::string, char32_t>& glyph_names() {
  static const std::map<std::string, char32_t> G = [] {
    std::map<std::string, char32_t> g = {
        {"space", ' '},       {"exclam", '!'},       {"quotedbl", '"'},      {"numbersign", '#'},
        {"dollar", '$'},      {"percent", '%'},      {"ampersand", '&'},     {"quotesingle", '\''},
        {"quoteright", 0x2019}, {"quoteleft", 0x2018}, {"parenleft", '('},   {"parenright", ')'},
        {"asterisk", '*'},    {"plus", '+'},         {"comma", ','},         {"hyphen", '-'},
        {"period", '.'},      {"slash", '/'},        {"colon", ':'},         {"semicolon", ';'},
        {"less", '<'},        {"equal", '='},        {"greater", '>'},       {"question", '?'},
        {"at", '@'},          {"bracketleft", '['},  {"backslash", '\\'},    {"bracketright", ']'},
        {"asciicircum", '^'}, {"underscore", '_'},   {"grave", '`'},         {"braceleft", '{'},
        {"bar", '|'},         {"braceright", '}'},   {"asciitilde", '~'},    {"bullet", 0x2022},
        {"endash", 0x2013},   {"emdash", 0x2014},    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D},
        {"ellipsis", 0x2026}, {"fi", 0xFB01},        {"fl", 0xFB02},         {"Euro", 0x20AC},
        {"exclamdown", 0xA1}, {"questiondown", 0xBF}, {"ordfeminine", 0xAA}, {"ordmasculine", 0xBA},
        {"degree", 0xB0},     {"copyright", 0xA9},   {"registered", 0xAE},   {"trademark", 0x2122},
        {"guillemotleft", 0xAB}, {"guillemotright", 0xBB}, {"periodcentered", 0xB7}, {"section", 0xA7},
        {"paragraph", 0xB6},  {"minus", 0x2212},     {"nbspace", 0xA0},      {"sterling", 0xA3},
        {"germandbls", 0xDF}, {"zero", '0'}, {"one", '1'}, {"two", '2'}, {"three", '3'}, {"four", '4'},
        {"five", '5'}, {"six", '6'}, {"seven", '7'}, {"eight", '8'}, {"nine", '9'}};
    for (char c = 'a'; c <= 'z'; ++c) g[std::string(1, c)] = (char32_t)c;
    for (char c = 'A'; c <= 'Z'; ++c) g[std::string(1, c)] = (char32_t)c;
    struct A {
      const char* suf;
      const char* lo;
      const char* up;
    };
    // vocales y letras acentuadas latinas
    const std::vector<std::pair<std::string, char32_t>> acc = {
        {"aacute", 0xE1}, {"eacute", 0xE9}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA},
        {"Aacute", 0xC1}, {"Eacute", 0xC9}, {"Iacute", 0xCD}, {"Oacute", 0xD3}, {"Uacute", 0xDA},
        {"agrave", 0xE0}, {"egrave", 0xE8}, {"igrave", 0xEC}, {"ograve", 0xF2}, {"ugrave", 0xF9},
        {"Agrave", 0xC0}, {"Egrave", 0xC8}, {"Igrave", 0xCC}, {"Ograve", 0xD2}, {"Ugrave", 0xD9},
        {"adieresis", 0xE4}, {"edieresis", 0xEB}, {"idieresis", 0xEF}, {"odieresis", 0xF6}, {"udieresis", 0xFC},
        {"Adieresis", 0xC4}, {"Edieresis", 0xCB}, {"Idieresis", 0xCF}, {"Odieresis", 0xD6}, {"Udieresis", 0xDC},
        {"acircumflex", 0xE2}, {"ecircumflex", 0xEA}, {"icircumflex", 0xEE}, {"ocircumflex", 0xF4},
        {"ucircumflex", 0xFB}, {"ntilde", 0xF1}, {"Ntilde", 0xD1}, {"ccedilla", 0xE7}, {"Ccedilla", 0xC7},
        {"atilde", 0xE3}, {"otilde", 0xF5}, {"Atilde", 0xC3}, {"Otilde", 0xD5}, {"aring", 0xE5}, {"Aring", 0xC5},
        {"ae", 0xE6}, {"AE", 0xC6}, {"oslash", 0xF8}, {"Oslash", 0xD8}, {"dotlessi", 0x131}};
    for (auto& [k, v] : acc) g[k] = v;
    return g;
  }();
  return G;
}

char32_t glyph(const std::string& name) {
  auto& g = glyph_names();
  auto it = g.find(name);
  if (it != g.end()) return it->second;
  if (starts_with(name, "uni") && name.size() >= 7) {
    try {
      return (char32_t)std::stoul(name.substr(3, 4), nullptr, 16);
    } catch (...) {
    }
  }
  if (name.size() > 1 && name[0] == 'u' && isxdigit((unsigned char)name[1])) {
    try {
      return (char32_t)std::stoul(name.substr(1), nullptr, 16);
    } catch (...) {
    }
  }
  return 0;
}

std::u32string mac_roman_hi() {
  // 0x80..0xFF de MacRoman
  static const char32_t t[128] = {
      0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
      0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
      0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
      0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
      0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
      0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A,
      0xFB01, 0xFB02, 0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC,
      0xD3, 0xD4, 0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD,
      0x2DB, 0x2C7};
  return std::u32string(t, t + 128);
}

struct Font {
  int bytes = 1;                                   // 2 en fuentes compuestas (Type0)
  std::map<uint32_t, std::u32string> tounicode;
  std::map<int, char32_t> diff;
  std::string base = "WinAnsi";
  int cs_bytes = 0;                                // codespace de la CMap (si lo declara)

  std::u32string decode(const std::string& s) const {
    std::u32string out;
    int nb = cs_bytes ? cs_bytes : bytes;
    for (size_t i = 0; i < s.size();) {
      uint32_t code = 0;
      int k = 0;
      for (; k < nb && i + k < s.size(); ++k) code = (code << 8) | (unsigned char)s[i + k];
      i += k;
      auto it = tounicode.find(code);
      if (it != tounicode.end()) {
        out += it->second;
        continue;
      }
      if (nb > 1) continue;   // sin ToUnicode no se puede saber (glifos de una fuente incrustada)
      auto d = diff.find((int)code);
      if (d != diff.end() && d->second) {
        out += d->second;
        continue;
      }
      if (base == "MacRoman" && code >= 0x80) {
        static const std::u32string hi = mac_roman_hi();
        out += hi[code - 0x80];
      } else if (code >= 0x80 && code < 0xA0) {
        out += u32(cp1252_to_utf8(std::string(1, (char)code)));
      } else if (code == 0x60 && base == "Standard") {
        out += (char32_t)0x2018;
      } else if (code == 0x27 && base == "Standard") {
        out += (char32_t)0x2019;
      } else {
        out += (char32_t)code;
      }
    }
    return out;
  }
};

std::u32string utf16be(const std::string& b) {
  std::u32string out;
  for (size_t i = 0; i + 1 < b.size(); i += 2) {
    char32_t c = ((unsigned char)b[i] << 8) | (unsigned char)b[i + 1];
    if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.size()) {
      char32_t lo = ((unsigned char)b[i + 2] << 8) | (unsigned char)b[i + 3];
      c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
      i += 2;
    }
    out += c;
  }
  return out;
}

void parse_cmap(const std::string& cm, Font& f) {
  Lexer lx(cm);
  std::vector<ObjP> st;
  while (auto o = lx.next()) {
    if (o->t != Obj::OP) {
      st.push_back(o);
      continue;
    }
    const std::string& op = o->s;
    if (op == "begincodespacerange") {
      auto a = lx.next();
      if (a && a->t == Obj::STR) f.cs_bytes = (int)a->s.size();
      while (auto x = lx.next())
        if (x->t == Obj::OP && x->s == "endcodespacerange") break;
    } else if (op == "beginbfchar") {
      while (true) {
        auto a = lx.next();
        if (!a || (a->t == Obj::OP && a->s == "endbfchar")) break;
        auto b = lx.next();
        if (!b) break;
        if (a->t == Obj::STR && b->t == Obj::STR) {
          uint32_t code = 0;
          for (unsigned char c : a->s) code = (code << 8) | c;
          f.tounicode[code] = utf16be(b->s);
        }
      }
    } else if (op == "beginbfrange") {
      while (true) {
        auto a = lx.next();
        if (!a || (a->t == Obj::OP && a->s == "endbfrange")) break;
        auto b = lx.next(), c = lx.next();
        if (!b || !c || a->t != Obj::STR || b->t != Obj::STR) continue;
        uint32_t lo = 0, hi = 0;
        for (unsigned char x : a->s) lo = (lo << 8) | x;
        for (unsigned char x : b->s) hi = (hi << 8) | x;
        if (hi < lo || hi - lo > 65535) continue;
        if (c->t == Obj::STR) {
          std::u32string base = utf16be(c->s);
          for (uint32_t code = lo; code <= hi && !base.empty(); ++code) {
            std::u32string v = base;
            v.back() += (char32_t)(code - lo);
            f.tounicode[code] = v;
          }
        } else if (c->t == Obj::ARR) {
          for (size_t k = 0; k < c->a.size() && lo + k <= hi; ++k)
            if (c->a[k]->t == Obj::STR) f.tounicode[lo + (uint32_t)k] = utf16be(c->a[k]->s);
        }
      }
    }
    st.clear();
  }
}

Font load_font(Pdf& pdf, ObjP fo) {
  Font f;
  fo = pdf.res(fo);
  if (!fo || (fo->t != Obj::DICT && fo->t != Obj::STREAM)) return f;
  ObjP sub = pdf.res(fo->get("Subtype"));
  if (sub && sub->t == Obj::NAME && sub->s == "Type0") f.bytes = 2;
  ObjP tu = pdf.res(fo->get("ToUnicode"));
  if (tu && tu->t == Obj::STREAM) parse_cmap(pdf.decode(tu), f);
  ObjP enc = pdf.res(fo->get("Encoding"));
  if (enc && enc->t == Obj::NAME) {
    if (enc->s == "MacRomanEncoding") f.base = "MacRoman";
    if (enc->s == "StandardEncoding") f.base = "Standard";
  } else if (enc && enc->t == Obj::DICT) {
    ObjP be = pdf.res(enc->get("BaseEncoding"));
    if (be && be->t == Obj::NAME && be->s == "MacRomanEncoding") f.base = "MacRoman";
    ObjP diffs = pdf.res(enc->get("Differences"));
    if (diffs && diffs->t == Obj::ARR) {
      int code = 0;
      for (auto& x : diffs->a) {
        if (x->t == Obj::NUM) {
          code = (int)x->n;
        } else if (x->t == Obj::NAME) {
          f.diff[code++] = glyph(x->s);
        }
      }
    }
  }
  return f;
}

// ---------------------------------------------------------------- contenido de una pagina
struct TextOut {
  std::u32string out;
  double last_y = NAN, last_x_end = NAN;
  void newline() {
    if (!out.empty() && out.back() != U'\n') out += U'\n';
  }
  void space() {
    if (!out.empty() && out.back() != U' ' && out.back() != U'\n') out += U' ';
  }
};

void run_content(Pdf& pdf, const std::string& content, ObjP resources, TextOut& to, int depth) {
  if (depth > 6) return;
  std::map<std::string, Font> fonts;
  ObjP fdict = resources ? pdf.res(resources->get("Font")) : nullptr;
  ObjP xobj = resources ? pdf.res(resources->get("XObject")) : nullptr;
  Font* cur = nullptr;
  Font plain;
  double tx = 0, ty = 0, leading = 0, tf = 12, scale = 1, fsize = 12;
  Lexer lx(content);
  std::vector<ObjP> st;
  auto show = [&](const std::string& s) {
    Font& f = cur ? *cur : plain;
    to.out += f.decode(s);
  };
  auto moved = [&](double nx, double ny) {
    if (!std::isnan(to.last_y) && std::fabs(ny - to.last_y) > std::max(1.0, fsize * 0.5))
      to.newline();
    else if (!std::isnan(to.last_x_end) && nx > to.last_x_end + fsize * 0.15)
      to.space();
    to.last_y = ny;
    tx = nx;
    ty = ny;
  };
  while (auto o = lx.next()) {
    if (o->t != Obj::OP) {
      st.push_back(o);
      if (st.size() > 64) st.erase(st.begin());
      continue;
    }
    const std::string& op = o->s;
    auto n = [&](size_t back) { return st.size() >= back && st[st.size() - back]->t == Obj::NUM ? st[st.size() - back]->n : 0.0; };
    if (op == "BI") {   // imagen en linea: saltar hasta EI
      size_t e = content.find("EI", lx.i);
      while (e != std::string::npos && !(is_ws(content[e - 1]) && (e + 2 >= content.size() || is_ws(content[e + 2]))))
        e = content.find("EI", e + 2);
      lx.i = e == std::string::npos ? content.size() : e + 2;
    } else if (op == "BT") {
      tx = ty = 0;
    } else if (op == "ET") {
      to.last_x_end = NAN;
    } else if (op == "Tf" && st.size() >= 2) {
      ObjP nm = st[st.size() - 2];
      if (std::fabs(n(1)) > 0) tf = std::fabs(n(1));
      fsize = tf * scale;
      if (nm->t == Obj::NAME) {
        auto it = fonts.find(nm->s);
        if (it == fonts.end() && fdict && (fdict->t == Obj::DICT))
          it = fonts.emplace(nm->s, load_font(pdf, fdict->get(nm->s))).first;
        cur = it == fonts.end() ? nullptr : &it->second;
      }
    } else if (op == "TL") {
      leading = n(1);
    } else if (op == "Td") {
      moved(tx + n(2), ty + n(1));
    } else if (op == "TD") {
      leading = -n(1);
      moved(tx + n(2), ty + n(1));
    } else if (op == "Tm") {
      scale = std::fabs(n(3)) > 0 ? std::fabs(n(3)) : 1;
      fsize = tf * scale;
      moved(n(2), n(1));
    } else if (op == "T*") {
      moved(tx, ty - (leading ? leading : fsize));
    } else if (op == "Tj" && !st.empty() && st.back()->t == Obj::STR) {
      show(st.back()->s);
      to.last_x_end = tx;
    } else if ((op == "'" || op == "\"") && !st.empty() && st.back()->t == Obj::STR) {
      moved(tx, ty - (leading ? leading : fsize));
      show(st.back()->s);
    } else if (op == "TJ" && !st.empty() && st.back()->t == Obj::ARR) {
      for (auto& x : st.back()->a) {
        if (x->t == Obj::STR)
          show(x->s);
        else if (x->t == Obj::NUM && x->n < -200)
          to.space();
      }
      to.last_x_end = tx;
    } else if (op == "Do" && !st.empty() && st.back()->t == Obj::NAME && xobj && xobj->t == Obj::DICT) {
      ObjP x = pdf.res(xobj->get(st.back()->s));
      ObjP sub = x ? pdf.res(x->get("Subtype")) : nullptr;
      if (x && x->t == Obj::STREAM && sub && sub->t == Obj::NAME && sub->s == "Form") {
        ObjP r = pdf.res(x->get("Resources"));
        run_content(pdf, pdf.decode(x), r ? r : resources, to, depth + 1);
      }
    }
    st.clear();
  }
}

void collect_pages(Pdf& pdf, ObjP node, ObjP inherited_res, std::vector<std::pair<ObjP, ObjP>>& pages, int depth,
                   std::set<Obj*>& seen) {
  node = pdf.res(node);
  if (!node || depth > 64 || seen.count(node.get())) return;
  seen.insert(node.get());
  ObjP r = pdf.res(node->get("Resources"));
  if (!r) r = inherited_res;
  ObjP kids = pdf.res(node->get("Kids"));
  ObjP type = pdf.res(node->get("Type"));
  if (kids && kids->t == Obj::ARR && !(type && type->t == Obj::NAME && type->s == "Page")) {
    for (auto& k : kids->a) collect_pages(pdf, k, r, pages, depth + 1, seen);
  } else {
    pages.emplace_back(node, r);
  }
}

std::string pdf_string(const std::string& s) {
  if (s.size() >= 2 && (unsigned char)s[0] == 0xFE && (unsigned char)s[1] == 0xFF) return u8(utf16be(s.substr(2)));
  if (s.size() >= 3 && starts_with(s, "\xEF\xBB\xBF")) return s.substr(3);
  return cp1252_to_utf8(s);
}

}  // namespace

Doc read_pdf(const std::string& bytes, const std::string& fallback_title) {
  if (bytes.substr(0, 1024).find("%PDF") == std::string::npos)
    throw std::runtime_error("PdfReadError: el fichero no es un PDF");
  Pdf pdf(bytes);
  if (pdf.trailer_value("Encrypt")) throw std::runtime_error("PDF cifrado: no se puede leer sin la contrasena");
  std::string title;
  if (ObjP info = pdf.res(pdf.trailer_value("Info")); info && info->t == Obj::DICT)
    if (ObjP t = pdf.res(info->get("Title")); t && t->t == Obj::STR) title = trim(valid_utf8(pdf_string(t->s)));
  std::vector<std::pair<ObjP, ObjP>> pages;
  std::set<Obj*> seen;
  if (ObjP root = pdf.res(pdf.trailer_value("Root")); root && root->t == Obj::DICT)
    collect_pages(pdf, root->get("Pages"), nullptr, pages, 0, seen);
  if (pages.empty()) {   // sin arbol legible: todas las /Type /Page en orden de objeto
    for (auto& [n, _] : pdf.offsets) {
      ObjP o = pdf.object(n);
      if (!o || (o->t != Obj::DICT && o->t != Obj::STREAM)) continue;
      ObjP t = pdf.res(o->get("Type"));
      if (t && t->t == Obj::NAME && t->s == "Page") pages.emplace_back(o, pdf.res(o->get("Resources")));
    }
  }
  std::vector<std::string> out;
  for (auto& [page, resources] : pages) {
    std::string content;
    ObjP c = pdf.res(page->get("Contents"));
    if (c && c->t == Obj::STREAM) content = pdf.decode(c);
    if (c && c->t == Obj::ARR)
      for (auto& part : c->a)
        if (ObjP s = pdf.res(part); s && s->t == Obj::STREAM) content += pdf.decode(s) + "\n";
    TextOut to;
    try {
      run_content(pdf, content, resources, to, 0);
    } catch (const std::exception&) {
    }
    std::string text = u8(to.out);
    static const Rx trailing(R"([ \t]+\n)");
    out.push_back(trailing.sub(valid_utf8(text), "\n"));
  }
  return {title.empty() ? fallback_title : title, out};
}

}  // namespace nx::rag
