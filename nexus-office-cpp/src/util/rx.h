// Expresiones regulares con la semantica del modulo re de Python (PCRE2 con UTF y propiedades Unicode):
// \w, \b y la insensibilidad a mayusculas funcionan con acentos, y hay lookbehind, grupos con nombre, etc.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <string_view>
#include <vector>

namespace nx {

class Rx {
 public:
  enum Flags : unsigned { NONE = 0, I = 1, S = 2, M = 4, X = 8 };

  struct Match {
    std::string_view subject;
    std::vector<long> ov;   // pares (inicio, fin) por grupo; -1 si el grupo no participa
    size_t groups() const { return ov.size() / 2; }
    bool has(size_t g) const { return g < groups() && ov[2 * g] >= 0; }
    std::string str(size_t g = 0) const {
      return has(g) ? std::string(subject.substr(ov[2 * g], ov[2 * g + 1] - ov[2 * g])) : std::string();
    }
    std::string operator[](size_t g) const { return str(g); }
    size_t start(size_t g = 0) const { return has(g) ? (size_t)ov[2 * g] : std::string::npos; }
    size_t end(size_t g = 0) const { return has(g) ? (size_t)ov[2 * g + 1] : std::string::npos; }
  };

  Rx() = default;
  explicit Rx(std::string_view pattern, unsigned flags = NONE);
  Rx(const Rx&) = delete;
  Rx& operator=(const Rx&) = delete;
  Rx(Rx&& o) noexcept;
  Rx& operator=(Rx&& o) noexcept;
  ~Rx();

  bool ok() const { return code_ != nullptr; }
  const std::string& error() const { return err_; }

  std::optional<Match> search(std::string_view s, size_t pos = 0) const;
  std::optional<Match> match(std::string_view s, size_t pos = 0) const;   // anclada al inicio (re.match)
  std::optional<Match> fullmatch(std::string_view s) const;
  bool test(std::string_view s) const { return exec(s, 0, 0).has_value(); }
  std::vector<Match> finditer(std::string_view s) const;
  // Match guarda una vista del texto: con un temporario quedaria colgando (usar test() o guardar el texto antes)
  template <class T, class = std::enable_if_t<std::is_same_v<T, std::string>>>
  std::optional<Match> search(T&& s, size_t pos = 0) const = delete;
  template <class T, class = std::enable_if_t<std::is_same_v<T, std::string>>>
  std::optional<Match> match(T&& s, size_t pos = 0) const = delete;
  template <class T, class = std::enable_if_t<std::is_same_v<T, std::string>>>
  std::optional<Match> fullmatch(T&& s) const = delete;
  template <class T, class = std::enable_if_t<std::is_same_v<T, std::string>>>
  std::vector<Match> finditer(T&& s) const = delete;
  // findall: el grupo 1 si existe, si no la coincidencia entera
  std::vector<std::string> findall(std::string_view s) const;
  // sub: \1, \g<1> y \g<nombre> en el reemplazo (como re.sub)
  std::string sub(std::string_view s, std::string_view repl, int count = 0) const;
  std::string sub(std::string_view s, const std::function<std::string(const Match&)>& fn, int count = 0) const;
  std::pair<std::string, int> subn(std::string_view s, std::string_view repl) const;
  std::vector<std::string> split(std::string_view s, int maxsplit = 0) const;

 private:
  void* code_ = nullptr;
  std::string err_;
  std::optional<Match> exec(std::string_view s, size_t pos, unsigned opts) const;
};

// Atajos para patrones de un solo uso (se compilan cada vez: para bucles, usar un Rx estatico).
inline bool re_search(std::string_view pat, std::string_view s, unsigned flags = Rx::NONE) {
  return Rx(pat, flags).test(s);
}
std::string re_sub(std::string_view pat, std::string_view repl, std::string_view s, unsigned flags = Rx::NONE);
std::string re_escape(std::string_view s);

}  // namespace nx
