// NexusLED - JSON minimo (lectura y escritura) sin librerias externas.
// Suficiente para la API: objetos, listas, numeros, cadenas, booleanos y null.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

namespace nl {

struct JVal {
  enum Type : uint8_t { Null, Bool, Num, Str, Arr, Obj };
  Type t = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JVal> a;
  std::vector<std::pair<std::string, JVal>> o;

  bool isNull() const { return t == Null; }
  bool isNum() const { return t == Num; }
  bool isStr() const { return t == Str; }
  bool isArr() const { return t == Arr; }
  bool isObj() const { return t == Obj; }
  bool isBool() const { return t == Bool; }
  const JVal* get(const char* key) const {
    if (t != Obj) return nullptr;
    for (auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  bool has(const char* key) const { return get(key) != nullptr; }
  double num(double def = 0) const {
    if (t == Num) return n;
    if (t == Bool) return b ? 1 : 0;
    if (t == Str && !s.empty()) {
      char* end = nullptr;
      double v = strtod(s.c_str(), &end);
      if (end && *end == 0) return v;
    }
    return def;
  }
  bool truthy(bool def = false) const {
    if (t == Bool) return b;
    if (t == Num) return n != 0;
    if (t == Str) return s == "true" || s == "1" || s == "on" || s == "si";
    return def;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& src) : p_(src.c_str()), end_(src.c_str() + src.size()) {}
  bool parse(JVal& out) {
    depth_ = 0;
    ws();
    if (!value(out)) return false;
    ws();
    return p_ == end_;
  }

 private:
  const char* p_;
  const char* end_;
  int depth_ = 0;

  void ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) p_++;
  }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if ((size_t)(end_ - p_) < n || strncmp(p_, w, n) != 0) return false;
    p_ += n;
    return true;
  }
  static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static void utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
      s += (char)cp;
    } else if (cp < 0x800) {
      s += (char)(0xC0 | (cp >> 6));
      s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      s += (char)(0xE0 | (cp >> 12));
      s += (char)(0x80 | ((cp >> 6) & 0x3F));
      s += (char)(0x80 | (cp & 0x3F));
    } else {
      s += (char)(0xF0 | (cp >> 18));
      s += (char)(0x80 | ((cp >> 12) & 0x3F));
      s += (char)(0x80 | ((cp >> 6) & 0x3F));
      s += (char)(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(uint32_t& cp) {
    if (end_ - p_ < 4) return false;
    cp = 0;
    for (int i = 0; i < 4; i++) {
      int h = hexv(p_[i]);
      if (h < 0) return false;
      cp = cp * 16 + h;
    }
    p_ += 4;
    return true;
  }
  bool str(std::string& s) {
    if (p_ >= end_ || *p_ != '"') return false;
    p_++;
    while (p_ < end_ && *p_ != '"') {
      char c = *p_++;
      if ((unsigned char)c < 0x20) return false;
      if (c != '\\') {
        s += c;
        continue;
      }
      if (p_ >= end_) return false;
      char e = *p_++;
      switch (e) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            uint32_t lo;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(s, cp);
          break;
        }
        default: return false;
      }
    }
    if (p_ >= end_) return false;
    p_++;
    return true;
  }
  bool value(JVal& v) {
    if (p_ >= end_ || ++depth_ > 16) return false;
    bool ok = false;
    char c = *p_;
    if (c == '{') {
      v.t = JVal::Obj;
      p_++;
      ws();
      if (p_ < end_ && *p_ == '}') {
        p_++;
        ok = true;
      } else {
        while (true) {
          ws();
          std::string k;
          if (!str(k)) break;
          ws();
          if (p_ >= end_ || *p_ != ':') break;
          p_++;
          ws();
          v.o.emplace_back(std::move(k), JVal());
          if (!value(v.o.back().second)) break;
          ws();
          if (p_ < end_ && *p_ == ',') { p_++; continue; }
          if (p_ < end_ && *p_ == '}') { p_++; ok = true; }
          break;
        }
      }
    } else if (c == '[') {
      v.t = JVal::Arr;
      p_++;
      ws();
      if (p_ < end_ && *p_ == ']') {
        p_++;
        ok = true;
      } else {
        while (true) {
          ws();
          v.a.emplace_back();
          if (!value(v.a.back())) break;
          ws();
          if (p_ < end_ && *p_ == ',') { p_++; continue; }
          if (p_ < end_ && *p_ == ']') { p_++; ok = true; }
          break;
        }
      }
    } else if (c == '"') {
      v.t = JVal::Str;
      ok = str(v.s);
    } else if (c == 't') {
      v.t = JVal::Bool; v.b = true; ok = lit("true");
    } else if (c == 'f') {
      v.t = JVal::Bool; v.b = false; ok = lit("false");
    } else if (c == 'n') {
      v.t = JVal::Null; ok = lit("null");
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      char buf[40];
      size_t i = 0;
      while (p_ < end_ && i < sizeof(buf) - 1 &&
             ((*p_ >= '0' && *p_ <= '9') || *p_ == '-' || *p_ == '+' || *p_ == '.' || *p_ == 'e' || *p_ == 'E'))
        buf[i++] = *p_++;
      buf[i] = 0;
      char* e = nullptr;
      v.t = JVal::Num;
      v.n = strtod(buf, &e);
      ok = e && *e == 0 && i > 0;
    }
    depth_--;
    return ok;
  }
};

inline bool parseJson(const std::string& src, JVal& out) {
  out = JVal();
  JParser p(src);
  return p.parse(out);
}

// Escritor: se encarga de las comas y del escape de cadenas.
class JWriter {
 public:
  std::string out;
  JWriter& obj() { pre(); out += '{'; first_.push_back(true); open_.push_back('{'); return *this; }
  JWriter& end() {
    char c = open_.back();
    open_.pop_back();
    first_.pop_back();
    out += (c == '{') ? '}' : ']';
    return *this;
  }
  JWriter& arr() { pre(); out += '['; first_.push_back(true); open_.push_back('['); return *this; }
  JWriter& key(const char* k) {
    comma();
    quote(k);
    out += ':';
    afterKey_ = true;
    return *this;
  }
  JWriter& str(const std::string& s) { pre(); quote(s.c_str(), s.size()); return *this; }
  JWriter& str(const char* s) { pre(); quote(s); return *this; }
  JWriter& num(double v) {
    pre();
    char b[32];
    if (v == (double)(long long)v && v > -1e15 && v < 1e15)
      snprintf(b, sizeof b, "%lld", (long long)v);
    else
      snprintf(b, sizeof b, "%.4g", v);
    out += b;
    return *this;
  }
  JWriter& boolean(bool v) { pre(); out += v ? "true" : "false"; return *this; }
  JWriter& null() { pre(); out += "null"; return *this; }
  JWriter& raw(const std::string& r) { pre(); out += r.empty() ? "null" : r; return *this; }
  // atajos clave-valor
  JWriter& kv(const char* k, const std::string& v) { return key(k).str(v); }
  JWriter& kv(const char* k, const char* v) { return key(k).str(v); }
  JWriter& kv(const char* k, double v) { return key(k).num(v); }
  JWriter& kv(const char* k, int v) { return key(k).num(v); }
  JWriter& kv(const char* k, unsigned v) { return key(k).num(v); }
  JWriter& kv(const char* k, long v) { return key(k).num((double)v); }
  JWriter& kv(const char* k, unsigned long v) { return key(k).num((double)v); }
  JWriter& kvb(const char* k, bool v) { return key(k).boolean(v); }

  JWriter() { open_.reserve(16); first_.reserve(16); }

 private:
  std::vector<char> open_;
  std::vector<bool> first_;
  bool afterKey_ = false;

  void comma() {
    if (!first_.empty()) {
      if (!first_.back()) out += ',';
      first_.back() = false;
    }
  }
  void pre() {
    if (afterKey_) {
      afterKey_ = false;
      return;
    }
    comma();
  }
  void quote(const char* s, size_t n = (size_t)-1) {
    if (n == (size_t)-1) n = strlen(s);
    out += '"';
    for (size_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)s[i];
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
          } else {
            out += (char)c;
          }
      }
    }
    out += '"';
  }
};

}  // namespace nl
