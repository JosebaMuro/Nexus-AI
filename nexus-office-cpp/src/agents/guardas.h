// Guardas de seguridad de los agentes: contenido no fiable (lo que llega de la web, documentos, ficheros o la memoria),
// deteccion heuristica de inyeccion de instrucciones y redaccion de datos personales antes de enviar texto a la nube.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "util/json.h"

namespace nx::guard {

// herramientas cuyo resultado trae texto de terceros (web, documentos, ficheros, memoria que pudo envenenarse)
bool untrusted_tool(const std::string& name);
// herramientas que escriben, gastan o actuan fuera del turno: tras leer contenido no fiable piden un escalon mas
bool sensitive_tool(const std::string& name, const std::vector<std::string>& permissions);

// frases tipicas de inyeccion ("ignora las instrucciones anteriores", "you are now"...). Devuelve las encontradas.
std::vector<std::string> injection_signals(const std::string& text);

// resultado de herramienta para el modelo. Los no fiables van delimitados con un nonce y un aviso, y se neutralizan
// las etiquetas del protocolo que el texto pudiera traer para hacerse pasar por otra llamada o resultado.
std::string wrap_result(const std::string& name, const std::string& body, bool untrusted, const std::string& nonce,
                        const std::vector<std::string>& signals = {});
std::string neutralize_tags(const std::string& text);

// regla fija del prompt de sistema sobre el contenido no fiable
const std::string& untrusted_rule();

// ---------------------------------------------------------------- datos personales (PII)
// Sustituye DNI/NIE, CIF, IBAN, tarjetas, correos y telefonos por marcas [DNI_1], [IBAN_1]... reversibles.
struct Redaction {
  std::string text;
  std::map<std::string, std::string> map;   // marca -> valor original
  int count() const { return (int)map.size(); }
};
Redaction redact_pii(const std::string& text, std::map<std::string, std::string>* shared = nullptr);
json redact_messages(const json& messages, std::map<std::string, std::string>& map);
std::string restore_pii(const std::string& text, const std::map<std::string, std::string>& map);

// restaura las marcas en una salida por trozos (streaming): retiene un posible "[MARCA_" partido entre trozos
class PiiRestorer {
 public:
  explicit PiiRestorer(const std::map<std::string, std::string>& map) : map_(map) {}
  std::string feed(const std::string& piece);
  std::string flush();

 private:
  const std::map<std::string, std::string>& map_;
  std::string pending_;
};

}  // namespace nx::guard
