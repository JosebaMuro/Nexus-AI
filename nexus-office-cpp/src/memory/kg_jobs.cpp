// Trabajos del gestor de conocimiento que leen ficheros (memory/kg_jobs.py): importar un grafo y razonar un archivo
// comprimido entero (grafos que traiga, biblioteca RAG, temas con embeddings y razonamiento con el LLM).
#include "memory/kg_jobs.h"

#include <algorithm>
#include <cmath>

#include "agents/agents.h"
#include "core/services.h"
#include "llm/llm.h"
#include "memory/kg.h"
#include "rag/rag.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::kg {

namespace {

auto L = log::get("kg");

const char* REASON_PROMPT = R"(Eres el bibliotecario de conocimiento de un asistente con varios agentes especialistas.
Lee el documento y devuelve SOLO un objeto JSON (sin texto alrededor) con esta forma:
{"resumen": "2-3 frases", "tipo": "manual|referencia|tutorial|codigo|notas|datos|otro",
 "ambito": "general" o el id del especialista al que corresponde este saber hacer,
 "hechos": [{"s": "sujeto", "r": "relacion_corta", "o": "objeto", "categoria": "comun|especialidad|procedimiento|comportamiento|relacion|preferencia"}],
 "pasos": ["paso 1", "paso 2"]}
- "general": conocimiento comun, pautas de comportamiento, relaciones o preferencias. Un especialista: su oficio.
- Como mucho 8 hechos concretos y utiles; "pasos" solo si explica como hacer algo (si no, lista vacia).
Especialistas: {agents}
Archivo: {path}
---
{text})";
const char* SYNTH_PROMPT = R"(Estos son los resumenes de los documentos de un archivo comprimido llamado «{name}».
Escribe en 3-4 frases de que trata el conjunto y para que sirve. Solo el texto.
{items})";

std::string fill(std::string t, const std::vector<std::pair<std::string, std::string>>& kv) {
  for (auto& [k, v] : kv) t = replace_all(t, "{" + k + "}", v);
  return t;
}

json json_obj(std::string raw) {
  if (raw.empty()) return nullptr;
  static const Rx FENCE(R"((?m)^```(?:json)?|```$)");
  raw = FENCE.sub(trim(raw), "");
  auto a = raw.find('{'), b = raw.rfind('}');
  if (a == std::string::npos || b == std::string::npos || b <= a) return nullptr;
  json d = jparse(raw.substr(a, b - a + 1));
  return d.is_object() ? d : json(nullptr);
}

std::string first_sentences(const std::string& text, size_t n = 2, size_t limit = 320) {
  static const Rx MD(R"([#>*`_\-]{2,})"), WS(R"(\s+)"), SENT(R"((?<=[.!?])\s+)");
  std::string t = trim(WS.sub(MD.sub(text, " "), " "));
  std::vector<std::string> parts;
  for (auto& p : SENT.split(t))
    if (ulen(p) > 30) parts.push_back(p);
  std::vector<std::string> head(parts.begin(), parts.begin() + std::min(n, parts.size()));
  std::string out = join(head, " ");
  return uleft(out.empty() ? t : out, limit);
}

std::string stem_of(const std::string& rel) {
  std::string base = split(rel, "/").back();
  auto p = base.rfind('.');
  return p == std::string::npos || p == 0 ? base : base.substr(0, p);
}

std::string suffix_of(const std::string& rel) {
  std::string base = split(rel, "/").back();
  auto p = base.rfind('.');
  return p == std::string::npos || p == 0 ? "" : lower(base.substr(p));
}

std::string last60(const std::string& s) { return uright(s, 60); }

bool in_cats(const json& cats, const std::string& c) {
  for (auto& x : cats)
    if (x == c) return true;
  return false;
}

}  // namespace

std::string resolve_target(Services& s, const std::string& target_in) {
  std::string target = trim(target_in);
  if (target.empty() || target == "general") return "";
  if (target == "auto") return "auto";
  if (!s.agents->get(target)) throw std::invalid_argument("agente desconocido: " + target);
  return target;
}

json run_import(Services& s, const json& parsed, const std::string& name, const std::string& target, const StepFn& step) {
  step("Formato " + jstr(parsed, "format") + ": " + std::to_string(parsed["nodes"].size()) + " nodos y " +
           std::to_string(parsed["edges"].size()) + " relaciones",
       -1, -1);
  json res = insert_graph(*s.graph, parsed, target, s.organizer.get(), s.agents->ids(), "grafo:" + name,
                          [&](size_t d, size_t t) {
                            step("Clasificando nodos " + std::to_string(d) + "/" + std::to_string(t), (long)d, (long)t);
                          });
  step("Insertados " + std::to_string(res["nodes"].get<long long>()) + " nodos (" + std::to_string(res["new"].get<long long>()) +
           " nuevos, " + std::to_string(res["merged"].get<long long>()) + " fusionados con los que ya habia) y " +
           std::to_string(res["relations"].get<long long>()) + " relaciones",
       -1, -1);
  return res;
}

namespace {

json themes(Services& s, const StepFn& step, const std::vector<std::pair<std::string, std::string>>& items,
            mem::KnowledgeGraph& view, const std::string& arch_title, std::vector<std::vector<size_t>>& members_out) {
  auto emb = s.embedder();
  bool semantic = !starts_with(emb->id, "hash");
  double thr = semantic ? 0.62 : 0.3;
  std::vector<std::string> texts;
  for (auto& [rel, t] : items) texts.push_back(rel + "\n" + uleft(t, 600));
  mem::Mat mat;
  for (size_t b = 0; b < texts.size(); b += 32) {
    std::vector<std::string> blk(texts.begin() + b, texts.begin() + std::min(texts.size(), b + 32));
    auto m = emb->embed_query(blk, "organize");
    mat.insert(mat.end(), m.begin(), m.end());
    size_t d = std::min(b + 32, texts.size());
    step("Organizando por temas " + std::to_string(d) + "/" + std::to_string(texts.size()), (long)d, (long)texts.size());
  }
  struct Cl {
    std::vector<size_t> members;
    mem::Vec centroid;
  };
  std::vector<Cl> clusters;
  for (size_t i = 0; i < mat.size(); ++i) {
    const auto& v = mat[i];
    Cl* best = nullptr;
    double sim = -1;
    for (auto& c : clusters) {
      double nrm = 0;
      for (float x : c.centroid) nrm += (double)x * x;
      double cs = mem::dot(c.centroid, v) / std::max(std::sqrt(nrm), 1e-9);
      if (cs > sim) {
        best = &c;
        sim = cs;
      }
    }
    if (best && (sim >= thr || clusters.size() >= 12)) {
      best->members.push_back(i);
      for (size_t k = 0; k < v.size() && k < best->centroid.size(); ++k) best->centroid[k] += v[k];
    } else {
      clusters.push_back({{i}, v});
    }
  }
  std::stable_sort(clusters.begin(), clusters.end(), [](const Cl& a, const Cl& b) { return a.members.size() > b.members.size(); });
  json out = json::array();
  static const Rx WS(R"(\s+)");
  for (auto& c : clusters) {
    std::string joined;
    for (size_t k = 0; k < c.members.size() && k < 20; ++k) {
      auto& [r, t] = items[c.members[k]];
      joined += replace_all(stem_of(r), "_", " ") + ". " + uleft(t, 800) + " ";
    }
    auto kws = mem::KnowledgeGraph::keywords(joined, 3);
    std::string label = kws.empty() ? "Tema " + std::to_string(out.size() + 1) : uleft(WS.sub("Tema: " + join(kws, ", "), " "), 60);
    view.upsert_entity(label, "tema", 1 + 0.2 * c.members.size(), "",
                       std::to_string(c.members.size()) + " documentos de " + arch_title, "organizador");
    view.relate(arch_title, "contiene", label, "proyecto", "tema", 1.0, "", "documento");
    for (size_t k = 0; k < c.members.size() && k < 60; ++k)
      view.relate(label, "incluye", last60(items[c.members[k]].first), "tema", "documento", 0.6, "documento");
    json ex = json::array();
    for (size_t k = 0; k < c.members.size() && k < 5; ++k) ex.push_back(items[c.members[k]].first);
    out.push_back({{"theme", label}, {"documents", c.members.size()}, {"examples", ex}});
    members_out.push_back(c.members);
  }
  return out;
}

json apply_reasoning(Services& s, const json& out, const std::string& rel, const std::string& title,
                     const std::string& scope, const std::string& name, json& report) {
  std::string resumen = uleft(jstr(out, "resumen"), 500);
  std::string amb = lower(trim(jstr(out, "ambito")));
  std::string dscope = scope;
  if (!amb.empty() && amb != "general" && amb != "comun" && amb != "común" && s.agents->get(amb) && scope.empty()) dscope = amb;
  auto& view = s.graph->for_agent(dscope);
  view.upsert_entity(title, "documento", 1.5, "documento", resumen, "zip:" + name);
  int n = 0;
  json hechos = out.value("hechos", json::array());
  for (size_t i = 0; hechos.is_array() && i < hechos.size() && i < 8; ++i) {
    const json& h = hechos[i];
    if (!h.is_object() || !truthy(h.value("s", json())) || !truthy(h.value("o", json()))) continue;
    std::string cat = jstr(h, "categoria");
    if (!in_cats(mem::CATEGORIES, cat)) cat = "";
    mem::KnowledgeGraph* hv = (in_cats(mem::GENERAL_CATEGORIES, cat) && cat != "comun") ? &s.graph->general() : &view;
    if (cat == "comun" && !dscope.empty()) hv = &s.graph->general();
    if (in_cats(mem::AGENT_CATEGORIES, cat) && hv->scope.empty()) cat = "comun";
    std::string r = jstr(h, "r");
    if (hv->relate(uleft(jstr(h, "s"), 60), uleft(r.empty() ? "relacionado_con" : r, 30), uleft(jstr(h, "o"), 60), "concepto",
                   "concepto", 1.0, cat, cat)) {
      if (hv == &view)
        hv->relate(title, "explica", uleft(jstr(h, "s"), 60), "documento", "concepto", 0.5, "", "documento");
      ++n;
    }
  }
  report["facts"] = report["facts"].get<long long>() + n;
  std::vector<std::string> steps;
  json pasos = out.value("pasos", json::array());
  for (size_t i = 0; pasos.is_array() && i < pasos.size() && steps.size() < 12; ++i) {
    std::string p = pasos[i].is_string() ? pasos[i].get<std::string>() : jdump(pasos[i]);
    if (!trim(p).empty()) steps.push_back(uleft(p, 200));
  }
  if (!steps.empty()) {
    s.memory->remember("Procedimiento (" + rel + "): " + join(steps, " → "), "skill", 0.75, "zip:" + name, dscope);
    view.upsert_entity(uleft("Cómo: " + title, 60), "procedimiento", 1.2, dscope.empty() ? "comun" : "procedimiento",
                       uleft(join(steps, " → "), 500), "zip:" + name);
    report["skills"] = report["skills"].get<long long>() + 1;
  }
  if (!resumen.empty())
    s.memory->remember(title + ": " + resumen, dscope.empty() ? "note" : "skill", 0.6, "zip:" + name, dscope);
  return {{"resumen", resumen}, {"ambito", dscope.empty() ? "general" : dscope}, {"hechos", n}, {"pasos", steps.size()}};
}

}  // namespace

json run_archive(Services& s, const fs::path& path, const std::string& name, const std::string& target, bool reason,
                 bool rag, int max_reason, const StepFn& step) {
  fs::path work = paths::sub({"cache"}) / ("kgzip_" + token_hex(6));
  mkdirs(work);
  json report = {{"archive", name}, {"graphs", json::array()}, {"themes", json::array()},
                 {"reasoned", 0},   {"facts", 0},                {"skills", 0}};
  struct Clean {
    fs::path w, p;
    ~Clean() {
      rm_rf(w);
      if (is_relative_to(p, paths::sub({"cache"}))) remove_file(p);
    }
  } clean{work, path};
  std::string kind = rag::detect_kind(path, name);
  if (kind.empty()) throw std::invalid_argument("el fichero no es un ZIP, RAR, 7z o tar valido");
  step("Descomprimiendo de forma segura", -1, -1);
  auto res = rag::extract_any(path, work, kind);
  auto& files = res.files;
  report["files"] = files.size();
  step(std::to_string(files.size()) + " ficheros", -1, -1);

  // 1) grafos de conocimiento que vengan dentro
  std::vector<std::pair<std::string, std::string>> graph_files;
  for (auto& rel : files) {
    fs::path p = work / P(rel);
    if (GRAPH_EXT.count(suffix_of(rel)) && fsize(p) < 50u * 1024 * 1024) {
      std::string data = read_file(p).value_or("");
      if (looks_like_graph(split(rel, "/").back(), data)) graph_files.emplace_back(rel, data);
    }
  }
  // 2) documentos legibles (lo que no es grafo)
  std::set<std::string> gset;
  for (auto& g : graph_files) gset.insert(g.first);
  std::vector<std::string> docs;
  for (auto& r : files)
    if (rag::indexable(r) && !gset.count(r)) docs.push_back(r);
  static const Rx PRIO(R"((^|/)(readme|index|overview|introduc|getting[-_ ]started|guia|guide|manual|tutorial|)"
                       R"(arquitectura|architecture|design|docs?/))",
                       Rx::I);
  auto light = [](const std::string& r) {
    std::string sfx = suffix_of(r);
    return sfx == ".md" || sfx == ".rst" || sfx == ".txt" || sfx == ".adoc" || rag::RICH_EXT.count(sfx);
  };
  std::stable_sort(docs.begin(), docs.end(), [&](const std::string& a, const std::string& b) {
    auto key = [&](const std::string& r) {
      return std::make_tuple(PRIO.test(r) ? 0 : 1, light(r) ? 0 : 1, std::count(r.begin(), r.end(), '/') + 1, r);
    };
    return key(a) < key(b);
  });
  std::vector<std::pair<std::string, std::string>> texts;
  size_t nread = std::min<size_t>(docs.size(), 600);
  for (size_t i = 0; i < nread; ++i) {
    auto [pages, err] = rag::read_for_index(work / P(docs[i]));
    if (err.empty() && !pages.empty()) texts.emplace_back(docs[i], uleft(join(pages, "\n"), 20000));
    if (i % 40 == 0) step("Leyendo documentos " + std::to_string(i + 1) + "/" + std::to_string(nread), (long)i + 1, (long)nread);
  }
  Files notes;
  for (auto& [r, t] : texts) {
    std::string sfx = suffix_of(r);
    if (sfx == ".md" || sfx == ".markdown") notes.emplace_back(stem_of(r), t);
  }
  json wiki = parse_wiki_notes(notes);

  // 3) destino
  std::string scope = target;
  if (target == "auto") {
    std::string sample = name + "\n";
    for (size_t i = 0; i < texts.size() && i < 12; ++i) sample += texts[i].first + ": " + uleft(texts[i].second, 400) + "\n";
    scope = texts.empty() ? "" : s.organizer->route(sample).first;
  }
  report["scope"] = scope.empty() ? "general" : scope;
  auto spec = scope.empty() ? std::nullopt : s.agents->get(scope);
  step("Destino: grafo " + (spec ? spec->name : std::string("general")), -1, -1);
  std::string graph_target = target == "auto" ? "auto" : scope;
  for (auto& [rel, data] : graph_files) {
    try {
      json parsed = parse_bytes(data, split(rel, "/").back());
      json r = insert_graph(*s.graph, parsed, graph_target, s.organizer.get(), s.agents->ids(), "zip:" + name);
      json g = {{"file", rel}};
      for (auto& [k, v] : r.items()) g[k] = v;
      report["graphs"].push_back(g);
      step("Grafo " + rel + ": " + std::to_string(r["nodes"].get<long long>()) + " nodos, " +
               std::to_string(r["relations"].get<long long>()) + " relaciones",
           -1, -1);
    } catch (const std::exception& e) {
      report["graphs"].push_back({{"file", rel}, {"error", uleft(e.what(), 200)}});
    }
  }
  if (!wiki.is_null()) {
    json r = insert_graph(*s.graph, wiki, graph_target, s.organizer.get(), s.agents->ids(), "zip:" + name);
    json g = {{"file", "notas [[enlazadas]]"}};
    for (auto& [k, v] : r.items()) g[k] = v;
    report["graphs"].push_back(g);
    step("Notas enlazadas: " + std::to_string(r["nodes"].get<long long>()) + " notas, " +
             std::to_string(r["relations"].get<long long>()) + " enlaces",
         -1, -1);
  }
  // 4) biblioteca RAG
  if (rag && !docs.empty()) {
    step("Indexando en la biblioteca RAG", -1, -1);
    json doc = s.rag->ingest_file(path, "", name, json::object(), true, scope);
    report["rag_doc"] = {{"id", doc["id"]}, {"title", doc["title"]}, {"chunks", doc["chunks"]}};
  }
  auto& view = s.graph->for_agent(scope);
  static const Rx ARCH_EXT(R"(\.(zip|rar|7z|tar|tgz|tar\.gz|tar\.bz2|tar\.xz)$)", Rx::I);
  std::string arch_title = uleft(ARCH_EXT.sub(name, ""), 60);
  view.upsert_entity(arch_title, "proyecto", 3, "documento", "Archivo " + name + ": " + std::to_string(files.size()) + " ficheros",
                     "zip:" + name);
  view.relate(view.self_name, "ha_estudiado", arch_title, view.self_type, "proyecto", 1.0, "documento");

  // 5) temas
  std::vector<std::pair<std::string, std::string>> items(texts.begin(), texts.begin() + std::min<size_t>(400, texts.size()));
  std::vector<std::vector<size_t>> members;
  json th = items.empty() ? json::array() : themes(s, step, items, view, arch_title, members);
  report["themes"] = th;

  // 6) razonamiento con el LLM
  json reasoned = json::array();
  bool llm_ok = reason && s.llm->has_real_llm() && max_reason > 0;
  std::set<size_t> chosen;
  std::vector<size_t> chosen_order;
  if (llm_ok) {
    for (auto& m : members) {
      if ((int)chosen.size() >= max_reason) break;
      size_t f = *std::min_element(m.begin(), m.end());
      if (chosen.insert(f).second) chosen_order.push_back(f);
    }
    for (size_t i = 0; i < items.size() && (int)chosen.size() < max_reason; ++i)
      if (chosen.insert(i).second) chosen_order.push_back(i);
  }
  std::string agents_txt;
  for (auto& a : s.organizer->specialists()) agents_txt += (agents_txt.empty() ? "" : "; ") + a.id + " (" + a.description + ")";
  if (agents_txt.empty()) agents_txt = "ninguno";
  for (size_t i = 0; i < items.size(); ++i) {
    auto& [rel, text] = items[i];
    std::string title = last60(rel);
    if (chosen.count(i)) {
      step("Razonando " + std::to_string(reasoned.size() + 1) + "/" + std::to_string(chosen.size()) + ": " + rel,
           (long)reasoned.size(), (long)chosen.size());
      json out = nullptr;
      try {
        llm::Opts o;
        o.o = {{"max_tokens", 700}, {"temperature", 0.1}};
        out = json_obj(s.llm->complete(fill(REASON_PROMPT, {{"agents", agents_txt}, {"path", rel}, {"text", uleft(text, 3500)}}),
                                       "default", o));
      } catch (const std::exception& e) {
        L.warning("Razonamiento de ", rel, " fallido: ", e.what());
      }
      if (!out.is_null()) {
        json r = {{"file", rel}};
        for (auto& [k, v] : apply_reasoning(s, out, rel, title, scope, name, report).items()) r[k] = v;
        reasoned.push_back(r);
        continue;
      }
    }
    view.upsert_entity(title, "documento", 1, "documento", first_sentences(text), "zip:" + name);
    if (i < 120) report["facts"] = report["facts"].get<long long>() + (long long)view.extract_topics(uleft(text, 3000), "user", "").size();
  }
  report["reasoned"] = reasoned.size();
  json rs = json::array();
  for (size_t i = 0; i < reasoned.size() && i < 50; ++i) rs.push_back(reasoned[i]);
  report["reasoning"] = rs;
  if (!reasoned.empty() && llm_ok) {
    step("Sintetizando el conjunto", -1, -1);
    std::string synth;
    try {
      std::string list;
      for (size_t i = 0; i < reasoned.size() && i < 20; ++i)
        list += "- " + jstr(reasoned[i], "file") + ": " + jstr(reasoned[i], "resumen") + "\n";
      llm::Opts o;
      o.o = {{"max_tokens", 300}, {"temperature", 0.2}};
      synth = s.llm->complete(fill(SYNTH_PROMPT, {{"name", name}, {"items", rtrim(list, "\n")}}), "default", o);
    } catch (...) {
    }
    if (!trim(synth).empty()) {
      report["summary"] = uleft(trim(synth), 1200);
      s.graph->db.execute("UPDATE entities SET note=? WHERE scope=? AND key=?",
                          {uleft(report["summary"].get<std::string>(), 500), scope, mem::gkey(arch_title)});
      s.memory->remember(arch_title + ": " + report["summary"].get<std::string>(), scope.empty() ? "note" : "skill", 0.7,
                         "zip:" + name, scope);
    }
  }
  report["llm"] = llm_ok;
  step("Terminado", -1, -1);
  return report;
}

}  // namespace nx::kg
