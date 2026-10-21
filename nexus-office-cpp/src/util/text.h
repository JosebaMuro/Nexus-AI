// Utilidades de texto UTF-8: lo que en Python dan str, unicodedata y los f-strings.
#pragma once
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nx {

// ---------------------------------------------------------------- concatenacion y numeros
template <class... A>
std::string cat(A&&... a) {
  std::ostringstream o;
  (o << ... << a);
  return o.str();
}
std::string fnum(double v, int decimals);        // round(v, n) como texto ("3.5", "12")
std::string pyfloat(double v);                    // repr de un float de Python ("0.7", "3.0")
double round_to(double v, int decimals);          // round(v, n)

// ---------------------------------------------------------------- UTF-8
std::u32string u32(std::string_view s);          // decodifica (los bytes invalidos pasan a U+FFFD)
std::string u8(std::u32string_view s);
std::string u8(char32_t c);
size_t ulen(std::string_view s);                  // len() en caracteres
std::string usub(std::string_view s, long start, long stop = (1L << 60));  // s[start:stop] en caracteres
std::string uleft(std::string_view s, size_t n);  // s[:n]
std::string uright(std::string_view s, size_t n); // s[-n:]
std::string valid_utf8(std::string_view s);       // decode(errors="replace")
std::string latin1_to_utf8(std::string_view s);
std::string cp1252_to_utf8(std::string_view s);
std::string utf16_to_utf8(std::string_view bytes, bool little_endian);

// ---------------------------------------------------------------- mayusculas, acentos
char32_t lower_cp(char32_t c);
char32_t upper_cp(char32_t c);
std::string lower(std::string_view s);
std::string upper(std::string_view s);
std::string strip_accents(std::string_view s);    // NFKD sin marcas combinantes (letras latinas)
std::string normalize(std::string_view s);        // nexus.memory.embeddings.normalize: minusculas y sin acentos
std::vector<std::string> tokens(std::string_view s);  // \w+ sobre normalize(s)
bool is_word_cp(char32_t c);                      // \w de Python (letras, digitos y _)
bool is_alpha_cp(char32_t c);
bool is_digit_str(std::string_view s);            // str.isdigit()
bool is_space_cp(char32_t c);

// ---------------------------------------------------------------- recorte, division, union
std::string trim(std::string_view s, std::string_view chars = " \t\r\n\v\f");
std::string ltrim(std::string_view s, std::string_view chars = " \t\r\n\v\f");
std::string rtrim(std::string_view s, std::string_view chars = " \t\r\n\v\f");
std::string strip_u(std::string_view s, std::u32string_view chars);   // strip() con caracteres no ASCII
std::vector<std::string> split(std::string_view s, std::string_view sep, int maxsplit = -1);
std::vector<std::string> split_ws(std::string_view s);   // str.split() sin argumentos
std::vector<std::string> splitlines(std::string_view s);
std::string join(const std::vector<std::string>& v, std::string_view sep);
std::string replace_all(std::string s, std::string_view from, std::string_view to);
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);
bool contains(std::string_view s, std::string_view p);
bool icontains(std::string_view s, std::string_view p);   // sin distinguir mayusculas (ASCII + latinas)
std::string repeat(std::string_view s, int n);
std::string rpad(std::string_view s, size_t width);

// ---------------------------------------------------------------- HTML y nombres de fichero
std::string html_unescape(std::string_view s);
std::string html_escape(std::string_view s);
std::string url_encode(std::string_view s, bool plus_for_space = false);
std::string url_decode(std::string_view s);
std::string safe_name(std::string_view s, size_t keep_last = 80);   // re.sub(r"[^A-Za-z0-9._-]+", "_", s)[-n:]
std::string slug(std::string_view s, size_t max = 60);             // a-z0-9 y guiones

}  // namespace nx
