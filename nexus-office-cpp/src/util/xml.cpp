#include "util/xml.h"

#include <map>

#include "util/text.h"

namespace nx::xml {

const std::string* Element::get(const std::string& k) const {
  for (auto& [a, v] : attrib)
    if (a == k) return &v;
  return nullptr;
}

std::string Element::get(const std::string& k, const std::string& def) const {
  auto* v = get(k);
  return v ? *v : def;
}

std::string Element::itertext() const {
  std::string out = text;
  for (auto& c : children) {
    out += c->itertext();
    out += c->tail;
  }
  return out;
}

void Element::iter(std::vector<const Element*>& out) const {
  out.push_back(this);
  for (auto& c : children) c->iter(out);
}

std::vector<const Element*> Element::iter() const {
  std::vector<const Element*> out;
  iter(out);
  return out;
}

std::string strip_ns(const std::string& tag) {
  auto p = tag.rfind('}');
  return p == std::string::npos ? tag : tag.substr(p + 1);
}

namespace {

const char* XML_NS = "http://www.w3.org/XML/1998/namespace";

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  std::unique_ptr<Element> run() {
    if (starts_with(s_, "\xEF\xBB\xBF")) i_ = 3;
    misc();
    if (i_ >= s_.size() || s_[i_] != '<') fail("no element found");
    auto root = element({{"xml", XML_NS}});
    misc();
    if (i_ < s_.size()) fail("junk after document element");
    return root;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;
  using NS = std::map<std::string, std::string>;

  [[noreturn]] void fail(const std::string& what) {
    size_t line = 1, col = 0;
    for (size_t k = 0; k < i_ && k < s_.size(); ++k) {
      if (s_[k] == '\n') {
        ++line;
        col = 0;
      } else {
        ++col;
      }
    }
    throw ParseError(what + ": line " + std::to_string(line) + ", column " + std::to_string(col));
  }

  bool at(const char* p) const { return s_.compare(i_, std::char_traits<char>::length(p), p) == 0; }

  void skip_ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) ++i_;
  }

  void skip_to(const char* end) {
    size_t p = s_.find(end, i_);
    if (p == std::string::npos) fail("unclosed token");
    i_ = p + std::char_traits<char>::length(end);
  }

  // prologo y epilogo: declaracion, comentarios, instrucciones, DOCTYPE y espacios
  void misc() {
    for (;;) {
      skip_ws();
      if (at("<?")) {
        skip_to("?>");
      } else if (at("<!--")) {
        skip_to("-->");
      } else if (at("<!DOCTYPE") || at("<!doctype")) {
        int depth = 0;
        for (; i_ < s_.size(); ++i_) {
          char c = s_[i_];
          if (c == '[') ++depth;
          else if (c == ']') --depth;
          else if (c == '>' && depth <= 0) {
            ++i_;
            break;
          }
        }
      } else {
        return;
      }
    }
  }

  std::string name() {
    size_t st = i_;
    while (i_ < s_.size()) {
      unsigned char c = (unsigned char)s_[i_];
      if (std::isalnum(c) || c == '_' || c == ':' || c == '-' || c == '.' || c >= 0x80) ++i_;
      else break;
    }
    if (st == i_) fail("not well-formed (invalid token)");
    return s_.substr(st, i_ - st);
  }

  std::string entities(const std::string& raw) {
    if (raw.find('&') == std::string::npos) return raw;
    std::string out;
    for (size_t k = 0; k < raw.size(); ++k) {
      if (raw[k] != '&') {
        out += raw[k];
        continue;
      }
      size_t e = raw.find(';', k);
      if (e == std::string::npos) fail("not well-formed (invalid token)");
      std::string ent = raw.substr(k + 1, e - k - 1);
      if (ent == "amp") out += '&';
      else if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else if (!ent.empty() && ent[0] == '#') {
        unsigned long cp = 0;
        try {
          cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::stoul(ent.substr(2), nullptr, 16)
                                                                  : std::stoul(ent.substr(1));
        } catch (...) {
          fail("not well-formed (invalid token)");
        }
        out += u8((char32_t)cp);
      } else {
        fail("undefined entity &" + ent + ";");
      }
      k = e;
    }
    return out;
  }

  std::string chars_until_tag() {
    std::string out;
    while (i_ < s_.size()) {
      if (s_[i_] == '<') {
        if (at("<![CDATA[")) {
          size_t e = s_.find("]]>", i_ + 9);
          if (e == std::string::npos) fail("unclosed CDATA section");
          out += s_.substr(i_ + 9, e - i_ - 9);
          i_ = e + 3;
          continue;
        }
        if (at("<!--")) {
          skip_to("-->");
          continue;
        }
        if (at("<?")) {
          skip_to("?>");
          continue;
        }
        break;
      }
      size_t e = s_.find('<', i_);
      if (e == std::string::npos) e = s_.size();
      out += entities(s_.substr(i_, e - i_));
      i_ = e;
    }
    return replace_all(out, "\r\n", "\n");
  }

  std::string qualify(const std::string& qname, const NS& ns, bool attr) {
    auto c = qname.find(':');
    if (c == std::string::npos) {
      if (attr) return qname;
      auto it = ns.find("");
      return it == ns.end() || it->second.empty() ? qname : "{" + it->second + "}" + qname;
    }
    std::string pre = qname.substr(0, c);
    auto it = ns.find(pre);
    if (it == ns.end()) fail("unbound prefix");
    return "{" + it->second + "}" + qname.substr(c + 1);
  }

  std::unique_ptr<Element> element(NS ns, int depth = 0) {
    if (depth > 4000) fail("demasiado anidado");
    ++i_;   // '<'
    std::string qname = name();
    std::vector<std::pair<std::string, std::string>> raw;
    bool empty = false;
    for (;;) {
      skip_ws();
      if (i_ >= s_.size()) fail("unclosed token");
      if (at("/>")) {
        i_ += 2;
        empty = true;
        break;
      }
      if (s_[i_] == '>') {
        ++i_;
        break;
      }
      std::string an = name();
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != '=') fail("not well-formed (invalid token)");
      ++i_;
      skip_ws();
      if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) fail("not well-formed (invalid token)");
      char q = s_[i_++];
      size_t e = s_.find(q, i_);
      if (e == std::string::npos) fail("unclosed token");
      std::string v = entities(s_.substr(i_, e - i_));
      for (auto& ch : v)
        if (ch == '\n' || ch == '\t' || ch == '\r') ch = ' ';
      i_ = e + 1;
      for (auto& [k, _] : raw)
        if (k == an) fail("duplicate attribute");
      raw.emplace_back(an, v);
    }
    for (auto& [k, v] : raw) {
      if (k == "xmlns") ns[""] = v;
      else if (starts_with(k, "xmlns:")) ns[k.substr(6)] = v;
    }
    auto el = std::make_unique<Element>();
    el->tag = qualify(qname, ns, false);
    for (auto& [k, v] : raw)
      if (k != "xmlns" && !starts_with(k, "xmlns:")) el->attrib.emplace_back(qualify(k, ns, true), v);
    if (empty) return el;
    el->text = chars_until_tag();
    for (;;) {
      if (i_ >= s_.size()) fail("no element found");
      if (at("</")) {
        i_ += 2;
        std::string end = name();
        if (end != qname) fail("mismatched tag");
        skip_ws();
        if (i_ >= s_.size() || s_[i_] != '>') fail("not well-formed (invalid token)");
        ++i_;
        return el;
      }
      auto child = element(ns, depth + 1);
      child->tail = chars_until_tag();
      el->children.push_back(std::move(child));
    }
  }
};

}  // namespace

std::unique_ptr<Element> parse(const std::string& text) { return Parser(text).run(); }

}  // namespace nx::xml
