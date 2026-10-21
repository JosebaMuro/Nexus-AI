// Configuracion en dos niveles (nexus/config.py): usuario (USER_KEYS) y administrador con PIN.
// Se guarda en data/config.yaml; los valores ausentes se completan con DEFAULTS.
#pragma once
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include "util/json.h"

namespace nx {

class PermissionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

const json& config_defaults();
const std::set<std::string>& user_keys();
const std::set<std::string>& secret_keys();
json deep_merge(const json& base, const json& extra);

class Config {
 public:
  Config();
  void load();
  void save();
  json get(const std::string& dotted, const json& def = nullptr) const;
  std::string str(const std::string& dotted, const std::string& def = "") const;
  double num(const std::string& dotted, double def = 0) const;
  bool flag(const std::string& dotted, bool def = false) const;
  void set(const std::string& dotted, const json& value, bool save = true);
  json update(const json& changes, bool admin);   // {"seccion.clave": valor}
  json view(bool admin) const;
  json data() const;

  bool has_pin() const;
  void set_pin(const std::string& pin);
  bool check_pin(const std::string& pin) const;

 private:
  mutable std::recursive_mutex mu_;
  json data_;
};

Config& get_config();
void reset_config();
std::string hash_pin(const std::string& pin, const std::string& salt_hex);

}  // namespace nx
