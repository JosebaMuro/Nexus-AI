#include "util/text.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace nx {

// ---------------------------------------------------------------- numeros
double round_to(double v, int decimals) {
  double m = std::pow(10.0, decimals);
  double r = std::nearbyint(v * m) / m;   // redondeo bancario como round() de Python
  return r == 0 ? 0.0 : r;
}

std::string fnum(double v, int decimals) {
  double r = round_to(v, decimals);
  if (std::fabs(r - std::round(r)) < 1e-12 && std::fabs(r) < 1e15) return std::to_string((long long)std::llround(r));
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, r);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

std::string pyfloat(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  std::string s = buf;
  if (s.find_first_of(".einn") == std::string::npos) s += ".0";
  return s;
}

// ---------------------------------------------------------------- UTF-8
std::u32string u32(std::string_view s) {
  std::u32string out;
  out.reserve(s.size());
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    char32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
    else { out.push_back(0xFFFD); ++i; continue; }
    if (i + len > n) { out.push_back(0xFFFD); ++i; continue; }
    bool ok = true;
    for (int k = 1; k < len; ++k) {
      unsigned char cc = (unsigned char)s[i + k];
      if ((cc >> 6) != 2) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += len;
  }
  return out;
}

std::string u8(char32_t c) {
  std::string o;
  if (c < 0x80) o += (char)c;
  else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) {
    o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F));
  } else {
    o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F));
    o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F));
  }
  return o;
}

std::string u8(std::u32string_view s) {
  std::string o;
  o.reserve(s.size());
  for (char32_t c : s) o += u8(c);
  return o;
}

size_t ulen(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

std::string usub(std::string_view s, long start, long stop) {
  // camino rapido: solo ASCII
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) { ascii = false; break; }
  long n = ascii ? (long)s.size() : (long)ulen(s);
  if (start < 0) start = std::max(0L, n + start);
  if (stop < 0) stop = std::max(0L, n + stop);
  if (stop > n) stop = n;
  if (start >= stop) return "";
  if (ascii) return std::string(s.substr(start, stop - start));
  auto u = u32(s);
  return u8(std::u32string_view(u).substr(start, stop - start));
}

std::string uleft(std::string_view s, size_t n) {
  if (s.size() <= n) return std::string(s);
  return usub(s, 0, (long)n);
}

std::string uright(std::string_view s, size_t n) {
  size_t len = ulen(s);
  if (len <= n) return std::string(s);
  return usub(s, (long)(len - n));
}

std::string valid_utf8(std::string_view s) {
  bool ok = true;
  for (unsigned char c : s)
    if (c >= 0x80) { ok = false; break; }
  if (ok) return std::string(s);
  return u8(u32(s));
}

std::string latin1_to_utf8(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (unsigned char c : s) o += u8((char32_t)c);
  return o;
}

std::string cp1252_to_utf8(std::string_view s) {
  static const char32_t hi[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
  std::string o;
  o.reserve(s.size());
  for (unsigned char c : s) o += u8(c >= 0x80 && c < 0xA0 ? hi[c - 0x80] : (char32_t)c);
  return o;
}

std::string utf16_to_utf8(std::string_view b, bool le) {
  std::u32string out;
  size_t i = 0;
  if (b.size() >= 2) {
    unsigned char a = b[0], c = b[1];
    if ((a == 0xFF && c == 0xFE) || (a == 0xFE && c == 0xFF)) { le = a == 0xFF; i = 2; }
  }
  auto rd = [&](size_t k) -> char32_t {
    unsigned char x = b[k], y = b[k + 1];
    return le ? (char32_t)(x | (y << 8)) : (char32_t)((x << 8) | y);
  };
  for (; i + 1 < b.size(); i += 2) {
    char32_t w = rd(i);
    if (w >= 0xD800 && w < 0xDC00 && i + 3 < b.size()) {
      char32_t w2 = rd(i + 2);
      if (w2 >= 0xDC00 && w2 < 0xE000) {
        out.push_back(0x10000 + ((w - 0xD800) << 10) + (w2 - 0xDC00));
        i += 2;
        continue;
      }
    }
    out.push_back(w);
  }
  return u8(out);
}

// ---------------------------------------------------------------- mayusculas y acentos
char32_t lower_cp(char32_t c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
  if (c >= 0x100 && c <= 0x137) return (c % 2 == 0) ? c + 1 : c;
  if (c >= 0x139 && c <= 0x148) return (c % 2 == 1) ? c + 1 : c;
  if (c >= 0x14A && c <= 0x177) return (c % 2 == 0) ? c + 1 : c;
  if (c == 0x178) return 0xFF;
  if (c == 0x179 || c == 0x17B || c == 0x17D) return c + 1;
  if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}

char32_t upper_cp(char32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
  if (c == 0xFF) return 0x178;
  if (c >= 0x100 && c <= 0x137) return (c % 2 == 1) ? c - 1 : c;
  if (c >= 0x139 && c <= 0x148) return (c % 2 == 0) ? c - 1 : c;
  if (c >= 0x14A && c <= 0x177) return (c % 2 == 1) ? c - 1 : c;
  if (c == 0x17A || c == 0x17C || c == 0x17E) return c - 1;
  if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return c - 32;
  if (c >= 0x430 && c <= 0x44F) return c - 32;
  if (c >= 0x450 && c <= 0x45F) return c - 80;
  return c;
}

std::string lower(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 0x80) { ascii = false; break; }
  if (ascii) {
    for (char c : s) o += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
    return o;
  }
  for (char32_t c : u32(s)) o += u8(lower_cp(c));
  return o;
}

std::string upper(std::string_view s) {
  std::string o;
  for (char32_t c : u32(s)) o += u8(upper_cp(c));
  return o;
}

static char32_t fold_cp(char32_t c) {
  // NFKD sin marcas combinantes (generado con unicodedata para U+00C0..U+024F en minusculas)
  static const std::pair<char32_t, char> tbl[] = {
      {0xE0,'a'},{0xE1,'a'},{0xE2,'a'},{0xE3,'a'},{0xE4,'a'},{0xE5,'a'},{0xE7,'c'},{0xE8,'e'},{0xE9,'e'},{0xEA,'e'},
      {0xEB,'e'},{0xEC,'i'},{0xED,'i'},{0xEE,'i'},{0xEF,'i'},{0xF1,'n'},{0xF2,'o'},{0xF3,'o'},{0xF4,'o'},{0xF5,'o'},
      {0xF6,'o'},{0xF9,'u'},{0xFA,'u'},{0xFB,'u'},{0xFC,'u'},{0xFD,'y'},{0xFF,'y'},{0x101,'a'},{0x103,'a'},{0x105,'a'},
      {0x107,'c'},{0x109,'c'},{0x10B,'c'},{0x10D,'c'},{0x10F,'d'},{0x113,'e'},{0x115,'e'},{0x117,'e'},{0x119,'e'},
      {0x11B,'e'},{0x11D,'g'},{0x11F,'g'},{0x121,'g'},{0x123,'g'},{0x125,'h'},{0x129,'i'},{0x12B,'i'},{0x12D,'i'},
      {0x12F,'i'},{0x135,'j'},{0x137,'k'},{0x13A,'l'},{0x13C,'l'},{0x13E,'l'},{0x144,'n'},{0x146,'n'},{0x148,'n'},
      {0x14D,'o'},{0x14F,'o'},{0x151,'o'},{0x155,'r'},{0x157,'r'},{0x159,'r'},{0x15B,'s'},{0x15D,'s'},{0x15F,'s'},
      {0x161,'s'},{0x163,'t'},{0x165,'t'},{0x169,'u'},{0x16B,'u'},{0x16D,'u'},{0x16F,'u'},{0x171,'u'},{0x173,'u'},
      {0x175,'w'},{0x177,'y'},{0x17A,'z'},{0x17C,'z'},{0x17E,'z'},{0x17F,'s'},{0x1A1,'o'},{0x1B0,'u'},{0x1CE,'a'},
      {0x1D0,'i'},{0x1D2,'o'},{0x1D4,'u'},{0x1D6,'u'},{0x1D8,'u'},{0x1DA,'u'},{0x1DC,'u'},{0x1DF,'a'},{0x1E1,'a'},
      {0x1E7,'g'},{0x1E9,'k'},{0x1EB,'o'},{0x1ED,'o'},{0x1F0,'j'},{0x1F5,'g'},{0x1F9,'n'},{0x1FB,'a'},{0x201,'a'},
      {0x203,'a'},{0x205,'e'},{0x207,'e'},{0x209,'i'},{0x20B,'i'},{0x20D,'o'},{0x20F,'o'},{0x211,'r'},{0x213,'r'},
      {0x215,'u'},{0x217,'u'},{0x219,'s'},{0x21B,'t'},{0x21F,'h'},{0x227,'a'},{0x229,'e'},{0x22B,'o'},{0x22D,'o'},
      {0x22F,'o'},{0x231,'o'},{0x233,'y'}};
  if (c < 0x80) return c;
  if (c == 0xAA) return 'a';
  if (c == 0xBA) return 'o';
  if (c == 0xB2) return '2';
  if (c == 0xB3) return '3';
  if (c == 0xB9) return '1';
  if (c >= 0x300 && c <= 0x36F) return 0;   // marcas combinantes
  if (c == 0xFB01) return 0x10001;          // ligadura fi (se expande abajo)
  if (c == 0xFB02) return 0x10002;          // ligadura fl
  if (c >= 0xE0 && c <= 0x233) {
    size_t lo = 0, hi = sizeof(tbl) / sizeof(tbl[0]);
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (tbl[mid].first < c) lo = mid + 1;
      else hi = mid;
    }
    if (lo < sizeof(tbl) / sizeof(tbl[0]) && tbl[lo].first == c) return (char32_t)tbl[lo].second;
  }
  return c;
}

std::string strip_accents(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (char32_t c : u32(s)) {
    char32_t f = fold_cp(c);
    if (f == 0) continue;
    if (f == 0x10001) { o += "fi"; continue; }
    if (f == 0x10002) { o += "fl"; continue; }
    o += u8(f);
  }
  return o;
}

std::string normalize(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (char32_t c : u32(s)) {
    char32_t f = fold_cp(lower_cp(c));
    if (f == 0) continue;
    if (f == 0x10001) { o += "fi"; continue; }
    if (f == 0x10002) { o += "fl"; continue; }
    o += u8(f);
  }
  return o;
}

bool is_alpha_cp(char32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  if (c == 0xAA || c == 0xB5 || c == 0xBA) return true;
  if (c >= 0xC0 && c <= 0x24F) return c != 0xD7 && c != 0xF7;
  if (c >= 0x250 && c <= 0x2AF) return true;
  if (c >= 0x370 && c <= 0x3FF) return c != 0x37E && c != 0x387;
  if (c >= 0x400 && c <= 0x52F) return !(c >= 0x482 && c <= 0x489);
  if (c >= 0x531 && c <= 0x587) return true;
  if (c >= 0x5D0 && c <= 0x5EA) return true;
  if (c >= 0x620 && c <= 0x64A) return true;
  if (c >= 0x900 && c <= 0x0DFF) return true;   // escrituras indicas (aproximado)
  if (c >= 0xE00 && c <= 0xE7F) return true;
  if (c >= 0x1E00 && c <= 0x1FFF) return true;
  if (c >= 0x3040 && c <= 0x30FF) return true;
  if (c >= 0x3400 && c <= 0x9FFF) return true;
  if (c >= 0xAC00 && c <= 0xD7AF) return true;
  if (c >= 0xF900 && c <= 0xFAFF) return true;
  if (c >= 0xFB00 && c <= 0xFB06) return true;
  if (c >= 0x20000 && c <= 0x2FFFF) return true;
  return false;
}

bool is_word_cp(char32_t c) {
  if (c == '_') return true;
  if (c >= '0' && c <= '9') return true;
  if (c == 0xB2 || c == 0xB3 || c == 0xB9) return true;
  if (c >= 0x660 && c <= 0x669) return true;
  if (c >= 0xFF10 && c <= 0xFF19) return true;
  return is_alpha_cp(c) || (c >= 0x300 && c <= 0x36F);
}

bool is_space_cp(char32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
         c == 0x85 || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x1F;
}

bool is_digit_str(std::string_view s) {
  if (s.empty()) return false;
  for (char32_t c : u32(s))
    if (!((c >= '0' && c <= '9') || c == 0xB2 || c == 0xB3 || c == 0xB9)) return false;
  return true;
}

std::vector<std::string> tokens(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (char32_t c : u32(normalize(s))) {
    if (is_word_cp(c)) cur += u8(c);
    else if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// ---------------------------------------------------------------- recorte, division, union
std::string trim(std::string_view s, std::string_view chars) { return rtrim(ltrim(s, chars), chars); }

std::string ltrim(std::string_view s, std::string_view chars) {
  size_t i = s.find_first_not_of(chars);
  return i == std::string_view::npos ? "" : std::string(s.substr(i));
}

std::string rtrim(std::string_view s, std::string_view chars) {
  size_t i = s.find_last_not_of(chars);
  return i == std::string_view::npos ? "" : std::string(s.substr(0, i + 1));
}

std::string strip_u(std::string_view s, std::u32string_view chars) {
  auto u = u32(s);
  size_t a = 0, b = u.size();
  while (a < b && chars.find(u[a]) != std::u32string_view::npos) ++a;
  while (b > a && chars.find(u[b - 1]) != std::u32string_view::npos) --b;
  return u8(std::u32string_view(u).substr(a, b - a));
}

std::vector<std::string> split(std::string_view s, std::string_view sep, int maxsplit) {
  std::vector<std::string> out;
  if (sep.empty()) return split_ws(s);
  size_t pos = 0;
  int n = 0;
  while (true) {
    if (maxsplit >= 0 && n >= maxsplit) break;
    size_t i = s.find(sep, pos);
    if (i == std::string_view::npos) break;
    out.emplace_back(s.substr(pos, i - pos));
    pos = i + sep.size();
    ++n;
  }
  out.emplace_back(s.substr(pos));
  return out;
}

std::vector<std::string> split_ws(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  for (char32_t c : u32(s)) {
    if (is_space_cp(c)) {
      if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
    } else {
      cur += u8(c);
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

std::vector<std::string> splitlines(std::string_view s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n' || s[i] == '\r') {
      out.emplace_back(s.substr(start, i - start));
      if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
      start = i + 1;
    }
  }
  if (start < s.size()) out.emplace_back(s.substr(start));
  return out;
}

std::string join(const std::vector<std::string>& v, std::string_view sep) {
  std::string o;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) o += sep;
    o += v[i];
  }
  return o;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }
bool icontains(std::string_view s, std::string_view p) { return lower(s).find(lower(p)) != std::string::npos; }

std::string repeat(std::string_view s, int n) {
  std::string o;
  for (int i = 0; i < n; ++i) o += s;
  return o;
}

std::string rpad(std::string_view s, size_t width) {
  std::string o(s);
  size_t l = ulen(s);
  if (l < width) o.append(width - l, ' ');
  return o;
}

// ---------------------------------------------------------------- HTML y nombres
static const std::unordered_map<std::string, char32_t>& entities() {
  static const std::unordered_map<std::string, char32_t> m = {
      {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"iexcl", 0xA1},
      {"cent", 0xA2}, {"pound", 0xA3}, {"curren", 0xA4}, {"yen", 0xA5}, {"brvbar", 0xA6}, {"sect", 0xA7},
      {"uml", 0xA8}, {"copy", 0xA9}, {"ordf", 0xAA}, {"laquo", 0xAB}, {"not", 0xAC}, {"shy", 0xAD}, {"reg", 0xAE},
      {"macr", 0xAF}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"acute", 0xB4},
      {"micro", 0xB5}, {"para", 0xB6}, {"middot", 0xB7}, {"cedil", 0xB8}, {"sup1", 0xB9}, {"ordm", 0xBA},
      {"raquo", 0xBB}, {"frac14", 0xBC}, {"frac12", 0xBD}, {"frac34", 0xBE}, {"iquest", 0xBF}, {"Agrave", 0xC0},
      {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4}, {"Aring", 0xC5}, {"AElig", 0xC6},
      {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9}, {"Ecirc", 0xCA}, {"Euml", 0xCB}, {"Igrave", 0xCC},
      {"Iacute", 0xCD}, {"Icirc", 0xCE}, {"Iuml", 0xCF}, {"ETH", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2},
      {"Oacute", 0xD3}, {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6}, {"times", 0xD7}, {"Oslash", 0xD8},
      {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC}, {"Yacute", 0xDD}, {"THORN", 0xDE},
      {"szlig", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"auml", 0xE4},
      {"aring", 0xE5}, {"aelig", 0xE6}, {"ccedil", 0xE7}, {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA},
      {"euml", 0xEB}, {"igrave", 0xEC}, {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"eth", 0xF0},
      {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6},
      {"divide", 0xF7}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC},
      {"yacute", 0xFD}, {"thorn", 0xFE}, {"yuml", 0xFF}, {"euro", 0x20AC}, {"ndash", 0x2013}, {"mdash", 0x2014},
      {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
      {"bdquo", 0x201E}, {"hellip", 0x2026}, {"bull", 0x2022}, {"trade", 0x2122}, {"larr", 0x2190},
      {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194}, {"lsaquo", 0x2039},
      {"rsaquo", 0x203A}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"permil", 0x2030}, {"prime", 0x2032},
      {"OElig", 0x152}, {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178}, {"fnof", 0x192},
      {"circ", 0x2C6}, {"tilde", 0x2DC}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C},
      {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F}, {"minus", 0x2212}, {"le", 0x2264}, {"ge", 0x2265},
      {"ne", 0x2260}, {"infin", 0x221E}, {"check", 0x2713}, {"hearts", 0x2665}, {"star", 0x2606}};
  return m;
}

std::string html_unescape(std::string_view s) {
  if (s.find('&') == std::string_view::npos) return std::string(s);
  std::string o;
  o.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') { o += s[i++]; continue; }
    size_t j = i + 1;
    if (j < s.size() && s[j] == '#') {
      ++j;
      bool hex = j < s.size() && (s[j] == 'x' || s[j] == 'X');
      if (hex) ++j;
      size_t k = j;
      while (k < s.size() && (hex ? std::isxdigit((unsigned char)s[k]) : std::isdigit((unsigned char)s[k])) && k - j < 8) ++k;
      if (k > j) {
        unsigned long cp = std::stoul(std::string(s.substr(j, k - j)), nullptr, hex ? 16 : 10);
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp >= 0x80 && cp < 0xA0) {   // referencias numericas cp1252 (como html.unescape)
          std::string one(1, (char)cp);
          o += cp1252_to_utf8(one);
        } else {
          o += u8((char32_t)cp);
        }
        i = (k < s.size() && s[k] == ';') ? k + 1 : k;
        continue;
      }
      o += s[i++];
      continue;
    }
    size_t k = j;
    while (k < s.size() && std::isalnum((unsigned char)s[k]) && k - j < 10) ++k;
    if (k > j) {
      auto it = entities().find(std::string(s.substr(j, k - j)));
      if (it != entities().end()) {
        o += u8(it->second);
        i = (k < s.size() && s[k] == ';') ? k + 1 : k;
        continue;
      }
    }
    o += s[i++];
  }
  return o;
}

std::string html_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#x27;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string url_encode(std::string_view s, bool plus) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else if (c == ' ' && plus) o += '+';
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

std::string url_decode(std::string_view s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
      o += (char)std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16);
      i += 2;
    } else if (s[i] == '+') {
      o += ' ';
    } else {
      o += s[i];
    }
  }
  return o;
}

std::string safe_name(std::string_view s, size_t keep_last) {
  std::string o;
  bool last_sub = false;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '.' || c == '_' || c == '-') { o += (char)c; last_sub = false; }
    else if (!last_sub) { o += '_'; last_sub = true; }
  }
  if (o.size() > keep_last) o = o.substr(o.size() - keep_last);
  return o;
}

std::string slug(std::string_view s, size_t max) {
  std::string low = lower(s), o;
  bool dash = false;
  for (unsigned char c : low) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { o += (char)c; dash = false; }
    else if (!dash) { o += '-'; dash = true; }
  }
  o = trim(o, "-");
  if (o.size() > max) o = o.substr(0, max);
  return o;
}

}  // namespace nx
