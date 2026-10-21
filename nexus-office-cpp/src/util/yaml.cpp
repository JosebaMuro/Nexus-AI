#define RYML_SINGLE_HDR_DEFINE_NOW
#include <ryml.hpp>

#include "util/yaml.h"

#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdexcept>

#include "util/rx.h"
#include "util/text.h"

namespace nx::yaml {

namespace {

[[noreturn]] void on_error(const char* msg, size_t len, ryml::Location loc, void*) {
  throw std::runtime_error(cat("YAML invalido (linea ", loc.line + 1, "): ", std::string(msg, len)));
}

void install_callbacks() {
  static std::once_flag once;
  std::call_once(once, [] {
    ryml::Callbacks cb = ryml::get_callbacks();
    cb.m_error = on_error;
    ryml::set_callbacks(cb);
  });
}

// Resolucion de escalares sin comillas (YAML 1.1 de PyYAML, lo que importa en la practica)
json scalar(std::string_view v, bool quoted) {
  std::string s(v);
  if (quoted) return s;
  if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") return nullptr;
  static const char* T[] = {"true", "True", "TRUE", "yes", "Yes", "YES", "on", "On", "ON"};
  static const char* F[] = {"false", "False", "FALSE", "no", "No", "NO", "off", "Off", "OFF"};
  for (auto t : T) if (s == t) return true;
  for (auto f : F) if (s == f) return false;
  static const Rx rint(R"(^[-+]?(0|[1-9][0-9_]*)$)");
  static const Rx rhex(R"(^0x[0-9a-fA-F_]+$)");
  static const Rx rfloat(R"(^[-+]?([0-9][0-9_]*)?\.[0-9_]*([eE][-+]?[0-9]+)?$|^[-+]?[0-9][0-9_]*[eE][-+]?[0-9]+$)");
  if (rint.fullmatch(s)) {
    std::string t = replace_all(s, "_", "");
    try {
      return std::stoll(t);
    } catch (...) {
      return std::stod(t);
    }
  }
  if (rhex.fullmatch(s)) return (long long)std::strtoll(replace_all(s, "_", "").c_str() + 2, nullptr, 16);
  if (s != "." && rfloat.fullmatch(s)) return std::stod(replace_all(s, "_", ""));
  if (s == ".inf" || s == ".Inf" || s == "+.inf") return INFINITY;
  if (s == "-.inf" || s == "-.Inf") return -INFINITY;
  if (s == ".nan" || s == ".NaN") return NAN;
  return s;
}

json convert(ryml::ConstNodeRef n) {
  if (n.is_map()) {
    json o = json::object();
    for (auto ch : n.children()) {
      std::string k(ch.key().str, ch.key().len);
      o[k] = convert(ch);
    }
    return o;
  }
  if (n.is_seq()) {
    json a = json::array();
    for (auto ch : n.children()) a.push_back(convert(ch));
    return a;
  }
  if (n.has_val()) {
    auto v = n.val();
    bool quoted = n.is_val_quoted() || n.type().has_any(ryml::VAL_LITERAL | ryml::VAL_FOLDED);
    if (v.str == nullptr) return quoted ? json("") : json(nullptr);
    return scalar(std::string_view(v.str, v.len), quoted);
  }
  return nullptr;
}

// ---------------------------------------------------------------- emisor (estilo de bloque de PyYAML)
bool needs_quotes(const std::string& s) {
  if (s.empty()) return true;
  json r = scalar(s, false);
  if (!r.is_string()) return true;   // parece numero, booleano o null
  static const std::string bad_start = "-?:,[]{}#&*!|>'\"%@`";
  if (bad_start.find(s[0]) != std::string::npos && !(s.size() > 1 && (s[0] == '-' || s[0] == '?' || s[0] == ':') && s[1] != ' '))
    return true;
  if (s.front() == ' ' || s.back() == ' ' || s.front() == '\t' || s.back() == '\t') return true;
  if (contains(s, ": ") || contains(s, " #") || ends_with(s, ":")) return true;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return true;
  return false;
}

std::string quote(const std::string& s) {
  // comillas simples si no hay caracteres de control; dobles con escapes si los hay
  bool ctrl = false;
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) ctrl = true;
  if (!ctrl) return "'" + replace_all(s, "'", "''") + "'";
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char b[8];
          std::snprintf(b, sizeof b, "\\x%02X", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  return o + "\"";
}

std::string key_str(const std::string& k) { return needs_quotes(k) || contains(k, "\n") ? quote(k) : k; }

std::string float_str(double d) {
  if (std::isnan(d)) return ".nan";
  if (std::isinf(d)) return d > 0 ? ".inf" : "-.inf";
  return pyfloat(d);
}

bool block_literal_ok(const std::string& s) {
  if (!contains(s, "\n")) return false;
  for (unsigned char c : s)
    if ((c < 0x20 && c != '\n') || c == 0x7f) return false;
  // las lineas con espacios al final no se conservan bien en literal: PyYAML usa comillas
  for (auto& line : split(s, "\n"))
    if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) return false;
  return s.front() != ' ' && s.front() != '\n';
}

void emit_scalar(std::string& out, const json& v, int indent) {
  if (v.is_null()) out += "null";
  else if (v.is_boolean()) out += v.get<bool>() ? "true" : "false";
  else if (v.is_number_integer() || v.is_number_unsigned()) out += v.dump();
  else if (v.is_number_float()) out += float_str(v.get<double>());
  else {
    const std::string& s = v.get_ref<const std::string&>();
    if (block_literal_ok(s)) {
      std::string body = s;
      std::string chomp;
      if (!ends_with(body, "\n")) chomp = "-";
      else if (ends_with(body, "\n\n")) chomp = "+";
      out += "|" + chomp + "\n";
      std::string pad(indent + 2, ' ');
      std::string b = chomp == "-" ? body : body.substr(0, body.size() - 1);
      if (chomp == "+") b = body.substr(0, body.size() - 1);
      auto lines = split(b, "\n");
      for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty()) out += pad + lines[i];
        if (i + 1 < lines.size()) out += "\n";
      }
      return;
    }
    out += needs_quotes(s) ? quote(s) : s;
  }
}

void emit(std::string& out, const json& v, int indent, bool in_seq);

void emit_map(std::string& out, const json& m, int indent, bool first_inline) {
  std::string pad(indent, ' ');
  bool first = true;
  for (auto it = m.begin(); it != m.end(); ++it) {
    if (!(first && first_inline)) out += pad;
    first = false;
    out += key_str(it.key()) + ":";
    const json& v = it.value();
    if (v.is_object() && !v.empty()) {
      out += "\n";
      emit_map(out, v, indent + 2, false);
    } else if (v.is_array() && !v.empty()) {
      out += "\n";
      emit(out, v, indent, false);   // PyYAML: las listas bajo una clave van sin sangria extra
    } else if (v.is_object()) {
      out += " {}\n";
    } else if (v.is_array()) {
      out += " []\n";
    } else {
      out += " ";
      emit_scalar(out, v, indent);
      out += "\n";
    }
  }
}

void emit(std::string& out, const json& v, int indent, bool) {
  std::string pad(indent, ' ');
  if (v.is_object()) {
    if (v.empty()) { out += pad + "{}\n"; return; }
    emit_map(out, v, indent, false);
  } else if (v.is_array()) {
    if (v.empty()) { out += pad + "[]\n"; return; }
    for (auto& e : v) {
      out += pad + "- ";
      if (e.is_object() && !e.empty()) {
        emit_map(out, e, indent + 2, true);
      } else if (e.is_array() && !e.empty()) {
        out += "\n";
        emit(out, e, indent + 2, true);
      } else if (e.is_object()) {
        out += "{}\n";
      } else if (e.is_array()) {
        out += "[]\n";
      } else {
        emit_scalar(out, e, indent);
        out += "\n";
      }
    }
  } else {
    emit_scalar(out, v, indent);
    out += "\n";
  }
}

}  // namespace

json load(const std::string& text) {
  install_callbacks();
  std::string t = text;
  if (starts_with(t, "\xEF\xBB\xBF")) t.erase(0, 3);
  if (trim(t).empty()) return nullptr;
  ryml::Tree tree = ryml::parse_in_arena(ryml::csubstr(t.data(), t.size()));
  ryml::ConstNodeRef root = tree.crootref();
  if (root.is_stream()) {
    if (!root.has_children()) return nullptr;
    root = root.first_child();
  }
  return convert(root);
}

json load_file(const fs::path& p) {
  auto txt = read_file(p);
  if (!txt) return nullptr;
  try {
    return load(*txt);
  } catch (...) {
    return nullptr;
  }
}

std::string dump(const json& j) {
  std::string out;
  emit(out, j, 0, false);
  return out;
}

}  // namespace nx::yaml
