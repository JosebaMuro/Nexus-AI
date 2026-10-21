#include "util/rx.h"

#include <pcre2.h>

#include <stdexcept>

namespace nx {

Rx::Rx(std::string_view pattern, unsigned flags) {
  uint32_t opts = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;
  if (flags & I) opts |= PCRE2_CASELESS;
  if (flags & S) opts |= PCRE2_DOTALL;
  if (flags & M) opts |= PCRE2_MULTILINE;
  if (flags & X) opts |= PCRE2_EXTENDED;
  int errcode = 0;
  PCRE2_SIZE erroff = 0;
  code_ = pcre2_compile((PCRE2_SPTR)pattern.data(), pattern.size(), opts, &errcode, &erroff, nullptr);
  if (!code_) {
    PCRE2_UCHAR buf[256];
    pcre2_get_error_message(errcode, buf, sizeof buf);
    err_ = std::string((const char*)buf) + " en " + std::to_string(erroff) + ": " + std::string(pattern);
    throw std::runtime_error("regex invalida: " + err_);
  }
}

Rx::Rx(Rx&& o) noexcept : code_(o.code_), err_(std::move(o.err_)) { o.code_ = nullptr; }

Rx& Rx::operator=(Rx&& o) noexcept {
  if (this != &o) {
    if (code_) pcre2_code_free((pcre2_code*)code_);
    code_ = o.code_;
    err_ = std::move(o.err_);
    o.code_ = nullptr;
  }
  return *this;
}

Rx::~Rx() {
  if (code_) pcre2_code_free((pcre2_code*)code_);
}

std::optional<Rx::Match> Rx::exec(std::string_view s, size_t pos, unsigned opts) const {
  if (!code_ || pos > s.size()) return std::nullopt;
  pcre2_match_data* md = pcre2_match_data_create_from_pattern((pcre2_code*)code_, nullptr);
  int rc = pcre2_match((pcre2_code*)code_, (PCRE2_SPTR)s.data(), s.size(), pos, opts, md, nullptr);
  if (rc < 0) {
    pcre2_match_data_free(md);
    return std::nullopt;
  }
  uint32_t n = pcre2_get_ovector_count(md);
  PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
  uint32_t cap = 0;
  pcre2_pattern_info((pcre2_code*)code_, PCRE2_INFO_CAPTURECOUNT, &cap);
  Match m;
  m.subject = s;
  m.ov.resize(2 * (cap + 1), -1);
  for (uint32_t i = 0; i < n && i <= cap; ++i) {
    if (ov[2 * i] == PCRE2_UNSET) continue;
    m.ov[2 * i] = (long)ov[2 * i];
    m.ov[2 * i + 1] = (long)ov[2 * i + 1];
  }
  pcre2_match_data_free(md);
  return m;
}

std::optional<Rx::Match> Rx::search(std::string_view s, size_t pos) const { return exec(s, pos, 0); }

std::optional<Rx::Match> Rx::match(std::string_view s, size_t pos) const { return exec(s, pos, PCRE2_ANCHORED); }

std::optional<Rx::Match> Rx::fullmatch(std::string_view s) const {
  return exec(s, 0, PCRE2_ANCHORED | PCRE2_ENDANCHORED);
}

std::vector<Rx::Match> Rx::finditer(std::string_view s) const {
  std::vector<Match> out;
  size_t pos = 0;
  while (pos <= s.size()) {
    auto m = exec(s, pos, 0);
    if (!m) break;
    size_t st = m->start(), en = m->end();
    out.push_back(*m);
    if (en == st) {
      // coincidencia vacia: avanzar un caracter UTF-8 entero
      if (en >= s.size()) break;
      size_t step = 1;
      while (en + step < s.size() && ((unsigned char)s[en + step] & 0xC0) == 0x80) ++step;
      pos = en + step;
    } else {
      pos = en;
    }
  }
  return out;
}

std::vector<std::string> Rx::findall(std::string_view s) const {
  std::vector<std::string> out;
  for (auto& m : finditer(s)) out.push_back(m.groups() > 1 ? m.str(1) : m.str(0));
  return out;
}

static std::string expand(const Rx::Match& m, std::string_view repl, void* code) {
  std::string out;
  for (size_t i = 0; i < repl.size(); ++i) {
    char c = repl[i];
    if (c != '\\' || i + 1 >= repl.size()) { out += c; continue; }
    char d = repl[i + 1];
    if (d >= '0' && d <= '9') {
      size_t j = i + 1, g = 0;
      while (j < repl.size() && j < i + 3 && repl[j] >= '0' && repl[j] <= '9') g = g * 10 + (repl[j++] - '0');
      out += m.str(g);
      i = j - 1;
    } else if (d == 'g' && i + 2 < repl.size() && repl[i + 2] == '<') {
      size_t close = repl.find('>', i + 3);
      std::string name(repl.substr(i + 3, close - i - 3));
      size_t g = 0;
      bool num = !name.empty();
      for (char ch : name) num = num && ch >= '0' && ch <= '9';
      if (num) g = std::stoul(name);
      else {
        int idx = pcre2_substring_number_from_name((pcre2_code*)code, (PCRE2_SPTR)name.c_str());
        g = idx > 0 ? (size_t)idx : 9999;
      }
      out += m.str(g);
      i = close;
    } else if (d == 'n') { out += '\n'; ++i; }
    else if (d == 't') { out += '\t'; ++i; }
    else if (d == '\\') { out += '\\'; ++i; }
    else { out += c; }
  }
  return out;
}

std::string Rx::sub(std::string_view s, std::string_view repl, int count) const {
  void* code = code_;
  return sub(s, [&](const Match& m) { return expand(m, repl, code); }, count);
}

std::string Rx::sub(std::string_view s, const std::function<std::string(const Match&)>& fn, int count) const {
  std::string out;
  size_t last = 0;
  int n = 0;
  for (auto& m : finditer(s)) {
    if (count > 0 && n >= count) break;
    out.append(s.substr(last, m.start() - last));
    out += fn(m);
    last = m.end();
    ++n;
  }
  out.append(s.substr(last));
  return out;
}

std::pair<std::string, int> Rx::subn(std::string_view s, std::string_view repl) const {
  int n = (int)finditer(s).size();
  return {sub(s, repl), n};
}

std::vector<std::string> Rx::split(std::string_view s, int maxsplit) const {
  std::vector<std::string> out;
  size_t last = 0;
  int n = 0;
  for (auto& m : finditer(s)) {
    if (maxsplit > 0 && n >= maxsplit) break;
    if (m.end() == m.start() && m.start() == 0) continue;
    out.emplace_back(s.substr(last, m.start() - last));
    for (size_t g = 1; g < m.groups(); ++g) out.push_back(m.str(g));   // grupos capturados, como re.split
    last = m.end();
    ++n;
  }
  out.emplace_back(s.substr(last));
  return out;
}

std::string re_sub(std::string_view pat, std::string_view repl, std::string_view s, unsigned flags) {
  return Rx(pat, flags).sub(s, repl);
}

std::string re_escape(std::string_view s) {
  std::string o;
  for (char c : s) {
    if (std::string_view("\\^$.|?*+()[]{}-#&~ \t\n").find(c) != std::string_view::npos) o += '\\';
    o += c;
  }
  return o;
}

}  // namespace nx
