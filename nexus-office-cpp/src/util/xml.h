// XML a arbol de elementos con la semantica de xml.etree.ElementTree: etiquetas y atributos con espacio de nombres
// como "{uri}local", texto, cola (tail), iter() e itertext(). Sin red: ignora DOCTYPE y no resuelve entidades externas.
#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nx::xml {

struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Element {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attrib;   // en orden del documento
  std::string text, tail;
  std::vector<std::unique_ptr<Element>> children;

  const std::string* get(const std::string& k) const;
  std::string get(const std::string& k, const std::string& def) const;
  std::string itertext() const;                               // "".join(el.itertext())
  void iter(std::vector<const Element*>& out) const;           // preorden, incluido el propio
  std::vector<const Element*> iter() const;
  size_t size() const { return children.size(); }
};

// Lanza ParseError si el documento no esta bien formado.
std::unique_ptr<Element> parse(const std::string& text);

std::string strip_ns(const std::string& tag);   // "{uri}local" -> "local"

}  // namespace nx::xml
