// JSON con el orden de claves conservado (como los dict de Python): la API devuelve lo mismo que la version Python.
#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace nx {

using json = nlohmann::ordered_json;

// Volcado tolerante: el texto invalido en UTF-8 se sustituye en vez de lanzar (ensure_ascii=False de Python).
inline std::string jdump(const json& j, int indent = -1) {
  return j.dump(indent, ' ', false, json::error_handler_t::replace);
}

// Lectura tolerante: devuelve null (discarded) si no es JSON valido.
inline json jparse(const std::string& s) {
  return json::parse(s, nullptr, false);
}

// Acceso seguro con valor por defecto ("d.get(k, default)").
template <class T>
inline T jget(const json& j, const std::string& key, T def) {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return def;
  try {
    return it->template get<T>();
  } catch (...) {
    return def;
  }
}

inline std::string jstr(const json& j, const std::string& key, const std::string& def = "") {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return def;
  if (it->is_string()) return it->get<std::string>();
  if (it->is_boolean()) return it->get<bool>() ? "True" : "False";
  if (it->is_number_integer()) return std::to_string(it->get<long long>());
  return jdump(*it);
}

// Valor "verdadero" al estilo Python (bool(x)).
inline bool truthy(const json& j) {
  if (j.is_null()) return false;
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number_integer()) return j.get<long long>() != 0;
  if (j.is_number_float()) return j.get<double>() != 0.0;
  if (j.is_string()) return !j.get_ref<const std::string&>().empty();
  if (j.is_array() || j.is_object()) return !j.empty();
  return true;
}

// json.dumps(x, ensure_ascii=False) de Python: separadores ", " y ": " (lo que ve el modelo y el usuario)
inline std::string pydump(const json& j) {
  if (j.is_object()) {
    std::string o = "{";
    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (!first) o += ", ";
      first = false;
      o += json(it.key()).dump(-1, ' ', false, json::error_handler_t::replace) + ": " + pydump(it.value());
    }
    return o + "}";
  }
  if (j.is_array()) {
    std::string o = "[";
    for (size_t i = 0; i < j.size(); ++i) {
      if (i) o += ", ";
      o += pydump(j[i]);
    }
    return o + "]";
  }
  if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
  if (j.is_binary()) return "\"<bytes>\"";
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

inline double jnum(const json& j, double def = 0.0) {
  if (j.is_number()) return j.get<double>();
  if (j.is_boolean()) return j.get<bool>() ? 1.0 : 0.0;
  if (j.is_string()) {
    try {
      return std::stod(j.get<std::string>());
    } catch (...) {
    }
  }
  return def;
}

}  // namespace nx
