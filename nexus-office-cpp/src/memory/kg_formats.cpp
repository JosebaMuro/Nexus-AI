// Lectores de grafos de conocimiento en cualquier formato habitual (memory/kg_formats.py), sin red. El formato se
// detecta por el contenido (la extension solo orienta) y todo se normaliza al esquema interno. Los atributos de cada
// nodo (propiedades de Neo4j, literales RDF, columnas de una tabla, <data> de GraphML...) van a su nota.
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "core/db.h"
#include "memory/kg.h"
#include "memory/memory.h"
#include "rag/rag.h"
#include "util/archive.h"
#include "util/crypto.h"
#include "util/csv.h"
#include "util/fs.h"
#include "util/rx.h"
#include "util/text.h"
#include "util/xml.h"
#include "util/yaml.h"

namespace nx::kg {

namespace {

const std::string RDF_NS = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

// ------------------------------------------------------------------ valores al estilo Python
std::string uuid_hex_short() { return uuid_hex().substr(0, 10); }
std::vector<std::string> rag_read_safe(const fs::path& p) { return std::get<1>(rag::read_safe(p)); }

bool is_scalar(const json& v) { return v.is_string() || v.is_number() || v.is_boolean(); }
bool empty_val(const json& v) {   // v in (None, "", [], {})
  return v.is_null() || (v.is_string() && v.get_ref<const std::string&>().empty()) ||
         ((v.is_array() || v.is_object()) && v.empty());
}

std::string pystr(const json& v) {   // str(v)
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) return pyfloat(v.get<double>());
  if (v.is_null()) return "None";
  return pydump(v);
}

std::optional<double> pyfloat_of(const json& v) {   // float(v)
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (!v.is_string()) return std::nullopt;
  std::string s = trim(v.get<std::string>());
  std::string l = lower(s);
  if (l == "inf" || l == "+inf" || l == "infinity") return INFINITY;
  if (l == "-inf" || l == "-infinity") return -INFINITY;
  if (l == "nan") return NAN;
  if (s.empty()) return std::nullopt;
  static const Rx NUM(R"(^[+-]?(?:\d[\d_]*(?:\.[\d_]*)?|\.\d[\d_]*)(?:[eE][+-]?\d+)?$)");
  if (!NUM.test(s)) return std::nullopt;
  try {
    return std::stod(replace_all(s, "_", ""));
  } catch (...) {
    return std::nullopt;
  }
}

json first(const json& d, std::initializer_list<const char*> keys, json def = nullptr) {
  if (!d.is_object()) return def;
  for (auto* k : keys) {
    auto it = d.find(k);
    if (it != d.end() && !empty_val(*it)) return *it;
  }
  return def;
}
json first_v(const json& d, const std::vector<std::string>& keys, json def = nullptr) {
  if (!d.is_object()) return def;
  for (auto& k : keys) {
    auto it = d.find(k);
    if (it != d.end() && !empty_val(*it)) return *it;
  }
  return def;
}
json get(const json& d, const char* k, json def = nullptr) {
  if (!d.is_object()) return def;
  auto it = d.find(k);
  return it == d.end() ? def : *it;
}
bool has(const json& d, const char* k) { return d.is_object() && d.contains(k); }

std::string label(const json& value) {
  std::string s = trim(value.is_null() ? "" : pystr(value));
  if (starts_with(s, "<") && ends_with(s, ">") && s.size() >= 2) s = s.substr(1, s.size() - 2);
  if (starts_with(s, "#") && s.size() > 1) s = s.substr(1);
  static const Rx IRI(R"(^[a-z][\w+.-]*://)", Rx::I), SEP(R"([#/:])"), US(R"([_]+)");
  if (IRI.test(s) || starts_with(s, "urn:")) {
    auto parts = SEP.split(rtrim(s, "/#"));
    std::string last = parts.empty() ? "" : parts.back();
    if (!last.empty()) s = last;
  }
  return uleft(trim(US.sub(s, " ")), 60);
}

std::string rel_of(const json& value) {
  std::string s = label(value);
  if (s.empty()) s = "relacionado_con";
  static const Rx CAMEL(R"(([a-z])([A-Z]))"), NONW(R"([^\w]+)");
  s = CAMEL.sub(s, "\\1_\\2");
  s = uleft(trim(NONW.sub(lower(s), "_"), "_"), 30);
  return s.empty() ? "relacionado_con" : s;
}

std::string local(const std::string& key) {
  static const Rx SEP(R"([#/:])");
  auto parts = SEP.split(rtrim(key, "/#"));
  return lower(parts.empty() ? "" : parts.back());
}

const std::vector<std::string> NAME_KEYS = {"name",     "nombre",       "title",    "titulo",    "título",
                                            "label",    "etiqueta",     "caption",  "displayname", "display_name",
                                            "fullname", "full_name",    "preflabel", "headline", "text",
                                            "texto",    "value",        "username"};
const std::set<std::string> NOTE_KEYS = {"description", "descripcion", "descripción", "note",    "nota",
                                         "comment",     "comentario",  "summary",     "resumen", "definition",
                                         "definicion",  "abstract"};
const std::set<std::string> SKIP_PROPS = {"unique import id", "id",    "_id",      "@id",  "identity", "elementid",
                                          "x",                "y",     "z",        "size", "color",    "colour",
                                          "graphics",         "shape", "style",    "fill", "viz",      "position",
                                          "pos",              "width", "height"};

json val(const json& v) {
  if (v.is_object()) {
    if (v.contains("@value")) return val(v["@value"]);
    if (v.contains("value")) {
      bool sub = true;
      for (auto& [k, _] : v.items())
        if (k != "id" && k != "value" && k != "label" && k != "properties" && k != "@type" && k != "key") sub = false;
      if (sub) return val(v["value"]);
    }
  }
  if (v.is_array() && !v.empty()) {
    bool all = true;
    for (auto& x : v)
      if (!(x.is_object() && (x.contains("value") || x.contains("@value")))) all = false;
    if (all) {
      json vals = json::array();
      for (auto& x : v) vals.push_back(val(x));
      return vals.size() == 1 ? vals[0] : vals;
    }
  }
  return v;
}

std::string text(const json& v0) {
  json v = val(v0);
  if (v.is_array()) {
    std::vector<std::string> out;
    for (auto& x : v)
      if (!(x.is_null() || (x.is_string() && x.get<std::string>().empty()))) out.push_back(text(x));
    return join(out, ", ");
  }
  if (v.is_object()) return uleft(pydump(v), 200);
  if (v.is_number_float()) {
    double d = v.get<double>();
    if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e18) return std::to_string((long long)d);
  }
  if (v.is_null()) return "";
  return trim(pystr(v));
}

std::pair<std::string, std::string> name_from(const json& props, bool* found = nullptr) {
  if (found) *found = false;
  if (!props.is_object()) return {"", ""};
  std::map<std::string, std::string> loc;
  for (auto& [k, _] : props.items()) loc[local(k)] = k;   // la ultima clave con el mismo final gana, como el dict
  for (auto& k : NAME_KEYS) {
    auto it = loc.find(k);
    if (it != loc.end()) {
      std::string t = text(props[it->second]);
      if (!t.empty()) {
        if (found) *found = true;
        return {t, it->second};
      }
    }
  }
  return {"", ""};
}

std::string props_note(const json& props, const std::set<std::string>& skip0 = {}) {
  std::set<std::string> skip;
  for (auto& s : skip0) skip.insert(lower(s));
  skip.insert(SKIP_PROPS.begin(), SKIP_PROPS.end());
  std::vector<std::string> out;
  if (!props.is_object()) return "";
  for (auto& [k, v] : props.items()) {
    if (skip.count(lower(k)) || skip.count(local(k)) || starts_with(k, "@")) continue;
    std::string t = text(v);
    if (!t.empty() && ulen(t) < 300) {
      std::string l = label(k);
      out.push_back((l.empty() ? k : l) + ": " + t);
    }
  }
  return join(out, "; ");
}

json gref(const json& x0) {
  json x = val(x0);
  if (x.is_object()) return val(first(x, {"id", "@id", "identity", "elementId", "_id", "key", "name"}));
  return x;
}

bool isdigit_s(const std::string& s) { return is_digit_str(s); }

// ------------------------------------------------------------------ constructor del grafo normalizado
class Builder {
 public:
  std::vector<std::string> order;
  std::unordered_map<std::string, json> nodes;
  std::vector<json> edges;

  bool has_node(const std::string& id) const { return nodes.count(id) > 0; }
  json& at(const std::string& id) { return nodes.at(id); }
  size_t size() const { return nodes.size(); }

  // extra: claves de la llamada en orden (type, category, note, scope, weight...)
  std::string node(const json& nid0, const json& name0 = nullptr, json extra = json::object(),
                   const json* props = nullptr) {
    std::string nid = text(nid0);
    if (nid.empty()) nid = text(name0);
    if (nid.empty()) return "";
    std::string name = text(name0);
    auto it = nodes.find(nid);
    if (it == nodes.end()) {
      if (nodes.size() >= MAX_NODES) throw FormatError("el grafo tiene mas de " + std::to_string(MAX_NODES) + " nodos");
      order.push_back(nid);
      it = nodes.emplace(nid, json{{"id", nid}, {"name", label(name.empty() ? nid : name)}, {"type", "concepto"},
                                   {"category", ""}, {"note", ""}})
               .first;
    }
    json& n = it->second;
    if (!name.empty() && n["name"].get<std::string>() == label(nid)) {
      n["name"] = label(name);
      if (ulen(name) > 60 && !contains(n["note"].get<std::string>(), name)) {
        std::string en = extra.contains("note") && truthy(extra["note"]) ? text(extra["note"]) : "";
        extra["note"] = name + (en.empty() ? "" : ". " + en);
      }
    }
    for (auto& [k, v] : extra.items()) {
      if (empty_val(v)) continue;
      if (k == "weight") {
        n[k] = v;
      } else if (k == "type") {
        if (n["type"] == "concepto") {
          std::string t = uleft(lower(text(v.is_array() ? v[0] : v)), 30);
          n["type"] = t.empty() ? "concepto" : t;
        }
      } else if (k == "note") {
        add_note(n, uleft(text(v), 500));
      } else {
        n[k] = uleft(text(v), 500);
      }
    }
    if (props && props->is_object() && !props->empty()) add_note(n, props_note(*props));
    return nid;
  }

  static void add_note(json& n, const std::string& t) {
    if (t.empty()) return;
    std::string cur = n["note"].get<std::string>();
    if (contains(cur, t)) return;
    n["note"] = uleft(cur.empty() ? t : cur + ". " + t, 500);
  }

  std::string entity(const json& nid, json props = json::object(), const json& name = nullptr, json extra = json::object()) {
    if (!props.is_object()) props = json::object();
    bool found;
    auto [pname, used] = name_from(props, &found);
    json note = nullptr;
    std::set<std::string> skip = {found ? used : "None"};
    if (!props.empty()) {
      std::vector<std::string> nk;
      for (auto& [k, _] : props.items())
        if (NOTE_KEYS.count(local(k))) nk.push_back(k);
      note = first_v(props, nk);
      skip.insert(nk.begin(), nk.end());
    }
    json nm = name;
    if (!truthy(nm)) {
      if (!pname.empty()) {
        nm = pname;
      } else if (extra.contains("type") && truthy(extra["type"]) && isdigit_s(text(nid))) {
        nm = trim(text(extra["type"]) + " " + text(nid));
      } else {
        nm = nullptr;
      }
    }
    std::string id = node(nid, nm, extra);
    if (!id.empty()) {
      node(id, nullptr, json{{"note", note}});
      add_note(nodes[id], props_note(props, skip));
    }
    return id;
  }

  void edge(const json& src, const json& dst, const json& rel = nullptr, const json& weight = 1.0) {
    std::string s = node(src), d = node(dst);
    if (s.empty() || d.empty() || s == d) return;
    auto w = pyfloat_of(val(weight));
    edges.push_back({{"src", s}, {"dst", d}, {"rel", rel_of(rel)}, {"weight", w ? *w : 1.0}});
  }

  void erase(const std::string& id) {
    nodes.erase(id);
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
  }

  json result(const std::string& fmt, bool generic = false) const {
    json ns = json::array();
    for (auto& id : order) {
      auto it = nodes.find(id);
      if (it != nodes.end() && truthy(it->second["name"])) ns.push_back(it->second);
    }
    if (ns.empty()) throw FormatError("no se ha encontrado ningun nodo");
    json es = json::array();
    for (auto& e : edges) es.push_back(e);
    json out = {{"format", fmt}, {"nodes", ns}, {"edges", es}};
    if (generic) out["generic"] = true;
    return out;
  }
};

json obj(std::initializer_list<std::pair<const char*, json>> kv) {
  json o = json::object();
  for (auto& [k, v] : kv) o[k] = v;
  return o;
}

void collapse_blank(Builder& b);

// ------------------------------------------------------------------ JSON
const std::vector<std::string> NODE_LISTS = {"nodes", "vertices", "entities", "entidades", "nodos", "concepts"};
const std::vector<std::string> EDGE_LISTS = {"edges",     "links",   "relationships", "relations",
                                             "arcs",      "aristas", "relaciones",    "enlaces"};
const std::vector<std::string> SRC_KEYS = {"source", "src",   "from",     "fromId",  "fromNode", "startNode",
                                           "start",  "outV",  "_outV",    "head",    "u",        "subject",
                                           "origen", "sujeto", "s",       "node1",   "sourceId"};
const std::vector<std::string> DST_KEYS = {"target",  "dst",  "to",      "toId",   "toNode", "endNode",
                                           "end",     "inV",  "_inV",    "tail",   "v",      "object",
                                           "destino", "objeto", "o",     "node2",  "targetId"};
const std::vector<std::string> REL_KEYS = {"relation", "rel",      "label",       "_label",  "type",
                                           "predicate", "relacion", "relación",   "interaction", "name",
                                           "caption",  "kind",     "i"};
const std::vector<std::string> NODE_ID = {"id", "@id", "key", "_id", "uid", "identity", "elementId", "name", "label"};
const std::vector<std::string> NODE_NAME = {"label", "name", "title", "caption", "text", "nombre", "file", "url", "n"};

json edge_rel(const json& e) {
  json r = first_v(e, REL_KEYS);
  if (r.is_string()) {
    std::string l = lower(r.get<std::string>());
    if (l == "directed" || l == "undirected" || l == "dirigida" || l == "no dirigida" || l == "edge" || l == "link") {
      std::vector<std::string> ks;
      for (auto& k : REL_KEYS)
        if (k != "type" && k != "kind") ks.push_back(k);
      r = first_v(e, ks);
    }
  }
  return is_scalar(r) ? r : json(nullptr);
}

std::string generic_node(Builder& b, const json& n) {
  if (!n.is_object()) return b.node(n, n);
  json pv = first(n, {"properties", "attributes", "data", "v"});
  // n.get("properties") or n.get("attributes")...: el primero que sea verdadero
  pv = nullptr;
  for (auto* k : {"properties", "attributes", "data", "v"})
    if (truthy(get(n, k))) {
      pv = n[k];
      break;
    }
  json props = pv.is_object() ? pv : json::object();
  json meta = get(n, "metadata").is_object() ? n["metadata"] : json::object();
  for (auto& [k, v] : meta.items())
    if (k != "type" && k != "description") props[k] = v;
  json nid = gref(first_v(n, NODE_ID));
  if (!truthy(nid)) {
    bool f;
    auto pn = name_from(props, &f).first;
    nid = f ? json(pn) : json(nullptr);
  }
  json name = first_v(n, NODE_NAME);
  if (name.is_object()) name = nullptr;
  json labels = get(n, "labels").is_array() ? n["labels"] : json(nullptr);
  json etype = (truthy(labels) ? labels[0] : json(nullptr));
  if (!truthy(etype)) etype = first(n, {"type", "group", "kind", "_type"});
  if (!truthy(etype)) etype = get(meta, "type");
  if (etype.is_array() || etype.is_object()) etype = nullptr;
  static const std::set<std::string> EXCL = {"id",      "@id",   "key",    "_id",      "uid",     "identity", "elementId",
                                             "name",    "label", "title",  "caption",  "text",    "nombre",   "file",
                                             "url",     "n",     "properties", "attributes", "data", "v",       "metadata",
                                             "labels",  "type",  "group",  "kind",     "_type",   "category", "scope",
                                             "weight",  "children", "adjacency", "outE", "inE",   "edges",    "links"};
  json rest = json::object();
  for (auto& [k, v] : n.items())
    if (!EXCL.count(k) && !v.is_object() && !v.is_array()) rest[k] = v;
  json note = first(n, {"description", "note"}, get(meta, "description"));
  if (get(n, "type") == "text" && get(n, "text").is_string()) {
    std::string t = trim(n["text"].get<std::string>());
    auto lines = splitlines(t);
    name = trim(ltrim(lines.empty() ? "" : lines[0], "# "));
    note = t;
  } else if (get(n, "type") == "file" && truthy(get(n, "file"))) {
    name = stem(fs::path(text(n["file"])));
  }
  json all = rest;
  for (auto& [k, v] : props.items()) all[k] = v;
  return b.entity(nid, all, name,
                  obj({{"type", etype}, {"category", get(n, "category")}, {"scope", get(n, "scope")},
                       {"weight", get(n, "weight")}, {"note", note}}));
}

int generic_edges(Builder& b, const json& edges) {
  int n = 0;
  if (!edges.is_array()) return 0;
  for (auto& e : edges) {
    if (e.is_object()) {
      json s = gref(first_v(e, SRC_KEYS)), t = gref(first_v(e, DST_KEYS));
      if (!s.is_null() && !t.is_null()) {
        json props = nullptr;
        for (auto* k : {"properties", "data", "v"})
          if (truthy(get(e, k))) {
            props = e[k];
            break;
          }
        if (props.is_null()) props = json::object();
        json rel = edge_rel(e);
        if (rel.is_null() && props.is_object()) rel = edge_rel(props);
        json w = has(e, "weight") ? e["weight"]
                 : has(e, "value") ? e["value"]
                 : props.is_object() ? get(props, "weight", 1)
                                     : json(1);
        b.edge(s, t, rel, w);
        ++n;
      }
    } else if (e.is_array() && e.size() >= 2) {
      b.edge(e[0], e.back(), e.size() > 2 ? e[1] : json(nullptr));
      ++n;
    }
  }
  return n;
}

bool neo_obj(Builder& b, const json& o) {
  if (!o.is_object()) return false;
  if (o.contains("segments")) {
    for (auto& seg : o["segments"]) {
      neo_obj(b, get(seg, "start"));
      neo_obj(b, get(seg, "end"));
      neo_obj(b, get(seg, "relationship"));
    }
    return true;
  }
  if (((o.contains("start") && o.contains("end")) || (o.contains("startNode") && o.contains("endNode"))) &&
      (o.contains("type") || o.contains("label"))) {
    json rel = get(o, "type") == "relationship" ? get(o, "label") : get(o, "type");
    for (auto* side : {"start", "end"})
      if (get(o, side).is_object() && !get(o[side], "labels").is_null()) neo_obj(b, o[side]);
    json props = truthy(get(o, "properties")) ? o["properties"] : json::object();
    b.edge(gref(has(o, "start") ? o["start"] : get(o, "startNode")), gref(has(o, "end") ? o["end"] : get(o, "endNode")),
           rel, get(props, "weight", 1));
    return true;
  }
  if (o.contains("labels") && (o.contains("identity") || o.contains("elementId") || o.contains("id"))) {
    json labels = truthy(o["labels"]) ? o["labels"] : json::array();
    b.entity(gref(o), truthy(get(o, "properties")) ? o["properties"] : json::object(), nullptr,
             obj({{"type", labels.is_array() && !labels.empty() ? labels[0] : json(nullptr)}}));
    return true;
  }
  return false;
}

json untype(const json& o) {
  if (o.is_object()) {
    if (o.size() == 2 && o.contains("@type") && o.contains("@value")) {
      std::string t = pystr(o["@type"]);
      if (starts_with(t, "g:") || starts_with(t, "gx:")) return untype(o["@value"]);
    }
    json out = json::object();
    for (auto& [k, v] : o.items()) out[k] = untype(v);
    return out;
  }
  if (o.is_array()) {
    json out = json::array();
    for (auto& x : o) out.push_back(untype(x));
    return out;
  }
  return o;
}

void graphson_vertex(Builder& b, const json& v) {
  json props = json::object();
  if (truthy(get(v, "properties")) && v["properties"].is_object())
    for (auto& [k, p] : v["properties"].items()) props[k] = val(p);
  for (auto& [k, x] : v.items())
    if (!starts_with(k, "_") && k != "id" && k != "label" && k != "properties" && k != "outE" && k != "inE" && k != "type")
      props[k] = x;
  json vid = gref(first(v, {"id", "_id"}));
  b.entity(vid, props, nullptr, obj({{"type", first(v, {"label", "_label"})}}));
  if (truthy(get(v, "outE")) && v["outE"].is_object())
    for (auto& [el, es] : v["outE"].items())
      for (auto& e : es) {
        json p = truthy(get(e, "properties")) ? e["properties"] : json::object();
        b.edge(vid, gref(get(e, "inV")), el, val(get(p, "weight", 1)));
      }
}

void jsonld(Builder& b, const json& data) {
  int counter = 0;
  std::function<std::string(const json&)> node;
  std::function<void(const std::string&, const std::string&, const json&, json&)> refl;
  node = [&](const json& o) -> std::string {
    if (!o.is_object()) return "";
    std::string nid = text(get(o, "@id"));
    if (nid.empty()) nid = "_:b" + std::to_string(++counter);
    json types = get(o, "@type");
    json t0 = types.is_array() && !types.empty() ? types[0] : types;
    std::string et = label(truthy(t0) ? t0 : json(""));
    json literals = json::object();
    for (auto& [k, vals] : o.items()) {
      if (starts_with(k, "@")) continue;
      json list = vals.is_array() ? vals : json::array({vals});
      for (auto& v : list) {
        if (v.is_object() && v.contains("@list")) {
          for (auto& x : v["@list"]) refl(nid, k, x, literals);
          continue;
        }
        refl(nid, k, v, literals);
      }
    }
    bool f;
    auto nm = name_from(literals, &f).first;
    b.entity(nid, literals, f ? json(nm) : json(nullptr), obj({{"type", et.empty() ? json(nullptr) : json(et)}}));
    return nid;
  };
  refl = [&](const std::string& nid, const std::string& k, const json& v, json& literals) {
    bool nonat = false;
    if (v.is_object())
      for (auto& [x, _] : v.items())
        if (!starts_with(x, "@")) nonat = true;
    if (v.is_object() && (v.contains("@id") || nonat) && !v.contains("@value")) {
      b.edge(nid, node(v), k);
    } else {
      std::string lk = local(k);
      if (lk == "label" || lk == "name" || lk == "preflabel" || lk == "title" || (!v.is_object() && !v.is_array()) ||
          (v.is_object() && v.contains("@value")))
        if (!literals.contains(k)) literals[k] = val(v);
    }
  };
  json items = data.is_object() ? get(data, "@graph", data) : data;
  if (data.is_object() && !data.contains("@graph")) items = data;
  if (items.is_array()) {
    for (auto& o : items) node(o);
  } else {
    node(items);
  }
  collapse_blank(b);
}

int tree(Builder& b, const json& o, const std::string& parent, const std::string& rel = "contiene", int depth = 0) {
  if (depth > 60 || !o.is_object()) return 0;
  bool f;
  auto [name, used] = name_from(o, &f);
  if (!f) name = text(first(o, {"id", "key"}));
  if (name.empty()) return 0;
  std::string nid = text(first(o, {"id", "@id", "key"}));
  if (nid.empty()) nid = parent + "/" + name;
  json scal = json::object();
  for (auto& [k, v] : o.items())
    if (!v.is_object() && !v.is_array()) scal[k] = v;
  json tk = first(o, {"type", "kind"});
  b.entity(nid, scal, name, obj({{"type", tk.is_string() ? tk : json(nullptr)}}));
  int made = 0;
  if (!parent.empty()) {
    b.edge(parent, nid, rel);
    ++made;
  }
  for (auto& [k, v] : o.items()) {
    json kids = v.is_object() && get(v, "attached").is_array() ? v["attached"] : v;
    if (kids.is_object()) kids = json::array({kids});
    if (!kids.is_array()) continue;
    std::string r = (k == "children" || k == "hijos" || k == "topics" || k == "attached" || k == "items" ||
                     k == "subtopics" || k == "rootTopic")
                        ? "contiene"
                        : k;
    for (auto& c : kids) {
      if (c.is_object()) {
        made += tree(b, c, nid, r, depth + 1);
      } else if (is_scalar(c) && ulen(text(c)) <= 60 && k != "labels" && k != "markers") {
        b.edge(nid, b.node(k + ":" + pystr(c), c), k);
        ++made;
      }
    }
  }
  return made;
}

json as_list(const json& nodes) {
  if (nodes.is_object()) {
    json out = json::array();
    for (auto& [k, v] : nodes.items()) {
      json o = {{"id", k}};
      if (v.is_object()) {
        for (auto& [kk, vv] : v.items()) o[kk] = vv;
      } else {
        o["label"] = v;
      }
      out.push_back(o);
    }
    return out;
  }
  return truthy(nodes) ? nodes : json::array();
}

bool triple(Builder& b, const json& t) {
  if (t.is_array() && t.size() >= 3 && is_scalar(t[0]) && is_scalar(t[1]) && is_scalar(t[2])) {
    b.edge(b.node(t[0], t[0]), b.node(t[2], t[2]), t[1], t.size() > 3 ? t[3] : json(1.0));
    return true;
  }
  if (t.is_object()) {
    json s = first(t, {"s", "subject", "sujeto", "head", "from"});
    json r = first(t, {"r", "p", "predicate", "relation", "relacion", "rel"});
    json o = first(t, {"o", "object", "objeto", "tail", "to"});
    if (!s.is_null() && !o.is_null() && !s.is_object() && !s.is_array() && !o.is_object() && !o.is_array()) {
      b.node(s, s);
      json cat = truthy(get(t, "category")) ? t["category"] : get(t, "categoria");
      b.node(o, o, obj({{"type", get(t, "type")}, {"category", cat}}));
      b.edge(s, o, r, get(t, "weight", 1.0));
      return true;
    }
  }
  return false;
}

}  // namespace

json parse_json(json data) {
  Builder b;
  if ((data.is_object() || data.is_array()) && contains(uleft(pydump(data), 20000), "\"@type\": \"g:")) data = untype(data);
  if (data.is_object() && get(data, "format") == "nexus-kg") {
    for (auto& n : get(data, "nodes", json::array())) {
      b.node(n.at("id"), get(n, "name"),
             obj({{"type", get(n, "type")}, {"category", get(n, "category")}, {"note", get(n, "note")},
                  {"scope", get(n, "scope", "")}, {"weight", get(n, "weight")}}));
      if (get(n, "scope", "") == "") b.at(text(n["id"]))["scope"] = "";
    }
    for (auto& e : get(data, "edges", json::array())) b.edge(e.at("src"), e.at("dst"), get(e, "rel"), get(e, "weight", 1.0));
    return b.result("nexus-kg");
  }
  if (data.is_object() && data.contains("elements")) {   // Cytoscape (.cyjs)
    json el = data["elements"];
    json items = json::array();
    if (el.is_array()) {
      items = el;
    } else {
      for (auto& x : get(el, "nodes", json::array())) items.push_back(x);
      for (auto& x : get(el, "edges", json::array())) items.push_back(x);
    }
    for (auto& it : items) {
      json d = get(it, "data", it);
      if (has(d, "source") && has(d, "target")) {
        b.edge(d["source"], d["target"], first(d, {"label", "interaction", "relation", "name"}), get(d, "weight", 1));
      } else if (has(d, "id")) {
        json props = json::object();
        for (auto& [k, v] : d.items())
          if (k != "id" && k != "label" && k != "name" && k != "type" && k != "description") props[k] = v;
        b.entity(d["id"], props, first(d, {"label", "name"}),
                 obj({{"type", get(d, "type")}, {"note", get(d, "description")}}));
      }
    }
    return b.result("cytoscape");
  }
  if (data.is_array() && !data.empty()) {   // Cytoscape CX / CX2: lista de aspectos
    bool all = true, any = false;
    for (auto& x : data) {
      if (!(x.is_object() && x.size() <= 3)) all = false;
      if (has(x, "nodes") || has(x, "edges")) any = true;
    }
    if (all && any) {
      for (auto& asp : data) {
        for (auto& n : get(asp, "nodes", json::array()))
          b.entity(first(n, {"@id", "id"}), truthy(get(n, "v")) ? n["v"] : json::object(), get(n, "n"));
        for (auto& e : get(asp, "edges", json::array())) {
          json r = first(e, {"i"});
          if (!truthy(r)) r = get(truthy(get(e, "v")) ? e["v"] : json::object(), "interaction");
          b.edge(first(e, {"s", "source"}), first(e, {"t", "target"}), r);
        }
      }
      return b.result("cytoscape-cx");
    }
  }
  bool ld = data.is_object() && (data.contains("@graph") || data.contains("@context"));
  if (!ld && data.is_array() && !data.empty()) {
    ld = true;
    for (size_t i = 0; i < data.size() && i < 20; ++i)
      if (!(data[i].is_object() && data[i].contains("@id"))) ld = false;
  }
  if (ld) {
    jsonld(b, data);
    return b.result("json-ld");
  }
  if (data.is_object() && get(data, "results").is_array()) {   // Neo4j HTTP API
    for (auto& res : data["results"])
      for (auto& row : get(res, "data", json::array())) {
        json g = truthy(get(row, "graph")) ? row["graph"] : json::object();
        for (auto& n : get(g, "nodes", json::array())) neo_obj(b, n);
        for (auto& r : get(g, "relationships", json::array())) neo_obj(b, r);
        if (!truthy(g))
          for (auto& v : get(row, "row", json::array())) neo_obj(b, v);
      }
    if (b.size()) return b.result("neo4j");
  }
  if (data.is_array() && !data.empty()) {
    bool all = true;
    for (size_t i = 0; i < data.size() && i < 50; ++i)
      if (!data[i].is_object()) all = false;
    if (all) {
      bool apoc = false, gson = false;
      for (size_t i = 0; i < data.size() && i < 50; ++i) {
        json t = get(data[i], "type");
        if (t == "node" || t == "relationship") apoc = true;
        const json& x = data[i];
        if (x.contains("outE") || x.contains("inE")) gson = true;
        if (x.contains("label") && get(x, "properties").is_object()) {
          bool lists = true;
          for (auto& [_, v] : x["properties"].items())
            if (!v.is_array()) lists = false;
          if (lists) gson = true;
        }
      }
      if (apoc) {
        for (auto& x : data) {
          if (get(x, "type") == "node") {
            json labels = truthy(get(x, "labels")) ? x["labels"] : json::array();
            b.entity(get(x, "id"), truthy(get(x, "properties")) ? x["properties"] : json::object(), nullptr,
                     obj({{"type", labels.is_array() && !labels.empty() ? labels[0] : json(nullptr)}}));
          } else if (get(x, "type") == "relationship") {
            neo_obj(b, x);
          }
        }
        return b.result("neo4j-apoc");
      }
      if (gson) {
        for (auto& x : data) graphson_vertex(b, x);
        return b.result("graphson");
      }
      int neo = 0;
      for (auto& rec : data)
        for (auto& [_, v] : rec.items()) {
          if (v.is_array()) {
            for (auto& o : v) neo += neo_obj(b, o);
          } else {
            neo += neo_obj(b, v);
          }
        }
      if (neo) return b.result("neo4j");
    }
  }
  if (data.is_object() && get(data, "vertices").is_array() && !truthy(get(data, "nodes"))) {   // GraphSON
    for (auto& v : data["vertices"]) graphson_vertex(b, v);
    generic_edges(b, get(data, "edges"));
    return b.result("graphson");
  }
  if (data.is_array() && !data.empty() && data[0].is_object() && data[0].contains("rootTopic")) {   // XMind
    for (auto& sheet : data) tree(b, sheet.at("rootTopic"), "");
    return b.result("xmind");
  }
  json g = data;
  if (data.is_object() && get(data, "graph").is_object()) {
    bool any = data["graph"].contains("graphs");
    for (auto& k : NODE_LISTS) any = any || data["graph"].contains(k);
    for (auto& k : EDGE_LISTS) any = any || data["graph"].contains(k);
    if (any) g = data["graph"];
  }
  if (g.is_object() && g.contains("graphs") && g["graphs"].is_array()) {   // JGF con varios grafos
    json ns = json::array(), es = json::array();
    for (auto& x : g["graphs"]) {
      for (auto& n : as_list(get(x, "nodes"))) ns.push_back(n);
      for (auto& e : get(x, "edges", json::array())) es.push_back(e);
    }
    g = {{"nodes", ns}, {"edges", es}};
  }
  if (g.is_object()) {
    bool any = false;
    for (auto& k : NODE_LISTS) any = any || g.contains(k);
    for (auto& k : EDGE_LISTS) any = any || g.contains(k);
    if (any) {
      json nodes = json::array();
      for (auto& k : NODE_LISTS)
        if (g.contains(k)) {
          nodes = g[k];
          break;
        }
      nodes = as_list(nodes);
      std::vector<std::string> ids;
      if (nodes.is_array())
        for (auto& n : nodes) ids.push_back(generic_node(b, n));
      for (auto& k : EDGE_LISTS)
        if (g.contains(k)) generic_edges(b, g[k]);
      if (get(g, "adjacency").is_array()) {   // NetworkX adjacency_data
        std::set<std::pair<std::string, std::string>> seen;
        auto& adj = g["adjacency"];
        for (size_t i = 0; i < adj.size(); ++i) {
          if (i >= ids.size() || !adj[i].is_array()) continue;
          for (auto& nb : adj[i]) {
            if (!nb.is_object()) continue;
            std::pair<std::string, std::string> pair{text(gref(nb)), ids[i]};
            if (get(g, "directed") == false && seen.count(pair)) continue;
            seen.insert({pair.second, pair.first});
            b.edge(ids[i], gref(nb), edge_rel(nb), get(nb, "weight", 1));
          }
        }
      }
      return b.result("json");
    }
  }
  json rows = data.is_array() ? data : json(nullptr);
  if (data.is_object()) {
    rows = nullptr;
    for (auto* k : {"triples", "facts", "tripletas"})
      if (truthy(get(data, k))) {
        rows = data[k];
        break;
      }
  }
  if (rows.is_array()) {
    int n = 0;
    for (auto& t : rows) n += triple(b, t);
    if (n) return b.result("triples");
  }
  if (data.is_object() && !data.empty()) {
    bool adj = true;
    for (auto& [_, v] : data.items()) {
      if (!(v.is_array() && !v.empty())) {
        adj = false;
        break;
      }
      for (auto& x : v)
        if (!is_scalar(x)) adj = false;
    }
    if (adj) {   // {"A": ["B", "C"]}
      for (auto& [k, vs] : data.items())
        for (auto& v : vs) b.edge(k, v);
      return b.result("adyacencia");
    }
    bool dod = true;
    for (auto& [_, v] : data.items()) {
      if (!(v.is_object() && !v.empty())) {
        dod = false;
        break;
      }
      for (auto& [__, x] : v.items())
        if (!x.is_object()) dod = false;
    }
    if (dod) {   // NetworkX to_dict_of_dicts
      for (auto& [k, nbrs] : data.items())
        for (auto& [nb, attrs] : nbrs.items()) b.edge(k, nb, edge_rel(attrs), get(attrs, "weight", 1));
      return b.result("adyacencia");
    }
  }
  if (data.is_object() && (get(data, "children").is_array() || data.contains("rootTopic"))) {
    tree(b, get(data, "rootTopic", data), "");
    return b.result("arbol");
  }
  if (data.is_array()) {
    for (auto& o : data)
      if (o.is_object()) tree(b, o, "");
  } else {
    tree(b, data, "");
  }
  if (!b.size()) throw FormatError("el JSON no tiene nodos/enlaces, tripletas ni objetos con nombre reconocibles");
  return b.result("json-generico", true);
}

namespace {

json parse_jsonl(const std::string& t) {
  Builder b;
  json objs = json::array();
  int n = 0;
  for (auto& line0 : splitlines(t)) {
    std::string line = rtrim(trim(line0), ",");
    if (line.empty()) continue;
    json o = jparse(line);
    if (o.is_discarded()) continue;
    if (triple(b, o)) {
      ++n;
    } else {
      objs.push_back(o);
    }
  }
  if (!objs.empty()) {
    json g = nullptr;
    try {
      g = parse_json(objs);
    } catch (const FormatError&) {
    }
    if (!g.is_null() && !(n && truthy(get(g, "generic")))) {
      if (!n) return g;
      return merge({{"tripletas", b.result("jsonl")}, {"objetos", g}});
    }
  }
  if (!n) throw FormatError("ninguna linea es una tripleta o un objeto de grafo JSON");
  return b.result("jsonl");
}

json parse_yaml(const std::string& t) {
  // varios documentos separados por ---: como yaml.safe_load_all
  static const Rx DOCSEP(R"((?m)^---\s*$)");
  json docs = json::array();
  for (auto& part : DOCSEP.split(t)) {
    if (trim(part).empty()) continue;
    json d;
    try {
      d = yaml::load(part);
    } catch (const std::exception& e) {
      throw FormatError(std::string("YAML no valido: ") + e.what());
    }
    if (!d.is_null()) docs.push_back(d);
  }
  if (docs.empty()) throw FormatError("el YAML esta vacio");
  json g = parse_json(docs.size() == 1 ? docs[0] : docs);
  g["format"] = truthy(get(g, "generic")) ? "yaml-generico" : "yaml";
  return g;
}

json parse_toml(const std::string& t) {
  json d;
  try {
    d = toml::load(t);
  } catch (const std::exception& e) {
    throw FormatError(e.what());
  }
  json g = parse_json(d);
  g["format"] = truthy(get(g, "generic")) ? "toml-generico" : "toml";
  return g;
}

// ------------------------------------------------------------------ tablas (CSV, TSV, hojas, SQLite)
const std::vector<std::string> SRC = {"source", "src",       "from",     "origen",    "s",        "subject",
                                      "sujeto", "head",      "node1",    "source_id", "sourceid", "source id",
                                      "from_id", "nodo1",    "start",    "inicio",    "parent",   "padre"};
const std::vector<std::string> DST = {"target", "dst",       "to",       "destino",   "o",        "object",
                                      "objeto", "tail",      "node2",    "target_id", "targetid", "target id",
                                      "to_id",  "nodo2",     "end",      "fin",       "child",    "hijo"};
const std::vector<std::string> REL = {"relation", "rel", "label", "predicate", "relacion", "relación", "interaction",
                                      "r",        "p",   "edge",  "type",      "tipo",     "kind",     "name"};
const std::vector<std::string> ID_COLS = {"id",     "node",       "nodo", "key",       "node_id",
                                          "nodeid", "identifier", "uid",  "entity_id", "vertex"};
const std::vector<std::string> NAME_COLS = {"label",  "name",    "nombre",  "title",    "titulo",
                                            "título", "entity",  "entidad", "concepto", "concept"};
const std::vector<std::string> TYPE_COLS = {"type", "tipo", "group", "grupo", "class", "clase", "labels", "kind"};
const std::vector<std::string> NOTE_COLS = {"description", "descripcion", "descripción", "note",
                                            "nota",        "comment",     "comentario",  "definition"};
const std::vector<std::string> WEIGHT_COLS = {"weight", "peso", "value", "valor", "strength"};

std::optional<double> numv(const std::string& s0) {   // _num de las tablas
  std::string s = trim(s0);
  if (s.empty()) return 0.0;
  return pyfloat_of(json(replace_all(s, ",", ".")));
}

std::pair<std::string, std::string> neo_col(const std::string& h) {
  static const Rx M(R"(^\s*([^:]*?)\s*:\s*(ID|START_ID|END_ID|TYPE|LABEL)\b)", Rx::I), T(R"(:\w+(\[\])?$)");
  auto m = M.search(h);
  if (m) return {m->str(1), upper(m->str(2))};
  return {trim(T.sub(h, "")), ""};
}

using Rows = std::vector<std::vector<std::string>>;

std::string rows_to_graph(Builder& b, Rows rows_in) {
  Rows rows;
  for (auto& r : rows_in) {
    bool any = false;
    for (auto& c : r)
      if (!trim(c).empty()) any = true;
    if (!any) continue;
    std::vector<std::string> rr;
    for (auto& c : r) rr.push_back(trim(c));
    rows.push_back(rr);
  }
  if (rows.empty()) throw FormatError("tabla vacia");
  auto raw = rows[0];
  std::vector<std::string> head;
  for (auto& c : raw) head.push_back(trim(trim(lower(c)), "\""));
  Rows body(rows.begin() + 1, rows.end());
  std::vector<std::pair<std::string, std::string>> neo;
  bool neo_any = false;
  for (auto& h : raw) {
    neo.push_back(neo_col(h));
    if (!neo.back().second.empty()) neo_any = true;
  }
  auto cell = [](const std::vector<std::string>& r, long i) -> std::string { return i >= 0 && i < (long)r.size() ? r[i] : ""; };
  if (neo_any) {   // neo4j-admin import
    std::vector<std::string> kinds, cols;
    for (auto& [n, k] : neo) {
      cols.push_back(n);
      kinds.push_back(k);
    }
    auto idx = [&](const char* k) -> long {
      auto it = std::find(kinds.begin(), kinds.end(), k);
      return it == kinds.end() ? -1 : (long)(it - kinds.begin());
    };
    auto props = [&](const std::vector<std::string>& r, std::set<long> skip) {
      json p = json::object();
      for (long i = 0; i < (long)std::min(r.size(), cols.size()); ++i)
        if (!skip.count(i) && !cols[i].empty() && !r[i].empty()) p[cols[i]] = r[i];
      return p;
    };
    long si = idx("START_ID"), di = idx("END_ID");
    if (si >= 0 && di >= 0) {
      long ti = idx("TYPE");
      for (auto& r : body)
        if ((long)r.size() > std::max(si, di))
          b.edge(r[si], r[di], ti >= 0 && ti < (long)r.size() ? json(r[ti]) : json(nullptr),
                 get(props(r, {si, di, ti}), "weight", 1));
      return "neo4j-aristas";
    }
    long ii = idx("ID") >= 0 ? idx("ID") : 0, li = idx("LABEL");
    for (auto& r : body) {
      if ((long)r.size() > ii && !r[ii].empty()) {
        json p = props(r, {ii, li});
        if (!cols[ii].empty()) p[cols[ii]] = r[ii];
        json t = li >= 0 && li < (long)r.size() && !r[li].empty() ? json(split(r[li], ";")[0]) : json(nullptr);
        b.entity(r[ii], p, nullptr, obj({{"type", t}}));
      }
    }
    return "neo4j-nodos";
  }
  if (raw.size() >= 4 && starts_with(raw[0], "/a/") && starts_with(raw[1], "/r/")) {   // ConceptNet
    for (auto& r : rows) {
      if (r.size() < 4) continue;
      auto rp = split(r[1], "/");
      b.edge(r[2], r[3], rp.back());
      auto nm = [](const std::string& x) {
        auto p = split(x, "/");
        return std::count(x.begin(), x.end(), '/') >= 3 ? p[3] : x;
      };
      b.node(r[2], nm(r[2]));
      b.node(r[3], nm(r[3]));
    }
    return "conceptnet";
  }
  auto col = [&](const std::vector<std::string>& names, std::set<long> excl = {}) -> long {
    for (auto& n : names)
      for (long i = 0; i < (long)head.size(); ++i)
        if (head[i] == n && !excl.count(i)) return i;
    return -1;
  };
  long si = col(SRC), di = col(DST);
  if (si >= 0 && di >= 0 && si != di) {
    long ri = col(REL, {si, di});
    if (ri >= 0) {
      bool dir = true;
      for (size_t k = 0; k < body.size() && k < 50; ++k)
        if (ri < (long)body[k].size()) {
          std::string v = lower(body[k][ri]);
          if (v != "directed" && v != "undirected" && !v.empty()) dir = false;
        }
      if (dir) {
        std::vector<std::string> rr;
        for (auto& x : REL)
          if (x != "type" && x != "tipo" && x != "kind") rr.push_back(x);
        ri = col(rr, {si, di});
      }
    }
    long wi = col(WEIGHT_COLS, {si, di});
    for (auto& r : body)
      if ((long)r.size() > std::max(si, di) && !r[si].empty() && !r[di].empty())
        b.edge(r[si], r[di], ri >= 0 && ri < (long)r.size() ? json(r[ri]) : json(nullptr),
               wi >= 0 && wi < (long)r.size() ? json(r[wi]) : json(1.0));
    return "aristas";
  }
  long ii = col(ID_COLS), ni = col(NAME_COLS);
  bool head_text = true;
  for (auto& h : head)
    if (!h.empty() && numv(h)) head_text = false;
  if ((ii >= 0 || ni >= 0) && !body.empty() && head.size() >= 2 && head_text) {
    long ti = col(TYPE_COLS, {ii, ni}), oi = col(NOTE_COLS, {ii, ni});
    long key = ii >= 0 ? ii : ni;
    for (auto& r : body) {
      if ((long)r.size() > key && !r[key].empty()) {
        json p = json::object();
        for (long i = 0; i < (long)std::min(r.size(), raw.size()); ++i)
          if (i != ii && i != ni && i != ti && i != oi && !r[i].empty()) p[raw[i]] = r[i];
        b.entity(r[key], p, ni >= 0 && ni < (long)r.size() && !r[ni].empty() ? json(r[ni]) : json(nullptr),
                 obj({{"type", ti >= 0 && ti < (long)r.size() ? json(r[ti]) : json(nullptr)},
                      {"note", oi >= 0 && oi < (long)r.size() ? json(r[oi]) : json(nullptr)}}));
      }
    }
    return "nodos";
  }
  size_t n = raw.size();
  bool matrix = n >= 3 && body.size() >= 2;
  for (auto& r : body) {
    if (!matrix) break;
    if (r.size() != n) matrix = false;
    for (size_t j = 1; matrix && j < r.size(); ++j)
      if (!numv(r[j])) matrix = false;
  }
  if (matrix) {   // matriz de adyacencia con nombres
    std::vector<std::string> names(raw.begin() + 1, raw.end());
    for (auto& r : body)
      for (size_t j = 1; j < r.size(); ++j) {
        double w = *numv(r[j]);
        if (w) b.edge(r[0], names[j - 1], nullptr, w);
      }
    for (auto& nm : names) b.node(nm, nm);
    return "matriz";
  }
  if (n >= 3) {   // sin cabecera: sujeto, relacion, objeto[, peso]
    for (auto& r : rows)
      if (r.size() >= 3 && !r[0].empty() && !r[2].empty()) {
        auto w = r.size() > 3 ? numv(r[3]) : std::nullopt;
        b.edge(r[0], r[2], r[1], w ? *w : 1.0);
      }
    return "tripletas";
  }
  if (n == 2) {
    for (auto& r : rows)
      if (r.size() == 2 && !r[0].empty() && !r[1].empty()) b.edge(r[0], r[1]);
    return "pares";
  }
  (void)cell;
  throw FormatError("la tabla necesita columnas origen/destino, una columna id/nombre o tres columnas sujeto,relacion,objeto");
}

char sniff_delimiter(const std::string& sample) {
  char best = ',';
  long bc = -1;
  for (char d : {'\t', ',', ';', '|'}) {
    long c = std::count(sample.begin(), sample.end(), d);
    if (c > bc || (c == bc && d == ',')) {
      bc = c;
      best = d;
    }
  }
  return best;
}

json parse_table(const std::string& t, char delim = 0) {
  if (!delim) delim = sniff_delimiter(t.substr(0, 4096));
  auto rows = csv::read(t, delim);
  Builder b;
  std::string kind = rows_to_graph(b, rows);
  json g = b.result(delim == ',' ? "csv" : delim == '\t' ? "tsv" : "tabla");
  g["table"] = kind;
  return g;
}

json parse_spreadsheet(const std::string& data, const std::string& name) {
  // xlsx/ods: se leen con los lectores de la biblioteca (una pagina por hoja, celdas separadas por tabuladores)
  std::string ext = ends_with(lower(name), ".ods") ? ".ods" : ".xlsx";
  fs::path tmp = paths::sub({"cache"}) / ("hoja_" + uuid_hex_short() + ext);
  write_file(tmp, data);
  std::vector<std::string> pages;
  try {
    pages = rag_read_safe(tmp);
  } catch (...) {
  }
  remove_file(tmp);
  Builder b;
  std::vector<std::string> kinds;
  for (auto& page : pages) {
    Rows rows;
    for (auto& line : splitlines(page)) rows.push_back(split(line, "\t"));
    try {
      kinds.push_back(rows_to_graph(b, rows));
    } catch (const FormatError&) {
    }
  }
  if (kinds.empty()) throw FormatError("ninguna hoja tiene forma de grafo (origen/destino, id/nombre o tripletas)");
  json g = b.result(ext == ".xlsx" ? "excel" : "ods");
  g["table"] = join(kinds, "+");
  return g;
}

json parse_sqlite(const std::string& data) {
  fs::path tmp = paths::sub({"cache"}) / ("grafo_" + uuid_hex_short() + ".db");
  write_file(tmp, data);
  struct Rm {
    fs::path p;
    ~Rm() { remove_file(p); }
  } rm{tmp};
  sqlite3* db = nullptr;
  std::string uri = "file:" + tmp.string() + "?mode=ro";
  if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    throw FormatError("no se pudo abrir la base SQLite");
  }
  auto query = [&](const std::string& sql) {
    Rows out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      std::string e = sqlite3_errmsg(db);
      throw FormatError("SQLite: " + e);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      std::vector<std::string> r;
      for (int i = 0; i < sqlite3_column_count(st); ++i) {
        int ty = sqlite3_column_type(st, i);
        if (ty == SQLITE_NULL) {
          r.push_back("\x01NULL");
        } else if (ty == SQLITE_FLOAT) {
          r.push_back(pyfloat(sqlite3_column_double(st, i)));
        } else {
          auto* t = (const char*)sqlite3_column_text(st, i);
          r.push_back(t ? std::string(t, sqlite3_column_bytes(st, i)) : "");
        }
      }
      out.push_back(r);
    }
    sqlite3_finalize(st);
    return out;
  };
  json g;
  try {
    std::vector<std::string> tables;
    for (auto& r : query("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"))
      tables.push_back(r[0]);
    std::map<std::string, std::vector<std::string>> cols;
    for (auto& t : tables)
      for (auto& r : query("PRAGMA table_info(\"" + t + "\")")) cols[t].push_back(r[1]);
    auto has_cols = [&](const std::string& t, std::initializer_list<const char*> want) {
      auto& c = cols[t];
      for (auto* w : want)
        if (std::find(c.begin(), c.end(), w) == c.end()) return false;
      return true;
    };
    auto nn = [](const std::string& v) { return v == "\x01NULL" ? json(nullptr) : json(v); };
    Builder b;
    if (has_cols("entities", {"scope", "key", "name"}) && has_cols("relations", {"src", "dst", "rel"})) {
      for (auto& r : query("SELECT id, name, type, category, note, scope, weight FROM entities LIMIT " + std::to_string(MAX_NODES))) {
        json w = nn(r[6]);
        if (w.is_string()) {
          auto f = pyfloat_of(w);
          if (f) w = *f;
        }
        b.node(nn(r[0]), nn(r[1]),
               obj({{"type", nn(r[2])}, {"category", nn(r[3])}, {"note", nn(r[4])}, {"scope", nn(r[5])}, {"weight", w}}));
        b.at(text(nn(r[0])))["scope"] = r[5] == "\x01NULL" ? "" : r[5];
      }
      for (auto& r : query("SELECT src, dst, rel, weight FROM relations")) b.edge(nn(r[0]), nn(r[1]), nn(r[2]), nn(r[3]));
      g = b.result("nexus-kg");
    } else {
      std::vector<std::string> kinds;
      std::vector<std::string> sorted = tables;
      std::stable_sort(sorted.begin(), sorted.end(), [&](const std::string& a, const std::string& c) {
        auto is_edge = [&](const std::string& t) {
          for (auto& x : cols[t])
            if (std::find(SRC.begin(), SRC.end(), lower(x)) != SRC.end()) return true;
          return false;
        };
        return !is_edge(a) && is_edge(c);
      });
      for (auto& t : sorted) {
        Rows rows = {cols[t]};
        for (auto& r : query("SELECT * FROM \"" + t + "\" LIMIT " + std::to_string(MAX_NODES * 2))) {
          for (auto& c : r)
            if (c == "\x01NULL") c = "";
          rows.push_back(r);
        }
        try {
          kinds.push_back(t + ":" + rows_to_graph(b, rows));
        } catch (const FormatError&) {
        }
      }
      if (kinds.empty()) throw FormatError("ninguna tabla de la base SQLite tiene forma de grafo");
      g = b.result("sqlite");
      g["table"] = join(kinds, ", ");
    }
  } catch (...) {
    sqlite3_close(db);
    throw;
  }
  sqlite3_close(db);
  return g;
}

// ------------------------------------------------------------------ XML
using xml::Element;

json parse_graphml(const Element& root) {
  std::map<std::string, std::string> keys;
  for (auto* k : root.iter())
    if (xml::strip_ns(k->tag) == "key") {
      std::string id = k->get("id", "");
      std::string nm = k->get("attr.name", "");
      keys[id] = lower(nm.empty() ? id : nm);
    }
  Builder b;
  auto data = [&](const Element& el) {
    json d = json::object();
    for (auto& c : el.children)
      if (xml::strip_ns(c->tag) == "data") {
        std::string k = c->get("key", "");
        auto it = keys.find(k);
        d[it == keys.end() ? k : it->second] = trim(c->itertext());
      }
    return d;
  };
  auto yed_label = [&](const Element& el, const char* tag) -> json {
    for (auto* x : el.iter())
      if (xml::strip_ns(x->tag) == tag) return trim(x->itertext());
    return nullptr;
  };
  static const Rx DN(R"(^d\d+$)");
  for (auto* el : root.iter()) {
    std::string t = xml::strip_ns(el->tag);
    if (t == "node") {
      json d = data(*el);
      json name = first(d, {"label", "name", "title", "nombre"});
      if (!truthy(name)) name = yed_label(*el, "NodeLabel");
      json props = json::object();
      static const std::set<std::string> EX = {"label", "name", "title", "nombre", "type",
                                               "category", "description", "note", "scope"};
      for (auto& [k, v] : d.items())
        if (!EX.count(k) && truthy(v) && !DN.test(k)) props[k] = v;
      json id = el->get("id") ? json(*el->get("id")) : json(nullptr);
      b.entity(id, props, name,
               obj({{"type", first(d, {"type", "labels"})}, {"category", get(d, "category")},
                    {"note", first(d, {"description", "note"})}}));
    } else if (t == "edge") {
      json d = data(*el);
      json rel = first(d, {"label", "relation", "rel", "type", "predicate"});
      if (!truthy(rel) && el->get("label") && !el->get("label")->empty()) rel = *el->get("label");
      if (!truthy(rel)) rel = yed_label(*el, "EdgeLabel");
      b.edge(el->get("source") ? json(*el->get("source")) : json(nullptr),
             el->get("target") ? json(*el->get("target")) : json(nullptr), rel, get(d, "weight", 1.0));
    }
  }
  return b.result("graphml");
}

json attr_or_null(const Element& el, const char* k) {
  auto* v = el.get(k);
  return v ? json(*v) : json(nullptr);
}

json parse_gexf(const Element& root) {
  Builder b;
  std::map<std::pair<std::string, std::string>, std::string> attrs;
  for (auto* a : root.iter())
    if (xml::strip_ns(a->tag) == "attributes")
      for (auto& x : a->children) {
        std::string title = x->get("title", "");
        attrs[{a->get("class", "node"), x->get("id", "")}] = title.empty() ? x->get("id", "") : title;
      }
  auto values = [&](const Element& el, const char* cls) {
    json p = json::object();
    for (auto* v : el.iter())
      if (xml::strip_ns(v->tag) == "attvalue") {
        std::string f = v->get("for", "");
        if (f.empty()) f = v->get("id", "");
        auto it = attrs.find({cls, f});
        std::string k = it == attrs.end() ? v->get("for", "None") : it->second;
        p[k] = attr_or_null(*v, "value");
      }
    return p;
  };
  for (auto* el : root.iter()) {
    std::string t = xml::strip_ns(el->tag);
    if (t == "node") {
      json props = values(*el, "node");
      json type = nullptr;
      if (props.contains("type")) {
        type = props["type"];
        props.erase("type");
      }
      b.entity(attr_or_null(*el, "id"), props, attr_or_null(*el, "label"), obj({{"type", type}}));
    } else if (t == "edge") {
      json props = values(*el, "edge");
      json rel = attr_or_null(*el, "label");
      if (!truthy(rel)) rel = get(props, "label");
      if (!truthy(rel)) rel = attr_or_null(*el, "kind");
      json w = el->get("weight") ? json(*el->get("weight")) : json(1.0);
      b.edge(attr_or_null(*el, "source"), attr_or_null(*el, "target"), rel, w);
    }
  }
  return b.result("gexf");
}

void rdf_triple(Builder& b, const std::string& s, const std::string& p, const std::string& o, bool literal) {
  std::string lp = local(p);
  if (p == "type" || p == RDF_NS + "type" || (lp == "type" && !literal)) {
    std::string t = uleft(lower(label(o)), 30);
    b.node(s, nullptr, obj({{"type", t.empty() ? "concepto" : t}}));
  } else if (literal && (lp == "label" || lp == "name" || lp == "preflabel" || lp == "title" || lp == "altlabel" ||
                         lp == "nombre")) {
    b.node(s, o);
  } else if (literal && (lp == "comment" || lp == "description" || lp == "definition" || lp == "abstract" ||
                         lp == "note" || lp == "scopenote")) {
    b.node(s, nullptr, obj({{"note", o}}));
  } else if (literal) {
    std::string l = label(p);
    b.entity(s, obj({{(l.empty() ? lp : l).c_str(), o}}));
  } else {
    b.edge(s, o, p);
  }
}

json parse_rdfxml(const Element& root) {
  Builder b;
  int blank = 0, triples = 0;
  const std::string RES = "{" + RDF_NS + "}resource", ABOUT = "{" + RDF_NS + "}about", ID = "{" + RDF_NS + "}ID",
                    NID = "{" + RDF_NS + "}nodeID", PT = "{" + RDF_NS + "}parseType";
  auto tag_iri = [](const std::string& tag) { return starts_with(tag, "{") ? replace_all(tag.substr(1), "}", "") : tag; };
  std::function<std::string(const Element&, std::string)> subject = [&](const Element& el, std::string sid) {
    if (sid.empty()) {
      if (el.get(ABOUT)) sid = *el.get(ABOUT);
      else if (el.get(ID)) sid = "#" + *el.get(ID);
      else if (el.get(NID)) sid = "_:" + *el.get(NID);
      else sid = "_:b" + std::to_string(++blank);
      b.node(sid);
      if (tag_iri(el.tag) != RDF_NS + "Description")
        b.node(sid, nullptr, obj({{"type", uleft(lower(label(tag_iri(el.tag))), 30)}}));
    }
    for (auto& [k, v] : el.attrib)
      if (!starts_with(k, "{" + RDF_NS + "}") && !starts_with(k, "{http://www.w3.org/XML/1998/namespace}")) {
        rdf_triple(b, sid, tag_iri(k), v, true);
        ++triples;
      }
    for (auto& prop : el.children) {
      std::string p = tag_iri(prop->tag);
      std::string res = prop->get(RES) ? *prop->get(RES) : prop->get(NID) ? "_:" + *prop->get(NID) : "";
      if (!res.empty()) {
        rdf_triple(b, sid, p, res, false);
      } else if (prop->get(PT) && *prop->get(PT) == "Resource") {
        std::string child = "_:b" + std::to_string(++blank);
        b.node(child);
        subject(*prop, child);
        rdf_triple(b, sid, p, child, false);
      } else if (prop->size()) {
        for (auto& c : prop->children) rdf_triple(b, sid, p, subject(*c, ""), false);
      } else if (!trim(prop->text).empty()) {
        rdf_triple(b, sid, p, trim(prop->text), true);
      } else {
        continue;
      }
      ++triples;
    }
    return sid;
  };
  for (auto& el : root.children) subject(*el, "");
  if (!triples && b.size() < 2) throw FormatError("no se han encontrado tripletas RDF/XML");
  collapse_blank(b);
  return b.result("rdf-xml");
}

const std::set<std::string> NODE_TAGS = {"node", "vertex", "nodo", "entity", "entidad", "concept", "concepto", "topic",
                                         "outline", "item"};
const std::set<std::string> EDGE_TAGS = {"edge",     "link",     "arc",         "relationship", "relation",
                                         "arista",   "association", "enlace",   "arrowlink"};

json parse_xml_generic(const Element& root) {
  Builder b;
  int nodes = 0, links = 0;
  std::string root_tag = lower(xml::strip_ns(root.tag));
  auto low = [](const Element& el) {
    json o = json::object();
    for (auto& [k, v] : el.attrib) o[lower(xml::strip_ns(k))] = v;
    return o;
  };
  auto child_props = [&](const Element& el) {
    json out = json::object();
    for (auto& c : el.children) {
      std::string t = lower(xml::strip_ns(c->tag));
      if (t == "att" || t == "attr" || t == "attribute" || t == "property" || t == "data" || t == "attvalue") {
        json a = low(*c);
        json k = truthy(get(a, "name")) ? a["name"] : truthy(get(a, "key")) ? a["key"] : get(a, "for");
        json v = a.contains("value") ? a["value"] : json(trim(c->itertext()));
        if (truthy(k) && truthy(v)) out[k.get<std::string>()] = v;
      } else if ((t == "label" || t == "name" || t == "text" || t == "title" || t == "nombre" || t == "caption") &&
                 !c->size() && !trim(c->text).empty()) {
        if (!out.contains(t)) out[t] = trim(c->text);
      }
    }
    return out;
  };
  std::function<void(const Element&, const std::string&)> walk = [&](const Element& el, const std::string& parent) {
    std::string t = lower(xml::strip_ns(el.tag));
    json a = low(el);
    if (NODE_TAGS.count(t) && !(t == "item" && (root_tag == "rss" || root_tag == "channel"))) {
      json props = child_props(el);
      json name = first(a, {"label", "name", "text", "title", "nombre", "caption"});
      if (!truthy(name)) name = first(props, {"label", "name", "text", "title", "nombre", "caption"});
      ++nodes;
      json nid = first(a, {"id", "key", "uid", "name"});
      if (!truthy(nid)) nid = truthy(name) ? json(parent + "/" + text(name)) : json("_n" + std::to_string(nodes));
      json rest = json::object();
      json both = a;
      for (auto& [k, v] : props.items()) both[k] = v;
      static const std::set<std::string> EX = {"id",   "key",   "uid",    "name",    "label",   "text",
                                               "title", "nombre", "caption", "type", "category"};
      for (auto& [k, v] : both.items())
        if (!EX.count(k) && ulen(pystr(v)) < 300) rest[k] = v;
      json type = truthy(get(a, "type")) ? a["type"] : truthy(get(a, "category")) ? a["category"] : get(props, "type");
      std::string id = b.entity(nid, rest, name, obj({{"type", type}}));
      if (!parent.empty()) {
        b.edge(parent, id.empty() ? text(nid) : id, "contiene");
        ++links;
      }
      for (auto& c : el.children) walk(*c, text(nid));
      return;
    }
    if (EDGE_TAGS.count(t)) {
      json s = first(a, {"source", "from", "src", "start", "origin", "origen"});
      if (!truthy(s) && !parent.empty()) s = parent;
      json d = first(a, {"target", "to", "dst", "end", "destination", "destino"});
      if (truthy(s) && truthy(d)) {
        json props = child_props(el);
        json rel = first(a, {"label", "type", "category", "relation", "name", "kind", "rel"});
        if (!truthy(rel)) rel = first(props, {"label", "interaction", "type"});
        json w = truthy(get(a, "weight")) ? a["weight"] : truthy(get(props, "weight")) ? props["weight"] : json(1);
        b.edge(s, d, rel, w);
        ++links;
      }
      return;
    }
    for (auto& c : el.children) walk(*c, parent);
  };
  walk(root, "");
  if (!b.size()) throw FormatError("el XML no tiene elementos de nodo o enlace reconocibles");
  static const std::map<std::string, std::string> FMT = {{"graph", "xgmml"}, {"directedgraph", "dgml"}, {"gxl", "gxl"},
                                                         {"map", "mapa-mental"}, {"opml", "opml"},
                                                         {"xmap-content", "xmind"}};
  auto it = FMT.find(root_tag);
  bool known = it != FMT.end();
  return b.result(known ? it->second : "xml", !known || !links);
}

json parse_xml(const std::string& t) {
  std::unique_ptr<Element> root;
  try {
    root = xml::parse(t);
  } catch (const xml::ParseError& e) {
    throw FormatError(std::string("XML mal formado: ") + e.what());
  }
  std::string tag = lower(xml::strip_ns(root->tag));
  if (tag == "graphml") return parse_graphml(*root);
  if (tag == "gexf") return parse_gexf(*root);
  if (tag == "rdf" || root->tag == "{" + RDF_NS + "}RDF") return parse_rdfxml(*root);
  if (tag == "html") throw FormatError("es una pagina HTML, no un grafo: importala en la Biblioteca");
  return parse_xml_generic(*root);
}


// ------------------------------------------------------------------ utilidades de texto
std::string unq(const std::string& s0) {
  std::string s = trim(s0);
  if (s.size() >= 2 && s[0] == s.back() && (s[0] == '"' || s[0] == '\'')) return s.substr(1, s.size() - 2);
  return s;
}

std::vector<std::string> tsplit(const std::string& line) {
  static const Rx TOK(R"rx("[^"]*"|'[^']*'|\S+)rx");
  std::vector<std::string> out;
  for (auto& m : TOK.finditer(line)) out.push_back(unq(m.str()));
  return out;
}

std::vector<std::string> split_ws_max(const std::string& s, int maxsplit) {   // str.split(None, maxsplit)
  std::vector<std::string> out;
  size_t i = 0, n = s.size();
  auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
  while (i < n) {
    while (i < n && sp(s[i])) ++i;
    if (i >= n) break;
    if ((int)out.size() == maxsplit) {
      out.push_back(rtrim(s.substr(i)));
      break;
    }
    size_t st = i;
    while (i < n && !sp(s[i])) ++i;
    out.push_back(s.substr(st, i - st));
  }
  return out;
}

std::pair<std::string, std::vector<std::string>> protect(const std::string& t, const Rx& pat) {
  std::vector<std::string> found;
  std::string out = pat.sub(t, [&](const Rx::Match& m) {
    found.push_back(m.str());
    return std::string("\x01") + std::to_string(found.size() - 1) + "\x01";
  });
  return {out, found};
}

std::optional<double> num_opt(const std::string& s) { return numv(s); }

// ------------------------------------------------------------------ Graphviz DOT
const char* DOT_ID = R"((?:\x01\d+\x01|<[^<>]*>|-?[\w.]+))";

json parse_dot(const std::string& t0) {
  static const Rx STR(R"rx("(?:[^"\\]|\\.)*")rx");
  auto [t1, strings] = protect(t0, STR);
  static const Rx COMMENTS(R"(//[^\n]*|/\*.*?\*/|^\s*#[^\n]*)", Rx::S | Rx::M);
  std::string t = COMMENTS.sub(t1, "");
  Builder b;
  static const Rx MARK("^\\x01(\\d+)\\x01$"), TAGS(R"(<[^>]+>)");
  auto unqd = [&](const std::string& s) -> std::string {
    std::string st = trim(s);
    auto m = MARK.match(st);
    if (m) return replace_all(replace_all(strings[std::stoul(m->str(1))].substr(1, strings[std::stoul(m->str(1))].size() - 2), "\\\"", "\""), "\\n", " ");
    if (starts_with(s, "<") && ends_with(s, ">")) return trim(TAGS.sub(s.substr(1, s.size() - 2), " "));
    return s;
  };
  auto attr = [&](const std::string& attrs, const char* key) -> json {
    Rx r(std::string("\\b") + key + "\\s*=\\s*(" + DOT_ID + ")");
    auto m = r.search(attrs);
    return m ? json(unqd(m->str(1))) : json(nullptr);
  };
  static const Rx NATTR(std::string("(") + DOT_ID + R"()\s*\[([^\]]*)\])"), TAILARROW(R"(-[->]\s*$)");
  for (auto& m : NATTR.finditer(t)) {
    std::string id = m.str(1);
    if (id == "graph" || id == "node" || id == "edge") continue;
    std::string before = rtrim(t.substr(0, m.start()));
    std::string last3 = before.size() > 3 ? before.substr(before.size() - 3) : before;
    if (TAILARROW.test(last3)) continue;
    b.node(unqd(id), attr(m.str(2), "label"), obj({{"note", attr(m.str(2), "tooltip")}}));
  }
  int found = 0;
  static const Rx EDGES(std::string("(") + DOT_ID + R"((?:\s*-[->]\s*)" + DOT_ID + R"()+)\s*(\[([^\]]*)\])?)"),
      ARR(R"(\s*-[->]\s*)");
  for (auto& m : EDGES.finditer(t)) {
    std::vector<std::string> ids;
    for (auto& x : ARR.split(m.str(1))) ids.push_back(unqd(x));
    std::string a = m.str(3);
    for (size_t k = 0; k + 1 < ids.size(); ++k) {
      json lab = attr(a, "label");
      if (!truthy(lab)) lab = attr(a, "xlabel");
      json w = attr(a, "weight");
      b.edge(ids[k], ids[k + 1], lab, truthy(w) ? w : json(1));
      ++found;
    }
  }
  if (!found) throw FormatError("no hay aristas a -> b en el fichero DOT");
  return b.result("dot");
}

// ------------------------------------------------------------------ GML
json parse_gml(const std::string& t0) {
  static const Rx COM(R"((?m)^\s*#.*$)"), TOK(R"rx("(?:[^"\\]|\\.)*"|\[|\]|[^\s\[\]"]+)rx");
  std::string t = COM.sub(t0, "");
  std::vector<std::string> toks;
  for (auto& m : TOK.finditer(t)) toks.push_back(m.str());
  // bloque -> lista de (clave, valor); valor es texto o una lista anidada (json array de pares)
  std::function<json(size_t&)> block = [&](size_t& i) -> json {
    json items = json::array();
    while (i < toks.size()) {
      const std::string& tk = toks[i];
      if (tk == "]") {
        ++i;
        return items;
      }
      if (i + 1 >= toks.size()) break;
      const std::string& v = toks[i + 1];
      json val;
      if (v == "[") {
        i += 2;
        val = block(i);
      } else {
        val = starts_with(v, "\"") && v.size() >= 2 ? v.substr(1, v.size() - 2) : v;
        i += 2;
      }
      items.push_back(json::array({lower(tk), val}));
    }
    return items;
  };
  size_t i = 0;
  json top = block(i);
  Builder b;
  bool any = false;
  for (auto& kv : top) {
    if (kv[0] != "graph" || !kv[1].is_array()) continue;
    any = true;
    for (auto& item : kv[1]) {
      std::string k = item[0];
      const json& v = item[1];
      if (!v.is_array()) continue;
      json d = json::object();
      for (auto& p : v)
        if (!d.contains(p[0].get<std::string>())) d[p[0].get<std::string>()] = p[1];
      if (k == "node") {
        json props = json::object();
        for (auto& [kk, vv] : d.items())
          if (!vv.is_array() && kk != "id" && kk != "label") props[kk] = vv;
        json name = truthy(get(d, "label")) ? d["label"] : get(d, "name");
        b.entity(get(d, "id"), props, name, obj({{"type", get(d, "type")}}));
      } else if (k == "edge") {
        b.edge(get(d, "source"), get(d, "target"), first(d, {"label", "relation", "type", "name"}),
               first(d, {"weight", "value"}, 1));
      }
    }
  }
  if (!any) throw FormatError("no hay bloque graph [ ... ] en el GML");
  return b.result("gml");
}

// ------------------------------------------------------------------ formatos de analisis de redes
json parse_pajek(const std::string& t) {
  Builder b;
  std::string section;
  for (auto& raw : splitlines(t)) {
    std::string line = trim(raw);
    if (line.empty() || starts_with(line, "%")) continue;
    if (starts_with(line, "*")) {
      section = lower(split_ws(line)[0]);
      continue;
    }
    auto parts = tsplit(line);
    if (section == "*vertices" && !parts.empty()) {
      b.node(parts[0], parts.size() > 1 ? parts[1] : parts[0]);
    } else if ((section == "*arcs" || section == "*edges") && parts.size() >= 2) {
      json rel = parts.size() > 3 && !num_opt(parts[3]) ? json(parts[3]) : json(nullptr);
      json w = parts.size() > 2 && num_opt(parts[2]) ? json(parts[2]) : json(1);
      b.edge(parts[0], parts[1], rel, w);
    } else if ((section == "*arcslist" || section == "*edgeslist") && parts.size() >= 2) {
      for (size_t k = 1; k < parts.size(); ++k) b.edge(parts[0], parts[k]);
    }
  }
  if (!b.size()) throw FormatError("no hay *Vertices ni aristas Pajek");
  return b.result("pajek");
}

json parse_tgf(const std::string& t) {
  Builder b;
  bool edges = false;
  for (auto& line : splitlines(t)) {
    if (trim(line) == "#") {
      edges = true;
      continue;
    }
    auto parts = split_ws_max(trim(line), edges ? 2 : 1);
    if (parts.empty()) continue;
    if (!edges) {
      b.node(parts[0], parts.size() > 1 ? parts[1] : parts[0]);
    } else if (parts.size() >= 2) {
      b.edge(parts[0], parts[1], parts.size() > 2 ? json(parts[2]) : json(nullptr));
    }
  }
  return b.result("tgf");
}

json parse_gdf(const std::string& t) {
  Builder b;
  std::string mode;
  std::vector<std::string> cols;
  char q = std::count(t.begin(), t.end(), '\'') > std::count(t.begin(), t.end(), '"') ? '\'' : '"';
  static const Rx DEF(R"(^\s*(nodedef|edgedef)\s*>\s*(.*)$)", Rx::I);
  for (auto& line : splitlines(t)) {
    if (trim(line).empty()) continue;
    auto m = DEF.match(line);
    if (m) {
      mode = lower(m->str(1));
      cols.clear();
      for (auto& c : split(m->str(2), ",")) cols.push_back(trim(c).empty() ? "" : lower(split_ws(trim(c))[0]));
      continue;
    }
    auto rows = csv::read(line, ',', q, true);
    auto r = rows.empty() ? std::vector<std::string>{} : rows[0];
    if (mode == "nodedef" && !r.empty()) {
      json p = json::object();
      for (size_t i = 1; i < std::min(r.size(), cols.size()); ++i)
        if (!r[i].empty() && cols[i] != "label") p[cols[i]] = r[i];
      auto li = std::find(cols.begin(), cols.end(), "label");
      long l = li == cols.end() ? -1 : (long)(li - cols.begin());
      b.entity(r[0], p, l >= 0 && l < (long)r.size() && !r[l].empty() ? json(r[l]) : json(nullptr));
    } else if (mode == "edgedef" && r.size() >= 2) {
      json d = json::object();
      for (size_t i = 0; i < std::min(r.size(), cols.size()); ++i) d[cols[i]] = r[i];
      b.edge(r[0], r[1], first(d, {"label", "relation", "type"}), first(d, {"weight"}, 1));
    }
  }
  return b.result("gdf");
}

json parse_ucinet(const std::string& t) {
  std::string low = lower(t);
  std::string head = split(low, "data:", 1)[0];
  static const Rx NR(R"(\bn\s*=\s*(\d+))"), FMT(R"(format\s*=\s*(\w+))"),
      LAB(R"((?:^|\n)\s*(?:row\s+)?labels\s*:\s*\n(.*?)(?=\n\s*(?:col\s+labels|data)\s*:))", Rx::I | Rx::S),
      LSEP(R"([,\n]+)"), DATA(R"((?im)^\s*data\s*:\s*$)");
  auto nm = NR.search(head);
  size_t n = nm ? std::stoul(nm->str(1)) : 0;
  auto fm = FMT.search(head);
  std::string fmt = fm ? fm->str(1) : "fullmatrix";
  std::vector<std::string> labels;
  auto lm = LAB.search(t);
  if (lm)
    for (auto& x : LSEP.split(lm->str(1)))
      if (!trim(x).empty()) labels.push_back(unq(x));
  auto body = DATA.split(t, 1);
  std::vector<std::string> lines;
  for (auto& ln : splitlines(body.size() > 1 ? body[1] : ""))
    if (!trim(ln).empty()) lines.push_back(ln);
  Builder b;
  auto lab = [&](size_t i) { return i < labels.size() ? labels[i] : std::to_string(i + 1); };
  if (starts_with(fmt, "edgelist")) {
    for (auto& ln : lines) {
      auto p = tsplit(replace_all(ln, ",", " "));
      if (p.size() >= 2) b.edge(p[0], p[1], nullptr, p.size() > 2 ? json(p[2]) : json(1));
    }
  } else if (starts_with(fmt, "nodelist")) {
    for (auto& ln : lines) {
      auto p = tsplit(replace_all(ln, ",", " "));
      for (size_t k = 1; k < p.size(); ++k) b.edge(p[0], p[k]);
    }
  } else {
    std::vector<std::vector<std::string>> rows;
    for (auto& ln : lines) rows.push_back(tsplit(replace_all(ln, ",", " ")));
    if (contains(head, "labels embedded") && !rows.empty()) {
      labels = rows[0];
      std::vector<std::vector<std::string>> rest;
      for (size_t k = 1; k < rows.size(); ++k) rest.emplace_back(rows[k].begin() + std::min<size_t>(1, rows[k].size()), rows[k].end());
      rows = rest;
    }
    size_t lim = n ? std::min(n, rows.size()) : rows.size();
    for (size_t i = 0; i < lim; ++i) {
      b.node(lab(i), lab(i));
      for (size_t j = 0; j < rows[i].size(); ++j) {
        auto w = num_opt(rows[i][j]);
        if (w && *w) b.edge(lab(i), lab(j), nullptr, *w);
      }
    }
  }
  return b.result("ucinet-dl");
}

json parse_sif(const std::string& t) {
  Builder b;
  for (auto& line : splitlines(t)) {
    std::vector<std::string> p;
    for (auto& x : contains(line, "\t") ? split(line, "\t") : split_ws(line))
      if (!trim(x).empty()) p.push_back(trim(x));
    if (p.size() >= 3) {
      for (size_t k = 2; k < p.size(); ++k) b.edge(p[0], p[k], p[1]);
    } else if (p.size() == 1) {
      b.node(p[0], p[0]);
    }
  }
  return b.result("sif");
}

json parse_lgl(const std::string& t) {
  Builder b;
  std::string cur;
  for (auto& line0 : splitlines(t)) {
    std::string line = trim(line0);
    if (starts_with(line, "#")) {
      cur = trim(line.substr(1));
      b.node(cur, cur);
    } else if (!line.empty() && !cur.empty()) {
      auto p = split_ws(line);
      b.edge(cur, p[0], nullptr, p.size() > 1 ? json(p[1]) : json(1));
    }
  }
  return b.result("lgl");
}

json parse_edgelist(const std::string& t, bool adjlist = false) {
  Builder b;
  static const Rx ATTRS(R"rx(['"](\w+)['"]\s*:\s*['"]?([^,'"}]+))rx");
  for (auto& raw : splitlines(t)) {
    std::string line = starts_with(ltrim(raw), "#") ? "" : trim(split(raw, "#", 1)[0]);
    if (line.empty()) continue;
    auto p = tsplit(contains(line, ",") && !contains(line, " ") ? replace_all(line, ",", " ") : line);
    if (p.empty()) continue;
    if (adjlist) {
      b.node(p[0], p[0]);
      for (size_t k = 1; k < p.size(); ++k) b.edge(p[0], p[k]);
    } else if (p.size() == 2) {
      b.edge(p[0], p[1]);
    } else if (p.size() >= 3) {
      auto w = num_opt(p[2]);
      if (w && p.size() == 3) {
        b.edge(p[0], p[1], nullptr, *w);
      } else if (starts_with(p[2], "{")) {
        json d = json::object();
        for (auto& m : ATTRS.finditer(line)) d[m.str(1)] = m.str(2);
        b.edge(p[0], p[1], first(d, {"label", "relation", "type"}), get(d, "weight", 1));
      } else {
        b.edge(p[0], p.size() == 3 ? p[2] : p[1], p.size() == 3 ? p[1] : p[2]);
      }
    } else if (p.size() == 1) {
      b.node(p[0], p[0]);
    }
  }
  return b.result(adjlist ? "adjlist" : "lista-de-aristas");
}

void collapse_blank(Builder& b) {
  for (int round = 0; round < 8; ++round) {
    std::set<std::string> blank;
    for (auto& [k, n] : b.nodes)
      if (starts_with(k, "_:") && n["name"].get<std::string>() == label(k)) blank.insert(k);
    std::map<std::string, std::vector<json>> inc;
    for (auto& e : b.edges)
      if (blank.count(e["dst"].get<std::string>())) inc[e["dst"].get<std::string>()].push_back(e);
    std::set<std::string> ready;
    for (auto& k : blank) {
      auto it = inc.find(k);
      if (it == inc.end() || it->second.empty()) continue;
      bool ok = true;
      for (auto& e : it->second)
        if (blank.count(e["src"].get<std::string>())) ok = false;
      if (ok) ready.insert(k);
    }
    if (ready.empty()) break;
    std::vector<json> keep;
    std::map<std::string, std::vector<json>> outs;
    for (auto& e : b.edges) {
      std::string s = e["src"], d = e["dst"];
      if (ready.count(s)) outs[s].push_back(e);
      else if (!ready.count(d)) keep.push_back(e);
    }
    // el orden de un set de Python no es estable; aqui se recorren en el orden de aparicion de los nodos
    std::vector<std::string> ready_ord;
    for (auto& id : b.order)
      if (ready.count(id)) ready_ord.push_back(id);
    for (auto& k : ready_ord) {
      for (auto& ei : inc[k]) {
        for (auto& eo : outs[k])
          keep.push_back({{"src", ei["src"]},
                          {"dst", eo["dst"]},
                          {"rel", uleft(ei["rel"].get<std::string>() + "_" + eo["rel"].get<std::string>(), 30)},
                          {"weight", ei["weight"]}});
        std::string note = b.at(k)["note"];
        if (!note.empty())
          b.node(ei["src"], nullptr, obj({{"note", replace_all(ei["rel"].get<std::string>(), "_", " ") + ": " + note}}));
      }
      b.erase(k);
    }
    b.edges = keep;
  }
}

// ------------------------------------------------------------------ RDF de texto (N-Triples, N-Quads, Turtle, TriG, N3)
std::string rdf_unescape(const std::string& s) {
  static const Rx ESC(R"(\\(u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|.))");
  return ESC.sub(s, [](const Rx::Match& m) -> std::string {
    std::string g = m.str(1);
    if ((g[0] == 'u' || g[0] == 'U') && g.size() > 1) return u8((char32_t)std::stoul(g.substr(1), nullptr, 16));
    if (g == "n") return "\n";
    if (g == "t") return "\t";
    if (g == "r" || g == "b" || g == "f") return "";
    return g;
  });
}

json parse_rdf(const std::string& t0) {
  static const Rx COM(R"((?m)^\s*#.*$)");
  static const Rx TTL(
      R"rx("""(?:.|\n)*?"""(?:@[\w-]+|\^\^\S+)?|'''(?:.|\n)*?'''(?:@[\w-]+|\^\^\S+)?|)rx"
      R"rx("(?:[^"\\]|\\.)*"(?:@[\w-]+|\^\^\S+)?|'(?:[^'\\]|\\.)*'(?:@[\w-]+|\^\^\S+)?|<[^>\s]*>|)rx"
      R"rx(@prefix|@base|PREFIX|BASE|GRAPH|[+-]?\d+\.\d+(?:[eE][+-]?\d+)?|[\[\](){};,.]|_:[\w.-]*\w|)rx"
      R"rx([\w-]*:[\w.\-%]*[\w\-%]|[\w-]*:|\ba\b|[^\s;,.\[\](){}]+)rx");
  std::string t = COM.sub(t0, "");
  std::vector<std::string> toks;
  for (auto& m : TTL.finditer(t)) toks.push_back(m.str());
  std::map<std::string, std::string> prefixes;
  Builder b;
  int triples = 0, bnodes = 0;
  std::vector<std::pair<std::optional<std::string>, std::optional<std::string>>> stack;
  auto iri = [&](const std::string& x) -> std::string {
    if (starts_with(x, "<")) return x.substr(1, x.size() - 2);
    if (x == "a") return "type";
    if (contains(x, ":") && !starts_with(x, "'") && !starts_with(x, "\"") && !starts_with(x, "_:")) {
      auto c = x.find(':');
      std::string p = x.substr(0, c), l = x.substr(c + 1);
      auto it = prefixes.find(p);
      if (it != prefixes.end()) return it->second + l;
      return l.empty() ? p : l;
    }
    return x;
  };
  static const Rx LIT(R"rx(^("""|'''|"|')(.*?)\1(?:@[\w-]+|\^\^\S+)?$)rx", Rx::S), NUMLIT(R"(^(?:[+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false)$)");
  auto lit = [&](const std::string& x) {
    auto m = LIT.match(x);
    return m ? rdf_unescape(m->str(2)) : x;
  };
  std::optional<std::string> subj, pred;
  bool last_obj = false;
  for (size_t i = 0; i < toks.size();) {
    const std::string& tk = toks[i];
    if (tk == "@prefix" || tk == "PREFIX") {
      if (i + 2 < toks.size()) prefixes[rtrim(toks[i + 1], ":")] = trim(toks[i + 2], "<>");
      i += (tk == "@prefix" && i + 3 < toks.size() && toks[i + 3] == ".") ? 4 : 3;
      continue;
    }
    if (tk == "@base" || tk == "BASE") {
      i += tk == "@base" ? 3 : 2;
      continue;
    }
    if (tk == "GRAPH") {
      i += 2;
      continue;
    }
    if (tk == "{" || tk == "}" || tk == "(" || tk == ")") {
      if (tk == "{" && subj && !pred && stack.empty()) subj.reset();
      ++i;
      continue;
    }
    if (tk == ".") {
      subj.reset();
      pred.reset();
      last_obj = false;
    } else if (tk == ";") {
      pred.reset();
      last_obj = false;
    } else if (tk == ",") {
      last_obj = false;
    } else if (tk == "[") {
      std::string nb = "_:b" + std::to_string(++bnodes);
      b.node(nb);
      if (subj && pred) {
        rdf_triple(b, *subj, *pred, nb, false);
        ++triples;
      }
      stack.push_back({subj, pred});
      subj = nb;
      pred.reset();
      last_obj = false;
    } else if (tk == "]") {
      auto nb = subj;
      if (!stack.empty()) {
        subj = stack.back().first;
        pred = stack.back().second;
        stack.pop_back();
      } else {
        subj.reset();
        pred.reset();
      }
      if (!subj) subj = nb;
      last_obj = true;
    } else if (!subj) {
      subj = iri(tk);
    } else if (!pred) {
      pred = iri(tk);
    } else if (last_obj) {
      // N-Quads: el cuarto termino es el grafo
    } else {
      bool is_lit = starts_with(tk, "\"") || starts_with(tk, "'") || NUMLIT.test(tk);
      rdf_triple(b, *subj, *pred, is_lit ? lit(tk) : iri(tk), is_lit);
      ++triples;
      last_obj = true;
    }
    ++i;
  }
  if (!triples) throw FormatError("no se han encontrado tripletas RDF");
  collapse_blank(b);
  return b.result("rdf");
}

// ------------------------------------------------------------------ Cypher (Neo4j)
json cy_map(const std::string& s, const std::vector<std::string>& strings) {
  json out = json::object();
  if (s.size() < 2) return out;
  static const Rx KV("(`[^`]+`|\\w+)\\s*:\\s*(\\x01\\d+\\x01|\\[[^\\]]*\\]|[^,}]+)"), MARK("\\x01(\\d+)\\x01");
  std::string inner = s.substr(1, s.size() - 2);
  for (auto& m : KV.finditer(inner)) {
    std::string k = trim(m.str(1), "`"), v = trim(m.str(2));
    v = MARK.sub(v, [&](const Rx::Match& x) {
      const std::string& q = strings[std::stoul(x.str(1))];
      return q.substr(1, q.size() - 2);
    });
    if (starts_with(v, "[")) {
      std::vector<std::string> parts;
      for (auto& x : split(v.substr(1, v.size() - 2), ","))
        if (!trim(x).empty()) parts.push_back(unq(x));
      v = join(parts, ", ");
    }
    out[k] = unq(v);
  }
  return out;
}

json parse_cypher(const std::string& t0) {
  static const Rx STR(R"rx('(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")rx"), COM(R"(//[^\n]*)");
  auto [t1, strings] = protect(t0, STR);
  std::string t = COM.sub(t1, "");
  Builder b;
  std::map<std::string, std::string> var, by_key;
  static const Rx CLAUSE(R"(\b(OPTIONAL\s+MATCH|MATCH|CREATE|MERGE|WITH|RETURN|WHERE|SET|DETACH\s+DELETE|DELETE|)"
                         R"(REMOVE|UNWIND|CALL|FOREACH|ON\s+CREATE|ON\s+MATCH|ORDER\s+BY|LIMIT)\b)",
                         Rx::I),
      STMT(R"(;\s*(?:\n|$)|^\s*:(?:begin|commit)\s*$)", Rx::M),
      NODE(R"(\(\s*(`[^`]+`|\w+)?\s*((?::\s*(?:`[^`]+`|\w+)\s*)*)(\{[^{}]*\})?\s*\))"),
      REL(R"(^\s*(<)?-\s*(?:\[\s*(`[^`]+`|\w+)?\s*(?::\s*(`[^`]+`|\w+)(?:\s*\|\s*:?\s*(?:`[^`]+`|\w+))*)?)"
          R"(\s*(?:\*[\d.]*)?\s*(\{[^{}]*\})?\s*\])?\s*-\s*(>)?\s*$)");
  for (auto& stmt : STMT.split(t)) {
    std::map<std::string, std::string> loc;
    auto resolve = [&](const Rx::Match& m, bool create) -> std::string {
      std::string v = trim(m.str(1), "`");
      std::vector<std::string> labels;
      for (auto& x : split(m.str(2), ":")) {
        std::string l = trim(x, " `");
        if (!l.empty()) labels.push_back(l);
      }
      json props = cy_map(m.str(3), strings);
      json uid = nullptr;
      if (props.contains("UNIQUE IMPORT ID")) {
        uid = props["UNIQUE IMPORT ID"];
        props.erase("UNIQUE IMPORT ID");
      }
      bool f;
      auto name = name_from(props, &f).first;
      std::string key = !uid.is_null() ? "uid:" + text(uid) : f ? (labels.empty() ? "" : labels[0]) + ":" + name : "";
      std::string nid;
      if (!key.empty() && by_key.count(key)) nid = by_key[key];
      if (nid.empty() && !v.empty()) nid = loc.count(v) ? loc[v] : var.count(v) ? var[v] : "";
      if (nid.empty()) {
        if (!(!key.empty() || (!v.empty() && create))) return "";
        nid = !key.empty() ? key : "var:" + v;
        if (!key.empty()) by_key[key] = nid;
      }
      if (create || !key.empty()) {
        if (!labels.empty() || !props.empty()) {
          json type = nullptr;
          for (auto& l : labels)
            if (l != "UNIQUE IMPORT LABEL") {
              type = l;
              break;
            }
          b.entity(nid, props, nullptr, obj({{"type", type}}));
        } else if (!b.has_node(nid)) {
          b.node(nid, v.empty() ? json(nullptr) : json(v));
        }
      }
      if (!v.empty()) loc[v] = var[v] = nid;
      return nid;
    };
    auto parts = CLAUSE.split(stmt);
    for (size_t k = 1; k + 1 < parts.size(); k += 2) {
      std::string kw = upper(parts[k]);
      const std::string& body = parts[k + 1];
      bool create = kw == "CREATE" || kw == "MERGE";
      if (!create && !ends_with(kw, "MATCH")) continue;
      auto nodes = NODE.finditer(body);
      std::vector<std::string> ids;
      for (auto& n : nodes) ids.push_back(resolve(n, create));
      if (!create) continue;
      for (size_t j = 0; j + 1 < nodes.size(); ++j) {
        std::string between = body.substr(nodes[j].end(), nodes[j + 1].start() - nodes[j].end());
        auto m = REL.match(between);
        if (m && !ids[j].empty() && !ids[j + 1].empty()) {
          std::string rel = m->has(3) ? trim(m->str(3), "`") : "relacionado_con";
          bool rev = m->has(1) && !m->has(5);
          b.edge(rev ? ids[j + 1] : ids[j], rev ? ids[j] : ids[j + 1], rel, get(cy_map(m->str(4), strings), "weight", 1));
        }
      }
    }
  }
  if (!b.size()) throw FormatError("no hay patrones (n)-[:REL]->(m) de Cypher");
  for (auto& id : b.order) {
    json& n = b.at(id);
    if (starts_with(id, "var:") && n["name"].get<std::string>() == label(id)) n["name"] = label(id.substr(4));
  }
  return b.result("cypher");
}

// ------------------------------------------------------------------ diagramas de texto (Mermaid, PlantUML, flechas)
const Rx& ARROW() {
  static const Rx R(R"(\s*(<-\[([^\]]*)\]-+|-+\[([^\]]*)\]->|<-+|<=+|<\.+-|-+>|=+>|-\.+->|\.+>|→|⇒|⟶|←|)"
                    R"((?<=\s)-{2,}(?=\s)|(?<=\s)={2,}(?=\s)|\s—\s|↔|<->)\s*)");
  return R;
}

std::string clean_end(const std::string& s0) {
  std::string s = trim(trim(trim(s0), ";"));
  static const Rx WRAP(R"(^\[(.*)\]$|^\((.*)\)$|^:(.*):$)"),
      KW(R"(^(?:class|interface|actor|participant|component|node|usecase|entity|database|object)\s+)", Rx::I);
  s = WRAP.sub(s, [](const Rx::Match& m) {
    for (size_t g = 1; g <= 3; ++g)
      if (m.has(g)) return m.str(g);
    return std::string();
  });
  s = KW.sub(s, "");
  return unq(trim(s));
}

int arrow_text(const std::vector<std::string>& lines, Builder& b, const json& labels = json::object()) {
  int made = 0;
  static const Rx COLON(R"(\S\s*:\s*\S)"), RELM(R"(^(.*?[^:\s])\s*:\s*([^:]+)$)");
  static const std::vector<std::pair<Rx, std::string>> NORM = [] {
    std::vector<std::pair<Rx, std::string>> v;
    v.emplace_back(Rx(R"(\s*(?:-{2,}|={2,}|-\.+-?)\s*>?\s*\|([^|]*)\|\s*)"), " -[\\1]-> ");
    v.emplace_back(Rx(R"(\s+(?:--|==|-\.)\s+([^->=.|][^>|]*?)\s+(?:-->|==>|\.->|---|===)\s*)"), " -[\\1]-> ");
    v.emplace_back(Rx(R"(\s*-{2,}\s*([^\s\-<>\[\]|][^<>\[\]|]*?)\s*-{2,}>\s*)"), " -[\\1]-> ");
    v.emplace_back(Rx(R"(\s*-+(?:up|down|left|right|u|d|l|r)-+>\s*)", Rx::I), " -> ");
    return v;
  }();
  for (auto line : lines) {
    std::string rel3;
    bool has_rel3 = false;
    auto br = split(line, "]");
    if (contains(line, " : ") || COLON.test(br.back())) {
      auto m = RELM.match(line);
      if (m) {
        std::string g1 = m->str(1);
        if (ARROW().test(g1)) {
          rel3 = trim(trim(m->str(2)), "\"");
          has_rel3 = true;
          line = g1;
        }
      }
    }
    for (auto& [rx, rep] : NORM) line = rx.sub(line, rep);
    auto parts = ARROW().split(line);
    if (parts.size() < 4) continue;
    std::vector<std::string> ends;
    for (size_t k = 0; k < parts.size(); k += 4) ends.push_back(parts[k]);
    for (size_t k = 0; k + 1 < ends.size(); ++k) {
      std::string op = parts[4 * k + 1], rev_lab = parts[4 * k + 2], fwd_lab = parts[4 * k + 3];
      std::string r = !rev_lab.empty() ? rev_lab : fwd_lab;
      json rel = !r.empty() ? json(r) : has_rel3 && !rel3.empty() ? json(rel3) : json(nullptr);
      bool reverse = starts_with(op, "<") && !ends_with(op, ">");
      for (auto& a : split(ends[k], "&"))
        for (auto& c : split(ends[k + 1], "&")) {
          std::string a_ = clean_end(a), c_ = clean_end(c);
          if (a_.empty() || c_.empty()) continue;
          for (auto& x : {a_, c_}) b.node(x, labels.contains(x) ? labels[x] : json(x));
          if (reverse) b.edge(c_, a_, rel);
          else b.edge(a_, c_, rel);
          ++made;
        }
    }
  }
  return made;
}

std::string expandtabs(const std::string& s, int n) {
  std::string out;
  size_t col = 0;
  for (char c : s) {
    if (c == '\t') {
      size_t sp = n - col % n;
      out.append(sp, ' ');
      col += sp;
    } else {
      out += c;
      col = (c == '\n' || c == '\r') ? 0 : ((unsigned char)c & 0xC0) == 0x80 ? col : col + 1;
    }
  }
  return out;
}

json outline(const std::string& t, Builder& b, const std::string& fmt, bool shapes = false) {
  std::vector<std::string> lines;
  for (auto& ln : splitlines(t))
    if (!trim(ln).empty()) lines.push_back(ln);
  static const Rx ITEM(R"(^\s*(?:[-*+]|\d+[.)])\s+\S|^#{1,6}\s)"), IND(R"(^\s+\S)"), CODE(R"([;{}=<>]|\(.*\))"),
      H(R"(^(#{1,6})\s+(.*)$)"), LI(R"(^(\s*)(?:[-*+]|\d+[.)])?\s*(.*)$)"), WL(R"(\[\[([^\]|]+)(?:\|[^\]]*)?\]\])"),
      MD(R"(\*\*|__|`)"), SH(R"(^[\w-]*[\[({]+(.*?)[\])}]+$)");
  size_t tree_n = 0;
  for (auto& ln : lines)
    if (ITEM.test(ln) || (IND.test(ln) && ulen(ln) < 100 && !CODE.test(ln))) ++tree_n;
  if (!shapes && (double)tree_n < std::max(2.0, lines.size() * 0.6))
    throw FormatError("el texto no tiene estructura de grafo (flechas, tablas o esquema con sangria)");
  std::vector<std::pair<long, std::string>> stack;
  int made = 0;
  for (auto& raw : splitlines(t)) {
    std::string st = trim(raw);
    if (st.empty() || starts_with(st, "```") || starts_with(st, "%%") || starts_with(st, "<!--")) continue;
    long level;
    std::string lab;
    auto h = H.match(raw);
    if (h) {
      level = (long)h->str(1).size() - 7;
      lab = h->str(2);
    } else {
      std::string ex = expandtabs(raw, 4);
      auto m = LI.match(ex);
      level = (long)ulen(m->str(1));
      lab = m->str(2);
    }
    lab = WL.sub(lab, "\\1");
    lab = trim(MD.sub(lab, ""));
    if (shapes) lab = SH.sub(lab, "\\1");
    if (lab.empty() || ulen(lab) > 200) continue;
    while (!stack.empty() && stack.back().first >= level) stack.pop_back();
    std::string nid = (stack.empty() ? "" : stack.back().second + "/") + uleft(lab, 60);
    b.node(nid, lab);
    if (!stack.empty()) {
      b.edge(stack.back().second, nid, "contiene");
      ++made;
    }
    stack.emplace_back(level, nid);
  }
  if (!made) throw FormatError("el texto no tiene estructura de grafo (flechas, tablas o esquema con sangria)");
  return b.result(fmt, true);
}

json parse_mermaid(const std::string& t) {
  static const Rx SEP(R"([\n;])");
  std::vector<std::string> lines;
  for (auto& ln : SEP.split(t)) {
    std::string s = trim(ln);
    if (!s.empty() && !starts_with(s, "%%")) lines.push_back(s);
  }
  Builder b;
  std::string kind = lines.empty() ? "" : lower(split_ws(lines[0])[0]);
  if (kind == "erdiagram") {
    static const Rx ER(R"rx(^"?([\w-]+)"?\s+[|}o{]{1,2}(?:--|\.\.)[|}o{]{1,2}\s+"?([\w-]+)"?\s*:\s*"?([^"]+)"?)rx");
    for (size_t i = 1; i < lines.size(); ++i) {
      auto m = ER.match(lines[i]);
      if (m) b.edge(m->str(1), m->str(2), m->str(3));
    }
    return b.result("mermaid");
  }
  if (kind == "mindmap") {
    auto nl = t.find('\n');
    return outline(nl == std::string::npos ? "" : t.substr(nl + 1), b, "mermaid", true);
  }
  static const Rx SKIP(R"(^(subgraph|end|classDef|class |style|linkStyle|click|direction)\b)"), CLS(R"(^\s*class\s+)"),
      SHAPE(R"((?<![-=.\w])([\w]+)\s*(\[\[.*?\]\]|\[\(.*?\)\]|\(\(\(.*?\)\)\)|\(\(.*?\)\)|\(\[.*?\]\)|)"
            R"(\{\{.*?\}\}|\[/.*?/\]|\[\\.*?\\\]|\[.*?\]|\(.*?\)|\{.*?\}|>.*?\]))"),
      STRIP(R"(^[\[({>/\\]+|[\])}/\\]+$)"), CLSARROW(R"(\s*(<\|--|--\|>|\*--|--\*|o--|--o|\.\.>|<\.\.|\.\.\|>)\s*)");
  json labels = json::object();
  std::vector<std::string> body;
  bool skip_first = kind == "graph" || kind == "flowchart" || kind == "classdiagram" || kind == "statediagram" ||
                    kind == "statediagram-v2";
  for (size_t i = skip_first ? 1 : 0; i < lines.size(); ++i) {
    std::string ln = lines[i];
    if (SKIP.match(ln)) continue;
    if (kind == "classdiagram") ln = CLS.sub(ln, "");
    ln = SHAPE.sub(ln, [&](const Rx::Match& m) {
      labels[m.str(1)] = trim(trim(STRIP.sub(m.str(2), "")), "\"");
      return m.str(1);
    });
    ln = CLSARROW.sub(ln, " --> ");
    body.push_back(ln);
  }
  int made = arrow_text(body, b, labels);
  for (auto& [k, v] : labels.items()) b.node(k, v);
  if (!made && !b.size()) throw FormatError("el diagrama Mermaid no tiene enlaces");
  return b.result("mermaid");
}

json parse_plantuml(const std::string& t) {
  static const Rx SKIP(R"(^(@start|@end|skinparam|title|left to right|top to bottom|hide|show|!|'|note|end note|legend|)"
                       R"(endlegend|package|namespace|}|{))",
                       Rx::I);
  std::vector<std::string> lines;
  for (auto& ln : splitlines(t)) {
    std::string s = trim(ln);
    if (!s.empty() && !SKIP.match(s)) lines.push_back(s);
  }
  Builder b;
  if (!arrow_text(lines, b)) throw FormatError("el diagrama PlantUML no tiene flechas");
  return b.result("plantuml");
}

const Rx& WIKILINK() {
  static const Rx R(R"(\[\[([^\]|#]+)(?:#[^\]|]*)?(?:\|[^\]]*)?\]\])");
  return R;
}

}  // namespace

json parse_wiki_notes(const Files& notes) {
  Builder b;
  int found = 0;
  static const Rx WS(R"(\s+)");
  for (auto& [name, t] : notes) {
    std::string k = mem::gkey(name);
    b.node(k.empty() ? name : k, name, obj({{"type", "nota"}, {"note", uleft(WS.sub(t, " "), 300)}}));
    for (auto& m : WIKILINK().finditer(t)) {
      std::string target = trim(m.str(1));
      if (target.empty()) continue;
      std::string tk = mem::gkey(target);
      b.node(tk.empty() ? target : tk, target, obj({{"type", "nota"}}));
      b.edge(k.empty() ? name : k, tk.empty() ? target : tk, "enlaza_con");
      ++found;
    }
  }
  return found ? b.result("markdown-wiki") : json(nullptr);
}

namespace {

json parse_text(const std::string& t, const std::string& ext = "", const std::string& name = "") {
  std::vector<std::string> lines;
  for (auto& ln : splitlines(t)) {
    std::string l = ltrim(ln);
    if (!trim(ln).empty() && !starts_with(l, "#") && !starts_with(l, "//") && !starts_with(l, "%")) lines.push_back(ln);
  }
  if (lines.empty()) throw FormatError("fichero vacio");
  Builder b;
  bool md = ext == ".md" || ext == ".markdown";
  if (md && WIKILINK().test(t)) {
    std::string st = stem(fs::path(name));
    return parse_wiki_notes({{st.empty() ? "nota" : st, t}});
  }
  static const Rx BUL(R"(^\s*[-*+]\s)");
  std::vector<std::string> arrows;
  for (auto& ln : lines)
    if (ARROW().test(ln) && !BUL.match(ln)) arrows.push_back(ln);
  if (!arrows.empty() && arrows.size() >= std::max<size_t>(1, lines.size() / 3))
    if (arrow_text(arrows, b)) return b.result("flechas");
  static const Rx ADJ(R"(^\s*([^:]{1,80}?)\s*:\s*(.+)$)"), DSEP(R"(\s*[,;]\s*)");
  bool all_adj = true;
  std::vector<std::pair<std::string, std::string>> adj;
  for (auto& ln : lines) {
    auto m = ADJ.match(ln);
    if (!m) {
      all_adj = false;
      break;
    }
    std::string g2 = m->str(2);
    if (!(contains(g2, ",") || ulen(g2) < 60)) all_adj = false;
    adj.emplace_back(trim(m->str(1)), g2);
  }
  if (all_adj && !md) {
    for (auto& [a, rest] : adj)
      for (auto& d : DSEP.split(rest))
        if (!trim(d).empty()) b.edge(a, trim(d));
    return b.result("adyacencia");
  }
  if (!md) {
    std::vector<std::string> first50(lines.begin(), lines.begin() + std::min<size_t>(50, lines.size()));
    std::string joined = join(first50, "\n");
    char delim = sniff_delimiter(joined);
    std::set<long> counts;
    for (size_t i = 0; i < lines.size() && i < 200; ++i) counts.insert(std::count(lines[i].begin(), lines[i].end(), delim));
    bool short_cells = true;
    for (auto& ln : first50)
      for (auto& c : split(ln, std::string(1, delim)))
        if (ulen(c) >= 120) short_cells = false;
    if (joined.find(delim) != std::string::npos && counts.size() == 1 && short_cells) {
      try {
        return parse_table(join(lines, "\n"), delim);
      } catch (const FormatError&) {
      }
    }
    bool el = true;
    for (size_t i = 0; i < lines.size() && i < 200; ++i) {
      size_t n = tsplit(lines[i]).size();
      if (n < 2 || n > 3 || ulen(lines[i]) >= 160) el = false;
    }
    if (el) return parse_edgelist(join(lines, "\n"));
  }
  return outline(t, b, "esquema");
}

// ------------------------------------------------------------------ contenedores
const std::set<std::string> TABLE_FORMATS = {"csv", "tsv", "tabla", "excel", "ods", "sqlite"};

std::pair<std::string, std::string> decompress(std::string data, std::string name) {
  for (int round = 0; round < 8; ++round) {
    std::optional<std::string> out;
    std::vector<std::string> exts;
    if (data.size() >= 2 && (unsigned char)data[0] == 0x1f && (unsigned char)data[1] == 0x8b) {
      out = archive::gunzip(data);
      exts = {".gz", ".tgz"};
    } else if (starts_with(data, "BZh")) {
      out = archive::bunzip2(data, MAX_BYTES + 1);
      exts = {".bz2"};
    } else if (starts_with(data, std::string("\xfd" "7zXZ\x00", 6))) {
      out = archive::unxz(data, MAX_BYTES + 1);
      exts = {".xz"};
    } else {
      return {data, name};
    }
    if (!out) throw FormatError("no se pudo descomprimir " + (name.empty() ? std::string("el fichero") : name));
    if (out->size() > MAX_BYTES) throw FormatError("descomprimido supera " + std::to_string(MAX_BYTES / (1024 * 1024)) + " MB");
    data = std::move(*out);
    std::string low = lower(name);
    for (auto& e : exts)
      if (ends_with(low, e)) {
        name = name.substr(0, name.size() - e.size()) + (e == ".tgz" ? ".tar" : "");
        low = lower(name);
      }
  }
  return {data, name};
}

bool is_tar(const std::string& d) { return d.size() > 262 && d.compare(257, 5, "ustar") == 0; }

Files tar_members(const std::string& d) {
  Files out;
  size_t pos = 0, total = 0;
  std::string long_name;
  while (pos + 512 <= d.size() && out.size() < 5000) {
    const char* h = d.data() + pos;
    if (std::all_of(h, h + 512, [](char c) { return c == 0; })) break;
    std::string nm(h, strnlen(h, 100));
    std::string prefix(h + 345, strnlen(h + 345, 155));
    if (!prefix.empty() && d.compare(pos + 257, 5, "ustar") == 0) nm = prefix + "/" + nm;
    size_t size = std::strtoull(std::string(h + 124, 12).c_str(), nullptr, 8);
    char type = h[156];
    pos += 512;
    if (pos + size > d.size()) break;
    std::string body = d.substr(pos, size);
    pos += (size + 511) / 512 * 512;
    if (type == 'L') {
      long_name = std::string(body.c_str());
      continue;
    }
    if (!long_name.empty()) {
      nm = long_name;
      long_name.clear();
    }
    if (type != '0' && type != '\0' && type != '7') continue;
    total += size;
    if (total > MAX_BYTES) throw FormatError("el TAR descomprimido es demasiado grande");
    out.emplace_back(nm, body);
  }
  return out;
}

Files members(const std::string& data) {
  Files out;
  if (starts_with(data, "PK\x03\x04")) {
    archive::Zip z(data, true);
    if (!z.ok()) throw FormatError("ZIP no valido");
    size_t total = 0, n = 0;
    for (auto& e : z.entries()) {
      if (++n > 5000) break;
      if (e.dir) continue;
      total += e.size;
      if (total > MAX_BYTES) throw FormatError("el ZIP descomprimido es demasiado grande");
      auto r = z.read(e.name);
      if (r) out.emplace_back(e.name, *r);
    }
  } else {
    out = tar_members(data);
  }
  Files keep;
  for (auto& [n, d] : out) {
    bool hidden = false;
    for (auto& p : split(n, "/"))
      if ((!p.empty() && p[0] == '.' && p != "." ) || p == "__MACOSX") hidden = true;
    if (!hidden) keep.emplace_back(n, d);
  }
  return keep;
}

const std::set<std::string> IGNORE_IN_ARCHIVE = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".pdf",  ".css",
                                                 ".js",  ".html", ".htm", ".py",  ".exe", ".dll", ".so",
                                                 ".bin", ".pyc",  ".ico", ".woff", ".woff2", ".ttf", ".mp3", ".mp4"};

std::string ext_of(const std::string& name) {
  std::string base = split(name, "/").back();
  auto p = base.rfind('.');
  return p == std::string::npos || p == 0 ? "" : lower(base.substr(p));
}

json parse_archive(const std::string& data, const std::string& name);

}  // namespace

json merge(const std::vector<std::pair<std::string, json>>& parts) {
  Builder b;
  std::vector<std::string> fmts;
  bool all_generic = true;
  for (size_t i = 0; i < parts.size(); ++i) {
    const json& g = parts[i].second;
    std::string f = jstr(g, "format", "?");
    bool shared = TABLE_FORMATS.count(f);
    std::string pre = shared ? "" : std::to_string(i) + ":";
    fmts.push_back(f);
    if (!truthy(get(g, "generic"))) all_generic = false;
    for (auto& n : g["nodes"]) {
      std::string nid = pre + n["id"].get<std::string>();
      bool fresh = !b.has_node(nid);
      json type = get(n, "type") != "concepto" ? get(n, "type") : json(nullptr);
      b.node(nid, n["name"], obj({{"type", type}, {"category", get(n, "category")}, {"note", get(n, "note")},
                                  {"weight", get(n, "weight")}}));
      if (fresh && n.contains("scope")) b.at(nid)["scope"] = n["scope"];
    }
    for (auto& e : g["edges"])
      b.edge(pre + e["src"].get<std::string>(), pre + e["dst"].get<std::string>(), e["rel"], get(e, "weight", 1));
  }
  std::vector<std::string> uniq;
  for (auto& f : fmts)
    if (std::find(uniq.begin(), uniq.end(), f) == uniq.end()) uniq.push_back(f);
  json out = b.result(uniq.size() > 1 ? join(uniq, "+") : (fmts.empty() ? "varios" : fmts[0]), all_generic);
  json files = json::array();
  for (auto& [n, g] : parts)
    files.push_back({{"file", n}, {"format", get(g, "format")}, {"nodes", g["nodes"].size()}, {"edges", g["edges"].size()}});
  out["files"] = files;
  return out;
}

std::string detect(const std::string& t, const std::string& ext) {
  std::string head = uleft(ltrim(t), 2000);
  std::string low = lower(head);
  if (ext == ".toml" || ext == ".yaml" || ext == ".yml") return ext == ".toml" ? "toml" : "yaml";
  static const Rx NT(R"(^(?:<[^>\s]+>|_:\S+)\s+<[^>\s]+>\s+\S)"), XMLH(R"(^<(?:\?xml|!--|!doctype|[a-z_][\w.:-]*[\s>/]))"),
      DOT(R"(^(?:strict\s+)?(?:di)?graph\s*(?:"[^"]*"|[\w.]+)?\s*\{)"), GML(R"(^(?:(?:creator|version|comment)\s+[^\n]*\s+)*graph\s*\[)"),
      MM(R"(^(?:flowchart|graph)\s*(?:tb|td|bt|rl|lr)?\s*(?:;|\n|$)|^(?:erdiagram|classdiagram|mindmap|statediagram(?:-v2)?)\b)"),
      PAJ(R"(^\*(?:network|vertices)\b)"), GDF(R"(^nodedef\s*>)"), UCI(R"(^dl\s+(?:n|nr|nm)\s*=)"),
      TTL(R"(^(?:@prefix|@base|prefix\s|base\s))"), CY1(R"((?im)^\s*(?:create|merge|match)\s*\()"),
      CY2(R"(\(\s*\w*\s*:\s*\w+[^)]*\)\s*-\s*\[)"), TGF(R"(^[^\n#]+\n(?:[^\n#]*\n)*#\s*\n)");
  static const std::map<std::string, std::string> EXT = {
      {".graphml", "xml"}, {".gexf", "xml"}, {".xgmml", "xml"}, {".dgml", "xml"}, {".gxl", "xml"}, {".rdf", "xml"},
      {".owl", "xml"}, {".xml", "xml"}, {".mm", "xml"}, {".opml", "xml"}, {".json", "json"}, {".jsonld", "json"},
      {".cyjs", "json"}, {".cx", "json"}, {".cx2", "json"}, {".canvas", "json"}, {".graphson", "json"},
      {".jgf", "json"}, {".jsonl", "jsonl"}, {".ndjson", "jsonl"}, {".yaml", "yaml"}, {".yml", "yaml"},
      {".toml", "toml"}, {".csv", "table"}, {".tsv", "table"}, {".tab", "table"}, {".dot", "dot"}, {".gv", "dot"},
      {".gml", "gml"}, {".net", "pajek"}, {".paj", "pajek"}, {".gdf", "gdf"}, {".dl", "ucinet"}, {".tgf", "tgf"},
      {".sif", "sif"}, {".lgl", "lgl"}, {".ncol", "edgelist"}, {".edgelist", "edgelist"}, {".edges", "edgelist"},
      {".el", "edgelist"}, {".wel", "edgelist"}, {".adjlist", "adjlist"}, {".nt", "rdf"}, {".nq", "rdf"},
      {".ttl", "rdf"}, {".trig", "rdf"}, {".n3", "rdf"}, {".cypher", "cypher"}, {".cql", "cypher"},
      {".cyp", "cypher"}, {".mmd", "mermaid"}, {".mermaid", "mermaid"}, {".puml", "plantuml"},
      {".plantuml", "plantuml"}, {".pu", "plantuml"}, {".md", "text"}, {".markdown", "text"}, {".txt", "text"}};
  if (NT.match(head)) return "rdf";
  if (XMLH.match(low)) return "xml";
  if ((starts_with(head, "{") || starts_with(head, "[")) && ext != ".ttl" && ext != ".n3" && ext != ".trig" && ext != ".gml")
    return "json";
  if (DOT.match(low)) return "dot";
  if (GML.match(low)) return "gml";
  if (MM.match(low)) return "mermaid";
  if (starts_with(low, "@startuml") || starts_with(low, "@startmindmap")) return starts_with(low, "@startuml") ? "plantuml" : "text";
  if (PAJ.match(low)) return "pajek";
  if (GDF.match(low)) return "gdf";
  if (UCI.match(low)) return "ucinet";
  if (TTL.match(low)) return "rdf";
  if (CY1.test(head) || CY2.test(head)) return "cypher";
  auto it = EXT.find(ext);
  if (it != EXT.end() && it->second != "xml" && it->second != "json") return it->second;
  if (starts_with(low, "---")) return "yaml";
  std::string h2 = head + "\n";
  if (TGF.match(h2) && (ext.empty() || ext == ".tgf")) return "tgf";
  return "text";
}

namespace {

const std::set<std::string> NOT_GRAPH_EXT = {".py",  ".js",  ".ts",   ".css", ".html", ".htm",  ".java", ".c",
                                             ".h",   ".cpp", ".cs",   ".go",  ".rs",   ".sh",   ".bat",  ".ps1",
                                             ".php", ".rb",  ".kt",   ".swift", ".sql", ".nsi", ".ini",  ".cfg",
                                             ".log", ".svg", ".pdf",  ".doc", ".docx", ".odt",  ".rtf",  ".epub",
                                             ".pptx", ".odp"};

bool binary(const std::string& d) {
  static const std::vector<std::string> MAGIC = {"%PDF-",      "\x89PNG",          "\xff\xd8\xff", "GIF8",
                                                 "\x7f" "ELF", "MZ",               "RIFF",         "OggS",
                                                 "ID3",        std::string(3, '\0'), "Rar!",        "7z\xbc\xaf",
                                                 "\xd0\xcf\x11\xe0", std::string("II*\0", 4), std::string("MM\0*", 4),
                                                 "wOFF",       "BM"};
  bool bom = starts_with(d, "\xff\xfe") || starts_with(d, "\xfe\xff");
  for (auto& m : MAGIC)
    if (starts_with(d, m) && !bom) return true;
  if (bom) return false;
  size_t n = std::min<size_t>(d.size(), 4096), ctrl = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char x = d[i];
    if (x < 9 || (13 < x && x < 32)) ++ctrl;
  }
  return ctrl > n * 0.05;
}

std::string decode(const std::string& d) {
  if (starts_with(d, "\xff\xfe")) return utf16_to_utf8(d.substr(2), true);
  if (starts_with(d, "\xfe\xff")) return utf16_to_utf8(d.substr(2), false);
  std::string s = starts_with(d, "\xEF\xBB\xBF") ? d.substr(3) : d;
  if (valid_utf8(s) == s) return s;
  return cp1252_to_utf8(d);
}

json parse_archive(const std::string& data, const std::string& name) {
  Files ms = members(data);
  std::set<std::string> names;
  for (auto& [n, _] : ms) names.insert(n);
  auto find = [&](const std::string& n) -> const std::string* {
    for (auto& [k, d] : ms)
      if (k == n) return &d;
    return nullptr;
  };
  if (names.count("xl/workbook.xml")) return parse_spreadsheet(data, "x.xlsx");
  const std::string* mt = find("mimetype");
  std::string mts = mt ? *mt : "";
  if (contains(mts, "opendocument.spreadsheet")) return parse_spreadsheet(data, "x.ods");
  if (names.count("content.json") && (names.count("metadata.json") || names.count("manifest.json"))) {
    json j = jparse(*find("content.json"));
    if (j.is_discarded()) throw FormatError("content.json de XMind no valido");
    return parse_json(j);
  }
  if (names.count("content.xml") && contains(find("content.xml")->substr(0, 400), "xmap-content"))
    return parse_xml(valid_utf8(*find("content.xml")));
  if (names.count("word/document.xml") || contains(mts, "opendocument.text") || names.count("ppt/presentation.xml"))
    throw FormatError("es un documento de oficina, no un grafo: importalo en la Biblioteca");
  std::vector<std::pair<std::string, json>> parts;
  Files notes;
  std::vector<std::string> errors;
  static const Rx NODEF(R"(node|nodo|vert|entit)", Rx::I);
  std::vector<std::pair<std::string, std::string>> sorted = ms;
  std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {
    bool na = !NODEF.test(a.first), nb = !NODEF.test(b.first);
    return na != nb ? na < nb : a.first < b.first;
  });
  for (auto& [n, d] : sorted) {
    std::string e = ext_of(n), base = split(n, "/").back();
    if (e == ".md" || e == ".markdown") {
      notes.emplace_back(stem(fs::path(base)), valid_utf8(d));
      continue;
    }
    if (IGNORE_IN_ARCHIVE.count(e)) continue;
    json g;
    try {
      g = parse_bytes(d, base);
    } catch (const std::exception&) {
      errors.push_back(n);
      continue;
    }
    if (!truthy(get(g, "generic"))) parts.emplace_back(n, g);
  }
  json wiki = notes.empty() ? json(nullptr) : parse_wiki_notes(notes);
  if (!wiki.is_null()) parts.emplace_back("notas [[enlazadas]]", wiki);
  if (parts.empty())
    throw FormatError("el comprimido no trae ningun grafo reconocible" +
                      (errors.empty() ? std::string() : " (" + std::to_string(errors.size()) + " ficheros no se pudieron leer)"));
  json g;
  if (parts.size() > 1) {
    g = merge(parts);
  } else {
    g = parts[0].second;
    g["files"] = json::array({{{"file", parts[0].first}, {"format", parts[0].second["format"]}}});
  }
  if (!errors.empty()) {
    json u = json::array();
    for (size_t i = 0; i < errors.size() && i < 50; ++i) u.push_back(errors[i]);
    g["unread"] = u;
  }
  return g;
}

}  // namespace

json parse_bytes(std::string data, std::string name) {
  if (data.empty()) throw FormatError("fichero vacio");
  std::tie(data, name) = decompress(std::move(data), name);
  if (starts_with(data, "PK\x03\x04") || is_tar(data)) return parse_archive(data, name);
  if (starts_with(data, std::string("SQLite format 3\0", 16))) return parse_sqlite(data);
  if (binary(data)) throw FormatError((name.empty() ? std::string("el fichero") : name) + " es binario o un documento, no un grafo");
  std::string ext = ext_of(name);
  std::string t = decode(data);
  std::string fmt = detect(t, ext);
  if (fmt == "text" && NOT_GRAPH_EXT.count(ext))
    throw FormatError(name + " no es un grafo (codigo, pagina web o documento): importalo en la Biblioteca");
  if (fmt == "xml") return parse_xml(t);
  if (fmt == "json") {
    json j = jparse(t);
    if (j.is_discarded()) return parse_jsonl(t);
    return parse_json(j);
  }
  if (fmt == "jsonl") return parse_jsonl(t);
  if (fmt == "table") return parse_table(t, ext == ".tsv" || ext == ".tab" ? '\t' : 0);
  if (fmt == "edgelist") return parse_edgelist(t);
  if (fmt == "adjlist") return parse_edgelist(t, true);
  if (fmt == "text") return parse_text(t, ext, name);
  if (fmt == "dot") return parse_dot(t);
  if (fmt == "gml") return parse_gml(t);
  if (fmt == "mermaid") return parse_mermaid(t);
  if (fmt == "plantuml") return parse_plantuml(t);
  if (fmt == "pajek") return parse_pajek(t);
  if (fmt == "gdf") return parse_gdf(t);
  if (fmt == "ucinet") return parse_ucinet(t);
  if (fmt == "rdf") return parse_rdf(t);
  if (fmt == "cypher") return parse_cypher(t);
  if (fmt == "yaml") return parse_yaml(t);
  if (fmt == "toml") return parse_toml(t);
  if (fmt == "tgf") return parse_tgf(t);
  if (fmt == "sif") return parse_sif(t);
  if (fmt == "lgl") return parse_lgl(t);
  throw FormatError("formato desconocido");
}

json parse_many(const Files& files) {
  std::vector<std::pair<std::string, json>> parts;
  for (auto& [n, d] : files) parts.emplace_back(n, parse_bytes(d, n));
  if (parts.size() == 1) return parts[0].second;
  std::stable_sort(parts.begin(), parts.end(), [](auto& a, auto& b) {
    bool ea = !a.second["edges"].empty(), eb = !b.second["edges"].empty();
    return ea != eb ? ea < eb : a.first < b.first;
  });
  return merge(parts);
}

json formats_json() {
  static const std::vector<std::array<const char*, 3>> F = {
      {"Nexus-Office", ".json", "exportación propia (respeta el grafo general y el de cada agente) o su base SQLite"},
      {"JSON node-link", ".json", "D3, vis.js, sigma.js, NetworkX (node_link y adjacency), JSON Graph Format"},
      {"Cytoscape", ".cyjs .cx .cx2 .sif .xgmml", "Cytoscape.js, CX/CX2, SIF y XGMML"},
      {"Neo4j", ".json .jsonl .csv .cypher", "Browser, API HTTP, APOC (json, cypher, graphml), CSV de neo4j-admin"},
      {"Cypher", ".cypher .cql", "CREATE / MERGE / MATCH ... CREATE"},
      {"JSON-LD", ".jsonld .json", "schema.org, Wikidata, @graph"},
      {"GraphSON", ".json .graphson", "TinkerPop, JanusGraph, Amazon Neptune"},
      {"RDF", ".ttl .nt .nq .trig .n3 .rdf .owl", "Turtle, N-Triples, N-Quads, TriG, N3, RDF/XML y OWL"},
      {"GraphML", ".graphml", "yEd, Gephi, NetworkX, igraph, APOC"},
      {"GEXF", ".gexf", "Gephi, con atributos"},
      {"GML", ".gml", "Graph Modelling Language: igraph, NetworkX, Cytoscape"},
      {"Graphviz DOT", ".dot .gv", "digraph / graph"},
      {"Pajek, GDF, UCINET, TGF, LGL, NCOL", ".net .gdf .dl .tgf .lgl .ncol", "análisis de redes"},
      {"Tablas", ".csv .tsv .xlsx .ods .db .sqlite", "aristas, nodos, matriz de adyacencia, tripletas, ConceptNet"},
      {"Listas", ".edgelist .adjlist .txt", "a b [peso], A: B, C, una tripleta por línea"},
      {"Diagramas", ".mmd .puml .txt", "Mermaid (flowchart, erDiagram, classDiagram, mindmap), PlantUML, A -> B"},
      {"Mapas mentales y esquemas", ".mm .opml .xmind .md", "FreeMind/Freeplane, OPML, XMind, listas con sangría"},
      {"Notas enlazadas", ".md .zip", "Obsidian, Logseq, Foam ([[enlaces]]) y Obsidian Canvas (.canvas)"},
      {"YAML / TOML", ".yaml .yml .toml", "con nodos y aristas, tripletas o cualquier estructura con nombres"},
      {"Comprimidos", ".zip .tar .gz .bz2 .xz", "uno o varios grafos dentro (p. ej. nodes.csv + edges.csv)"},
  };
  json fs_ = json::array();
  for (auto& f : F) fs_.push_back({{"name", f[0]}, {"extensions", f[1]}, {"description", f[2]}});
  std::set<std::string> ext = {".graphml", ".gexf", ".xgmml", ".dgml", ".gxl", ".rdf", ".owl", ".xml", ".mm", ".opml",
                               ".json", ".jsonld", ".cyjs", ".cx", ".cx2", ".canvas", ".graphson", ".jgf", ".jsonl",
                               ".ndjson", ".yaml", ".yml", ".toml", ".csv", ".tsv", ".tab", ".dot", ".gv", ".gml",
                               ".net", ".paj", ".gdf", ".dl", ".tgf", ".sif", ".lgl", ".ncol", ".edgelist", ".edges",
                               ".el", ".wel", ".adjlist", ".nt", ".nq", ".ttl", ".trig", ".n3", ".cypher", ".cql",
                               ".cyp", ".mmd", ".mermaid", ".puml", ".plantuml", ".pu", ".md", ".markdown", ".txt",
                               ".xlsx", ".ods", ".db", ".sqlite", ".sqlite3", ".zip", ".tar", ".tgz", ".gz", ".bz2",
                               ".xz", ".xmind"};
  json e = json::array();
  for (auto& x : ext) e.push_back(x);
  return {{"formats", fs_}, {"extensions", e}};
}

}  // namespace nx::kg
