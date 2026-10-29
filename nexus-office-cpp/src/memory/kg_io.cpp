// Insercion y exportacion de grafos (memory/kg_io.py): fusiona un grafo leido con el general, con el de un agente o
// reparte cada nodo con el organizador ("auto"); exporta en JSON propio, GraphML o CSV.
#include <cmath>
#include <set>

#include "core/db.h"
#include "memory/kg.h"
#include "memory/memory.h"
#include "util/crypto.h"
#include "util/csv.h"
#include "util/text.h"

namespace nx::kg {

const std::set<std::string> GRAPH_EXT = {".json", ".jsonl", ".ndjson", ".jsonld", ".cyjs", ".graphson", ".canvas", ".csv",
                                         ".tsv",  ".graphml", ".gexf", ".xgmml", ".dgml", ".gxl", ".gml", ".dot", ".gv",
                                         ".net",  ".paj", ".gdf", ".dl", ".tgf", ".sif", ".lgl", ".ncol", ".edgelist",
                                         ".adjlist", ".nt", ".nq", ".ttl", ".trig", ".n3", ".rdf", ".owl", ".cypher",
                                         ".cql",  ".mmd", ".mermaid", ".puml", ".mm", ".opml"};

namespace {

std::string ext_of(const std::string& name) {
  std::string base = split(name, "/").back();
  auto p = base.rfind('.');
  return p == std::string::npos || p == 0 ? "" : lower(base.substr(p));
}

double num_or(const json& v, double def) {
  if (v.is_number()) return std::max(0.0, v.get<double>());
  if (v.is_string()) {
    try {
      size_t used;
      double d = std::stod(trim(v.get<std::string>()), &used);
      return std::max(0.0, d);
    } catch (...) {
    }
  }
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  return def;
}

std::string xesc(const json& v) {
  std::string s = v.is_string() ? v.get<std::string>() : v.is_null() ? "None" : jdump(v);
  s = replace_all(s, "&", "&amp;");
  s = replace_all(s, "<", "&lt;");
  s = replace_all(s, ">", "&gt;");
  return replace_all(s, "\"", "&quot;");
}

}  // namespace

bool looks_like_graph(const std::string& name, const std::string& data) {
  std::string ext = ext_of(name);
  if (!GRAPH_EXT.count(ext)) return false;
  json g;
  try {
    g = parse_bytes(data.substr(0, 20000000), name);
  } catch (const std::exception&) {
    return false;
  }
  if (truthy(g.value("generic", json(false)))) return false;
  static const std::set<std::string> LOOSE = {".json", ".jsonl", ".ndjson", ".csv", ".tsv", ".canvas"};
  if (!LOOSE.count(ext)) return true;
  if (ext == ".csv" || ext == ".tsv") {
    std::string t = jstr(g, "table");
    return t == "aristas" || t == "neo4j-aristas" || t == "neo4j-nodos";
  }
  return !g["edges"].empty();
}

json insert_graph(mem::KnowledgeGraph& graph, json parsed, const std::string& target, mem::Organizer* organizer,
                  const std::vector<std::string>& known_agents, const std::string& source, const Progress& progress) {
  json& nodes = parsed["nodes"];
  json& edges = parsed["edges"];
  std::map<std::string, std::string> scope_of;
  std::set<std::string> known(known_agents.begin(), known_agents.end());
  bool autot = target == "auto";
  auto known_scope = [&](const json& n) {
    if (!n.contains("scope") || !n["scope"].is_string()) return false;
    std::string s = n["scope"];
    return s.empty() || known.count(s);
  };
  if (autot && organizer) {
    std::map<std::string, std::vector<std::string>> neigh;
    std::map<std::string, std::string> names;
    for (auto& n : nodes) names[n["id"].get<std::string>()] = n["name"].get<std::string>();
    for (size_t i = 0; i < edges.size() && i < 200000; ++i) {
      std::string s = edges[i]["src"], d = edges[i]["dst"];
      neigh[s].push_back(names.count(d) ? names[d] : "");
      neigh[d].push_back(names.count(s) ? names[s] : "");
    }
    std::vector<json*> pending;
    for (auto& n : nodes)
      if (!known_scope(n)) pending.push_back(&n);
    for (size_t b = 0; b < pending.size(); b += 64) {
      std::vector<std::string> texts;
      size_t e = std::min(pending.size(), b + 64);
      for (size_t i = b; i < e; ++i) {
        json& n = *pending[i];
        auto& nb = neigh[n["id"].get<std::string>()];
        std::vector<std::string> first8(nb.begin(), nb.begin() + std::min<size_t>(8, nb.size()));
        texts.push_back(n["name"].get<std::string>() + ". " + jstr(n, "note") + " " + join(first8, ", "));
      }
      json aff = organizer->affinities(texts);
      for (size_t i = b; i < e; ++i) {
        json& n = *pending[i];
        scope_of[n["id"].get<std::string>()] = jstr(aff[i - b], "scope");
        if (!truthy(n.value("category", json(""))) ) n["category"] = aff[i - b]["category"];
      }
      if (progress) progress(e, pending.size());
    }
  }
  std::map<std::string, long long> ids;
  int skipped = 0, made = 0;
  long long last = (long long)jnum(graph.db.scalar("SELECT COALESCE(MAX(id), 0) AS m FROM entities"));
  std::map<long long, std::string> scopes;
  graph.db.transaction([&] {
    for (auto& n : nodes) {
      std::string id = n["id"];
      std::string scope;
      if (known_scope(n) && jstr(parsed, "format") == "nexus-kg" && autot) {
        scope = n["scope"];
      } else {
        auto it = scope_of.find(id);
        scope = it != scope_of.end() ? it->second : (autot ? "" : (target.empty() ? mem::GENERAL : target));
      }
      if (!scope.empty() && !known.count(scope)) scope = mem::GENERAL;
      std::string cat = jstr(n, "category");
      bool valid = false;
      for (auto& c : mem::CATEGORIES)
        if (c == cat) valid = true;
      if (!valid) cat = "";
      auto& view = graph.for_agent(scope);
      std::string etype = uleft(truthy(n.value("type", json(""))) ? jstr(n, "type") : "concepto", 30);
      if (etype == "self" || etype == "agente") etype = "concepto";
      long long eid = view.upsert_entity(n["name"], etype, num_or(n.value("weight", json(nullptr)), 1.0), cat,
                                         jstr(n, "note"), source);
      if (eid) ids[id] = eid;
    }
    std::set<long long> uniq;
    for (auto& [_, v] : ids) uniq.insert(v);
    if (!uniq.empty()) {
      std::vector<std::string> list;
      for (auto v : uniq) list.push_back(std::to_string(v));
      for (auto& r : graph.db.query("SELECT id, scope FROM entities WHERE id IN (" + join(list, ",") + ")"))
        scopes[r["id"].get<long long>()] = jstr(r, "scope");
    }
    for (auto& e : edges) {
      auto a = ids.find(e["src"]), b = ids.find(e["dst"]);
      if (a == ids.end() || b == ids.end() || a->second == b->second) {
        ++skipped;
        continue;
      }
      std::string rs;
      try {
        rs = mem::KnowledgeGraph::relation_scope(scopes[a->second], scopes[b->second]);
      } catch (const std::invalid_argument&) {
        ++skipped;
        continue;
      }
      graph.db.execute("INSERT INTO relations(src, dst, rel, weight, last_seen, scope) VALUES(?,?,?,?,?,?) "
                       "ON CONFLICT(src, dst, rel) DO UPDATE SET weight=weight+excluded.weight",
                       {a->second, b->second, e["rel"], num_or(e.value("weight", json(nullptr)), 1.0), now(), rs});
      ++made;
    }
  });
  std::set<long long> uniq;
  for (auto& [_, v] : ids) uniq.insert(v);
  json by_scope = json::object();
  for (auto eid : uniq) {
    std::string sc = scopes.count(eid) && !scopes[eid].empty() ? scopes[eid] : "general";
    by_scope[sc] = by_scope.value(sc, 0) + 1;
  }
  long long fresh = 0;
  for (auto i : uniq)
    if (i > last) ++fresh;
  json out = {{"format", parsed.value("format", json(nullptr))},
              {"nodes", uniq.size()},
              {"new", fresh},
              {"merged", (long long)uniq.size() - fresh},
              {"relations", made},
              {"skipped", skipped},
              {"by_scope", by_scope}};
  for (auto* k : {"files", "unread", "table"})
    if (truthy(parsed.value(k, json(nullptr)))) out[k] = parsed[k];
  return out;
}

std::tuple<std::string, std::string, std::string> export_graph(mem::KnowledgeGraph& graph,
                                                               const std::optional<std::string>& scope,
                                                               const std::string& fmt) {
  std::string where = scope ? "WHERE scope=?" : "";
  Params p;
  if (scope) p.push_back(*scope);
  auto nodes = graph.db.query("SELECT id, name, type, category, note, scope, weight FROM entities " + where + " ORDER BY id", p);
  std::set<long long> idset;
  std::map<long long, std::string> names;
  for (auto& n : nodes) {
    idset.insert(n["id"].get<long long>());
    names[n["id"].get<long long>()] = jstr(n, "name");
  }
  std::vector<Row> rels;
  for (auto& r : graph.db.query("SELECT src, dst, rel, weight, scope FROM relations " + where + " ORDER BY id", p))
    if (idset.count(r["src"].get<long long>()) && idset.count(r["dst"].get<long long>())) rels.push_back(r);
  if (fmt == "csv") {
    std::string out = csv::row({"source", "relation", "target", "weight", "scope"});
    for (auto& r : rels)
      out += csv::row({names[r["src"].get<long long>()], jstr(r, "rel"), names[r["dst"].get<long long>()],
                       pyfloat(round_to(jnum(r["weight"]), 3)), jstr(r, "scope")});
    return {out, "text/csv", "csv"};
  }
  if (fmt == "graphml") {
    std::vector<std::string> out = {"<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
                                    "<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\">"};
    for (auto* k : {"label", "type", "category", "note", "scope"})
      out.push_back(std::string("<key id=\"") + k + "\" for=\"node\" attr.name=\"" + k + "\" attr.type=\"string\"/>");
    out.push_back("<key id=\"rel\" for=\"edge\" attr.name=\"label\" attr.type=\"string\"/>");
    out.push_back("<key id=\"w\" for=\"edge\" attr.name=\"weight\" attr.type=\"double\"/>");
    out.push_back("<graph edgedefault=\"directed\">");
    for (auto& n : nodes)
      out.push_back("<node id=\"n" + std::to_string(n["id"].get<long long>()) + "\"><data key=\"label\">" + xesc(n["name"]) +
                    "</data><data key=\"type\">" + xesc(n["type"]) + "</data><data key=\"category\">" +
                    xesc(n["category"]) + "</data><data key=\"note\">" + xesc(n["note"]) + "</data><data key=\"scope\">" +
                    xesc(n["scope"]) + "</data></node>");
    for (auto& r : rels)
      out.push_back("<edge source=\"n" + std::to_string(r["src"].get<long long>()) + "\" target=\"n" +
                    std::to_string(r["dst"].get<long long>()) + "\"><data key=\"rel\">" + xesc(r["rel"]) +
                    "</data><data key=\"w\">" + pyfloat(jnum(r["weight"])) + "</data></edge>");
    out.push_back("</graph>");
    out.push_back("</graphml>");
    return {join(out, "\n"), "application/xml", "graphml"};
  }
  json ns = json::array(), es = json::array();
  for (auto& n : nodes) {
    double w = jnum(n["weight"]);
    ns.push_back({{"id", n["id"]}, {"name", n["name"]}, {"type", n["type"]}, {"category", n["category"]},
                  {"note", n["note"]}, {"scope", n["scope"]}, {"weight", round_to(w ? w : 1, 3)}});
  }
  for (auto& r : rels) {
    double w = jnum(r["weight"]);
    es.push_back({{"src", r["src"]}, {"dst", r["dst"]}, {"rel", r["rel"]}, {"weight", round_to(w ? w : 1, 3)},
                  {"scope", r["scope"]}});
  }
  json data = {{"format", "nexus-kg"}, {"version", 1}, {"nodes", ns}, {"edges", es}};
  return {data.dump(1, ' ', false, json::error_handler_t::replace), "application/json", "json"};
}

}  // namespace nx::kg
