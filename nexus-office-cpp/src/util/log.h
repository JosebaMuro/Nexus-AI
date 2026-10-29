// Registro a consola y a data/logs/nexus.log, con un buffer en memoria para el panel de administracion.
#pragma once
#include <string>
#include <vector>

#include "util/text.h"

namespace nx::log {

void setup();
void write(const char* level, const std::string& name, const std::string& msg);
std::vector<std::string> recent(size_t n = 200);
void set_quiet(bool quiet);   // sin consola (pruebas)

struct Logger {
  std::string name;
  template <class... A> void info(A&&... a) const { write("INFO", name, cat(std::forward<A>(a)...)); }
  template <class... A> void warning(A&&... a) const { write("WARNING", name, cat(std::forward<A>(a)...)); }
  template <class... A> void error(A&&... a) const { write("ERROR", name, cat(std::forward<A>(a)...)); }
  template <class... A> void debug(A&&... a) const { write("DEBUG", name, cat(std::forward<A>(a)...)); }
};

inline Logger get(const std::string& name) { return Logger{"nexus." + name}; }

}  // namespace nx::log
