#include "util/csv.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "util/text.h"

namespace nx::csv {

std::vector<std::vector<std::string>> read(const std::string& t, char d, char q, bool skip_sp) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool in_q = false, was_q = false, start = true, any = false;
  auto end_field = [&] {
    row.push_back(field);
    field.clear();
    was_q = false;
    start = true;
  };
  auto end_row = [&] {
    if (any || !row.empty()) {
      end_field();
      rows.push_back(row);
    }
    row.clear();
    field.clear();
    any = false;
    start = true;
  };
  for (size_t i = 0; i < t.size(); ++i) {
    char c = t[i];
    if (in_q) {
      if (c == q) {
        if (i + 1 < t.size() && t[i + 1] == q) {
          field += q;
          ++i;
        } else {
          in_q = false;
        }
      } else {
        field += c;
      }
      continue;
    }
    if (c == d) {
      end_field();
      any = true;
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (c == '\r' && i + 1 < t.size() && t[i + 1] == '\n') ++i;
      end_row();
      continue;
    }
    if (start && skip_sp && c == ' ') continue;
    if (start && c == q) {
      in_q = was_q = true;
      start = false;
      any = true;
      continue;
    }
    start = false;
    any = true;
    field += c;
  }
  if (any || !row.empty() || !field.empty()) end_row();
  return rows;
}

std::string row(const std::vector<std::string>& fields, char d) {
  std::string out;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) out += d;
    const std::string& f = fields[i];
    bool need = f.find(d) != std::string::npos || f.find('"') != std::string::npos ||
                f.find('\n') != std::string::npos || f.find('\r') != std::string::npos;
    out += need ? "\"" + replace_all(f, "\"", "\"\"") + "\"" : f;
  }
  return out + "\r\n";
}

}  // namespace nx::csv

namespace nx::toml {

namespace {

class P {
 public:
  explicit P(const std::string& s) : s_(s) {}

  json run() {
    json root = json::object();
    json* cur = &root;
    while (true) {
      ws_nl();
      if (i_ >= s_.size()) break;
      if (s_[i_] == '[') {
        bool arr = s_.compare(i_, 2, "[[") == 0;
        i_ += arr ? 2 : 1;
        auto keys = key_path();
        ws();
        expect(arr ? "]]" : "]");
        json* t = &root;
        for (size_t k = 0; k < keys.size(); ++k) {
          json& n = (*t)[keys[k]];
          if (n.is_null()) n = json::object();
          if (n.is_array()) {
            if (k + 1 == keys.size() && arr) {
              n.push_back(json::object());
              t = &n.back();
              break;
            }
            t = &n.back();
            continue;
          }
          if (k + 1 == keys.size() && arr) {
            if (!n.empty() && !n.is_array()) fail("tabla redefinida como lista");
            n = json::array({json::object()});
            t = &n.back();
            break;
          }
          t = &n;
        }
        cur = t;
        eol();
        continue;
      }
      auto keys = key_path();
      ws();
      expect("=");
      ws();
      json v = value();
      json* t = cur;
      for (size_t k = 0; k + 1 < keys.size(); ++k) {
        json& n = (*t)[keys[k]];
        if (n.is_null()) n = json::object();
        t = &n;
      }
      if (t->contains(keys.back())) fail("clave repetida: " + keys.back());
      (*t)[keys.back()] = v;
      eol();
    }
    return root;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  [[noreturn]] void fail(const std::string& m) { throw std::runtime_error("TOML no valido: " + m); }
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) ++i_;
  }
  void comment() {
    if (i_ < s_.size() && s_[i_] == '#')
      while (i_ < s_.size() && s_[i_] != '\n') ++i_;
  }
  void ws_nl() {
    for (;;) {
      ws();
      comment();
      if (i_ < s_.size() && (s_[i_] == '\n' || s_[i_] == '\r')) {
        ++i_;
        continue;
      }
      return;
    }
  }
  void eol() {
    ws();
    comment();
    if (i_ < s_.size() && s_[i_] == '\r') ++i_;
    if (i_ < s_.size() && s_[i_] != '\n') fail("se esperaba fin de linea");
  }
  void expect(const char* t) {
    size_t n = std::char_traits<char>::length(t);
    if (s_.compare(i_, n, t) != 0) fail(std::string("se esperaba ") + t);
    i_ += n;
  }
  std::vector<std::string> key_path() {
    std::vector<std::string> keys;
    for (;;) {
      ws();
      if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
        keys.push_back(string());
      } else {
        size_t st = i_;
        while (i_ < s_.size() && (std::isalnum((unsigned char)s_[i_]) || s_[i_] == '_' || s_[i_] == '-')) ++i_;
        if (st == i_) fail("clave vacia");
        keys.push_back(s_.substr(st, i_ - st));
      }
      ws();
      if (i_ < s_.size() && s_[i_] == '.') {
        ++i_;
        continue;
      }
      return keys;
    }
  }
  std::string string() {
    char q = s_[i_];
    bool multi = s_.compare(i_, 3, std::string(3, q)) == 0;
    i_ += multi ? 3 : 1;
    if (multi && i_ < s_.size() && s_[i_] == '\n') ++i_;
    std::string out;
    while (i_ < s_.size()) {
      if (multi ? s_.compare(i_, 3, std::string(3, q)) == 0 : s_[i_] == q) {
        i_ += multi ? 3 : 1;
        return out;
      }
      char c = s_[i_++];
      if (!multi && c == '\n') fail("cadena sin cerrar");
      if (c == '\\' && q == '"') {
        if (i_ >= s_.size()) break;
        char e = s_[i_++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case 'u':
          case 'U': {
            size_t n = e == 'u' ? 4 : 8;
            out += u8((char32_t)std::stoul(s_.substr(i_, n), nullptr, 16));
            i_ += n;
            break;
          }
          default:
            if (multi && (e == '\n' || e == ' ' || e == '\r')) {
              while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) ++i_;
            } else {
              fail("escape no valido");
            }
        }
        continue;
      }
      out += c;
    }
    fail("cadena sin cerrar");
  }
  json value() {
    if (i_ >= s_.size()) fail("falta el valor");
    char c = s_[i_];
    if (c == '"' || c == '\'') return string();
    if (c == '[') {
      ++i_;
      json a = json::array();
      for (;;) {
        ws_nl();
        if (i_ < s_.size() && s_[i_] == ']') {
          ++i_;
          return a;
        }
        a.push_back(value());
        ws_nl();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        ws_nl();
        expect("]");
        return a;
      }
    }
    if (c == '{') {
      ++i_;
      json o = json::object();
      ws();
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return o;
      }
      for (;;) {
        auto keys = key_path();
        ws();
        expect("=");
        ws();
        json v = value();
        json* t = &o;
        for (size_t k = 0; k + 1 < keys.size(); ++k) t = &(*t)[keys[k]];
        (*t)[keys.back()] = v;
        ws();
        if (i_ < s_.size() && s_[i_] == ',') {
          ++i_;
          continue;
        }
        expect("}");
        return o;
      }
    }
    if (s_.compare(i_, 4, "true") == 0) {
      i_ += 4;
      return true;
    }
    if (s_.compare(i_, 5, "false") == 0) {
      i_ += 5;
      return false;
    }
    size_t st = i_;
    while (i_ < s_.size() && !std::strchr(",]}\n\r#", s_[i_])) ++i_;
    std::string tok = trim(s_.substr(st, i_ - st));
    std::string n = replace_all(tok, "_", "");
    if (n == "inf" || n == "+inf") return INFINITY;
    if (n == "-inf") return -INFINITY;
    if (n == "nan" || n == "+nan" || n == "-nan") return NAN;
    try {
      size_t used = 0;
      if (starts_with(n, "0x")) return (long long)std::stoll(n.substr(2), &used, 16);
      if (starts_with(n, "0o")) return (long long)std::stoll(n.substr(2), &used, 8);
      if (starts_with(n, "0b")) return (long long)std::stoll(n.substr(2), &used, 2);
      if (n.find_first_of(".eE") == std::string::npos) {
        long long v = std::stoll(n, &used);
        if (used == n.size()) return v;
      } else {
        double v = std::stod(n, &used);
        if (used == n.size()) return v;
      }
    } catch (...) {
    }
    if (!tok.empty() && std::isdigit((unsigned char)tok[0])) return tok;   // fechas y horas: como texto
    fail("valor no valido: " + tok);
  }
};

}  // namespace

json load(const std::string& text) { return P(text).run(); }

}  // namespace nx::toml
