// Grafo multicapa de conocimiento de la Wikipedia en espanol, como ejemplo para importar en Nexus-Office.
//
//   nexus-construir grafo-wikipedia [--salida ejemplos/grafo-wikipedia-es] [--articulos 1500] [--por-categoria 12]
//                                   [--profundidad 1] [--raices Ciencia,Historia,...]
//
// Usa las API publicas de es.wikipedia.org y wikidata.org (hace falta internet; se lanza desde la GitHub Action
// wikipedia-grafo.yml). Capas:
//   1-temas      categorias de la Wikipedia, desde unas raices tematicas (ciencia, historia, arte...), con su jerarquia
//   2-articulos  articulos de esas categorias, con el resumen de su introduccion como nota
//   3-enlaces    enlaces entre esos articulos (solo los que quedan dentro del grafo)
//   4-wikidata   hechos de Wikidata de cada articulo: que es, pais, lugar, autor, fechas, poblacion...
// Cada nodo y cada arista lleva su capa (`capa`), de modo que se puede filtrar en Gephi o al preguntar a los agentes.
// Salida: grafo.graphml (Gephi, yEd, Nexus-Office), grafo.json (node-link de D3/NetworkX) y LEEME.md con las cifras.
// Licencia de los textos: CC BY-SA 4.0 (Wikipedia); datos de Wikidata: CC0.
#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <thread>

#include "comun.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/text.h"

namespace nc {

using namespace nx;

namespace {

const std::string WIKI = "https://es.wikipedia.org/w/api.php";
const std::string WIKIDATA = "https://www.wikidata.org/w/api.php";
const std::string UA = "Nexus-Office-grafo/2.2 (https://github.com/JosebaMuro/Nexus-AI; ejemplo de grafo de conocimiento)";

const std::vector<std::string> RAICES = {
    "Ciencia", "Historia", "Geografía", "Arte", "Tecnología", "Economía", "Filosofía", "Deporte", "Música",
    "Literatura", "Medicina", "Matemáticas", "Biología", "Física", "Química", "Informática", "Derecho", "Política",
    "Gastronomía", "Cine", "Arquitectura", "Astronomía", "Educación", "Religión", "Idiomas", "España",
    "América Latina", "Empresas", "Agricultura", "Medio ambiente"};

// propiedades de Wikidata: entidad -> relacion entre nodos; valor -> atributo del articulo
const std::vector<std::pair<std::string, std::string>> RELACIONES = {
    {"P31", "es_un"}, {"P279", "subclase_de"}, {"P17", "pais"}, {"P131", "situado_en"}, {"P30", "continente"},
    {"P36", "capital"}, {"P106", "ocupacion"}, {"P27", "nacionalidad"}, {"P19", "nacio_en"}, {"P20", "murio_en"},
    {"P361", "parte_de"}, {"P50", "autor"}, {"P170", "creador"}, {"P136", "genero"}, {"P495", "pais_de_origen"},
    {"P69", "estudio_en"}, {"P108", "trabajo_para"}, {"P101", "campo"}, {"P135", "movimiento"},
    {"P138", "nombre_por"}, {"P61", "descubridor"}, {"P176", "fabricante"}, {"P127", "propietario"},
    {"P159", "sede"}, {"P452", "sector"}, {"P37", "idioma_oficial"}, {"P38", "moneda"}, {"P1376", "capital_de"},
    {"P737", "influido_por"}};
const std::vector<std::pair<std::string, std::string>> ATRIBUTOS = {
    {"P1082", "poblacion"}, {"P2046", "superficie_km2"}, {"P2044", "altitud_m"}, {"P569", "nacimiento"},
    {"P570", "fallecimiento"}, {"P571", "fundacion"}, {"P577", "publicacion"}, {"P575", "descubrimiento"},
    {"P585", "fecha"}, {"P2048", "altura_m"}, {"P2067", "masa_kg"}, {"P1128", "empleados"}, {"P856", "web"}};

void pausa(double s) { std::this_thread::sleep_for(std::chrono::milliseconds((int)(s * 1000))); }

json get(const std::string& base, std::map<std::string, std::string> params, int intentos = 8) {
  params["format"] = "json";
  params["formatversion"] = "2";
  // maxlag solo con la Wikipedia: Wikidata va con retraso a menudo y para leer no hace falta
  if (base == WIKI) params["maxlag"] = "5";
  std::string url = base + "?" + http::query_string(params);
  for (int n = 0; n < intentos; ++n) {
    std::string err;
    auto r = http::get(url, {{"User-Agent", UA}, {"Accept-Encoding", "identity"}}, 60);
    if (r.ok()) {
      json data = r.json_body();
      if (data.is_object() && !(data.contains("error") && jstr(data["error"], "code") == "maxlag")) return data;
      err = "maxlag";
    } else {
      err = r.status ? cat("HTTP ", r.status) : r.error.empty() ? std::string("sin conexion") : r.error;
    }
    if (n == intentos - 1) throw Fallo("la API no responde: " + err + " (" + base + ")");
    std::cerr << "  reintento (" << err << ")" << std::endl;   // red o servidor ocupado: espera creciente
    pausa(std::min(60.0, std::pow(2.0, n)));
  }
  return json::object();
}

// Recorre una consulta de la API de MediaWiki siguiendo «continue»
void query_all(std::map<std::string, std::string> params, int limite, const std::function<void(const json&)>& fn) {
  params["action"] = "query";
  std::map<std::string, std::string> cont;
  for (int vistos = 1;; ++vistos) {
    auto p = params;
    for (auto& [k, v] : cont) p[k] = v;
    json data = get(WIKI, p);
    fn(data.value("query", json::object()));
    if (!data.contains("continue") || vistos >= limite) return;
    cont.clear();
    for (auto& [k, v] : data["continue"].items()) cont[k] = v.is_string() ? v.get<std::string>() : v.dump();
    pausa(0.1);
  }
}

// Resumen de una introduccion: sin las formulas en TeX ({\displaystyle ...}, con llaves anidadas) y con los espacios
// normales. La version legible de la formula, que la Wikipedia pone justo antes, se queda.
std::string limpiar(const std::string& texto) {
  std::string out;
  size_t i = 0;
  while (true) {
    size_t j = texto.find("{\\displaystyle", i);
    if (j == std::string::npos) {
      out += texto.substr(i);
      break;
    }
    out += texto.substr(i, j - i);
    int depth = 0;
    size_t k = j;
    while (k < texto.size()) {
      depth += texto[k] == '{' ? 1 : texto[k] == '}' ? -1 : 0;
      ++k;
      if (depth == 0) break;
    }
    i = k;
  }
  return join(split_ws(out), " ");
}

template <class T>
std::vector<std::vector<T>> lotes(const std::vector<T>& v, size_t n) {
  std::vector<std::vector<T>> out;
  for (size_t i = 0; i < v.size(); i += n) out.emplace_back(v.begin() + i, v.begin() + std::min(v.size(), i + n));
  return out;
}

struct Grafo {
  std::vector<std::string> orden;          // nodos en orden de llegada (como un dict de Python)
  std::map<std::string, json> nodos;
  std::vector<std::tuple<std::string, std::string, std::string>> orden_a;
  std::map<std::tuple<std::string, std::string, std::string>, json> aristas;

  bool tiene(const std::string& id) const { return nodos.count(id) > 0; }

  std::string nodo(const std::string& id, const std::string& nombre, const std::string& tipo, const std::string& capa,
                   const json& attrs = json::object()) {
    auto it = nodos.find(id);
    if (it == nodos.end()) {
      orden.push_back(id);
      it = nodos.emplace(id, json{{"id", id}, {"label", nombre}, {"type", tipo}, {"capa", capa}}).first;
    }
    for (auto& [k, v] : attrs.items())
      if (!v.is_null() && !(v.is_string() && v.get<std::string>().empty())) it->second[k] = v;
    return id;
  }

  void arista(const std::string& a, const std::string& b, const std::string& rel, const std::string& capa,
              double peso = 1.0) {
    if (a == b || !tiene(a) || !tiene(b)) return;
    auto key = std::make_tuple(a, b, rel);
    if (aristas.count(key)) return;
    orden_a.push_back(key);
    aristas[key] = {{"source", a}, {"target", b}, {"relation", rel}, {"capa", capa}, {"weight", peso}};
  }
};

std::string wiki_url(const std::string& t) { return "https://es.wikipedia.org/wiki/" + replace_all(t, " ", "_"); }

// Capas 1 y 2: arbol de categorias desde las raices y los articulos de cada una
std::vector<std::string> categorias(Grafo& g, const std::vector<std::string>& raices, size_t por_categoria,
                                    size_t maximo, int profundidad) {
  std::vector<std::string> articulos;
  std::deque<std::tuple<std::string, int, std::string>> cola;
  for (auto& r : raices) cola.emplace_back(r, 0, "");
  std::set<std::string> vistas;
  size_t cupo = std::max<size_t>(1, maximo / std::max<size_t>(1, raices.size()));
  std::map<std::string, size_t> por_raiz;
  while (!cola.empty() && articulos.size() < maximo) {
    auto [cat_, prof, raiz_] = cola.front();
    cola.pop_front();
    if (vistas.count(cat_)) continue;
    vistas.insert(cat_);
    std::string raiz = raiz_.empty() ? cat_ : raiz_;
    std::string cid = g.nodo("cat:" + cat_, cat_, "categoria", "1-temas",
                             {{"url", "https://es.wikipedia.org/wiki/Categoría:" + cat_}});
    std::cerr << "[categoría] " << cat_ << " (nivel " << prof << ")" << std::endl;
    std::vector<std::string> paginas, subcats;
    query_all({{"list", "categorymembers"}, {"cmtitle", "Categoría:" + cat_}, {"cmtype", "page|subcat"},
               {"cmlimit", "500"}},
              2, [&](const json& q) {
                for (auto& m : q.value("categorymembers", json::array())) {
                  int ns = m.value("ns", -1);
                  std::string title = jstr(m, "title");
                  if (ns == 14) subcats.push_back(title.substr(title.find(':') + 1));
                  else if (ns == 0) paginas.push_back(title);
                }
              });
    for (size_t k = 0; k < paginas.size() && k < por_categoria; ++k) {
      const std::string& t = paginas[k];
      if (por_raiz[raiz] >= cupo || articulos.size() >= maximo) break;
      if (!g.tiene("art:" + t)) {
        articulos.push_back(t);
        por_raiz[raiz]++;
      }
      g.nodo("art:" + t, t, "articulo", "2-articulos", {{"url", wiki_url(t)}});
      g.arista("art:" + t, cid, "pertenece_a", "1-temas");
    }
    if (prof < profundidad)
      for (size_t k = 0; k < subcats.size() && k < 12; ++k) {
        const std::string& s = subcats[k];
        g.nodo("cat:" + s, s, "categoria", "1-temas");
        g.arista("cat:" + s, cid, "subcategoria_de", "1-temas");
        cola.emplace_back(s, prof + 1, raiz);
      }
  }
  return articulos;
}

// Capa 2: resumen e identificador de Wikidata de cada articulo. Devuelve {nodo: Qid}
std::vector<std::pair<std::string, std::string>> detalles(Grafo& g, const std::vector<std::string>& articulos) {
  std::vector<std::pair<std::string, std::string>> qids;
  std::set<std::string> con_qid;
  size_t n = 0;
  for (auto& lote : lotes(articulos, 20)) {
    std::cerr << "[artículos] " << n * 20 + lote.size() << "/" << articulos.size() << std::endl;
    ++n;
    query_all({{"prop", "extracts|pageprops|description"}, {"titles", join(lote, "|")}, {"exintro", "1"},
               {"explaintext", "1"}, {"exsentences", "3"}, {"exlimit", "20"}, {"ppprop", "wikibase_item"},
               {"redirects", "1"}},
              5, [&](const json& q) {
                std::vector<std::pair<std::string, std::string>> alias;
                for (auto& r : q.value("redirects", json::array())) alias.emplace_back(jstr(r, "from"), jstr(r, "to"));
                for (auto& [orig, dest] : alias)   // una redireccion: el nodo pasa a llamarse como el articulo real
                  if (g.tiene("art:" + orig) && !g.tiene("art:" + dest)) g.nodos["art:" + orig]["label"] = dest;
                for (auto& p : q.value("pages", json::array())) {
                  if (jget(p, "missing", false)) continue;
                  std::string t = jstr(p, "title"), nid;
                  if (g.tiene("art:" + t)) nid = "art:" + t;
                  else
                    for (auto& [o, d] : alias)
                      if (d == t && g.tiene("art:" + o)) { nid = "art:" + o; break; }
                  if (nid.empty()) continue;
                  g.nodo(nid, t, "articulo", "2-articulos",
                         {{"resumen", uleft(limpiar(jstr(p, "extract")), 900)}, {"descripcion", jstr(p, "description")}});
                  std::string q_id = p.contains("pageprops") ? jstr(p["pageprops"], "wikibase_item") : "";
                  if (!q_id.empty()) {
                    if (!con_qid.count(nid)) {
                      con_qid.insert(nid);
                      qids.emplace_back(nid, q_id);
                    } else {
                      for (auto& e : qids)
                        if (e.first == nid) e.second = q_id;
                    }
                    g.nodos[nid]["wikidata"] = q_id;
                  }
                }
              });
    pausa(0.1);
  }
  return qids;
}

// Capa 3: enlaces entre articulos del grafo
void enlaces(Grafo& g, const std::vector<std::string>& articulos) {
  std::map<std::string, std::string> por_titulo;
  std::vector<std::string> titulos;
  for (auto& t : articulos)
    if (g.tiene("art:" + t)) {
      std::string label = jstr(g.nodos["art:" + t], "label");
      if (!por_titulo.count(label)) titulos.push_back(label);
      por_titulo[label] = "art:" + t;
    }
  size_t n = 0;
  for (auto& lote : lotes(titulos, 50)) {
    std::cerr << "[enlaces] " << n * 50 + lote.size() << "/" << titulos.size() << std::endl;
    ++n;
    query_all({{"prop", "links"}, {"titles", join(lote, "|")}, {"plnamespace", "0"}, {"pllimit", "max"}}, 40,
              [&](const json& q) {
                for (auto& p : q.value("pages", json::array())) {
                  auto a = por_titulo.find(jstr(p, "title"));
                  for (auto& l : p.value("links", json::array())) {
                    auto b = por_titulo.find(jstr(l, "title"));
                    if (a != por_titulo.end() && b != por_titulo.end()) g.arista(a->second, b->second, "enlaza_a", "3-enlaces", 0.5);
                  }
                }
              });
    pausa(0.1);
  }
}

json valor(const json& dv) {
  if (!dv.is_object() || !dv.contains("value")) return nullptr;
  const json& v = dv["value"];
  std::string t = jstr(dv, "type");
  if (t == "quantity") {
    double x = std::stod(jstr(v, "amount"));
    if (std::floor(x) == x && std::fabs(x) < 9e15) return (long long)x;
    return std::round(x * 1000) / 1000;
  }
  if (t == "time") {
    std::string s = ltrim(jstr(v, "time"), "+");
    int prec = jget(v, "precision", 11);
    return prec <= 9 ? s.substr(0, 4) : prec == 10 ? s.substr(0, 7) : s.substr(0, 10);
  }
  if (t == "string") return v;
  if (t == "monolingualtext") return v.value("text", json());
  return nullptr;
}

// Capa 4: hechos de Wikidata (relaciones con otras entidades y atributos)
void wikidata(Grafo& g, const std::vector<std::pair<std::string, std::string>>& qids) {
  std::map<std::string, std::string> por_q;
  std::vector<std::string> lista;
  for (auto& [nid, q] : qids) {
    if (!por_q.count(q)) lista.push_back(q);
    por_q[q] = nid;
  }
  std::vector<std::tuple<std::string, std::string, std::string>> hechos;
  std::vector<std::string> destinos;
  std::set<std::string> vistos;
  size_t n = 0;
  for (auto& lote : lotes(lista, 50)) {
    std::cerr << "[wikidata] " << n * 50 + lote.size() << "/" << lista.size() << std::endl;
    ++n;
    json data = get(WIKIDATA, {{"action", "wbgetentities"}, {"ids", join(lote, "|")}, {"props", "claims"}});
    for (auto& [q, ent] : data.value("entities", json::object()).items()) {
      auto it = por_q.find(q);
      if (it == por_q.end()) continue;
      const std::string& nid = it->second;
      json claims = ent.value("claims", json::object());
      for (auto& [p, rel] : RELACIONES) {
        if (!claims.contains(p)) continue;
        size_t k = 0;
        for (auto& c : claims[p]) {
          if (k++ >= 6) break;
          json dv = c.value("mainsnak", json::object()).value("datavalue", json::object());
          if (jstr(dv, "type") == "wikibase-entityid") {
            std::string id = jstr(dv["value"], "id");
            hechos.emplace_back(nid, rel, id);
            if (!vistos.count(id)) { vistos.insert(id); destinos.push_back(id); }
          }
        }
      }
      for (auto& [p, attr] : ATRIBUTOS) {
        if (!claims.contains(p)) continue;
        std::vector<json> cs, pref;
        for (auto& c : claims[p])
          if (jstr(c, "rank") != "deprecated") cs.push_back(c);
        for (auto& c : cs)
          if (jstr(c, "rank") == "preferred") pref.push_back(c);
        if (pref.empty()) pref = cs;
        if (!pref.empty()) {
          json val = valor(pref.back().value("mainsnak", json::object()).value("datavalue", json::object()));
          if (!val.is_null()) g.nodos[nid][attr] = val;
        }
      }
    }
    pausa(0.2);
  }
  std::map<std::string, std::pair<std::string, std::string>> etiquetas;
  std::vector<std::string> pendientes;
  for (auto& q : destinos)
    if (!por_q.count(q)) pendientes.push_back(q);
  n = 0;
  for (auto& lote : lotes(pendientes, 50)) {
    std::cerr << "[wikidata etiquetas] " << n * 50 + lote.size() << "/" << pendientes.size() << std::endl;
    ++n;
    json data = get(WIKIDATA, {{"action", "wbgetentities"}, {"ids", join(lote, "|")}, {"props", "labels|descriptions"},
                               {"languages", "es"}, {"languagefallback", "1"}});
    for (auto& [q, ent] : data.value("entities", json::object()).items()) {
      std::string lab = jstr(ent.value("labels", json::object()).value("es", json::object()), "value");
      if (!lab.empty())
        etiquetas[q] = {lab, jstr(ent.value("descriptions", json::object()).value("es", json::object()), "value")};
    }
    pausa(0.2);
  }
  for (auto& [nid, rel, q] : hechos) {
    if (por_q.count(q)) {
      g.arista(nid, por_q[q], rel, "4-wikidata");
    } else if (etiquetas.count(q)) {
      auto& [lab, desc] = etiquetas[q];
      std::string dest = g.tiene("art:" + lab) ? "art:" + lab
                                               : g.nodo("wd:" + q, lab, "concepto", "4-wikidata",
                                                        {{"wikidata", q}, {"descripcion", desc},
                                                         {"url", "https://www.wikidata.org/wiki/" + q}});
      g.arista(nid, dest, rel, "4-wikidata");
    }
  }
}

// Quita los conceptos de Wikidata que solo toca un articulo y las categorias vacias: el ejemplo queda legible
void podar(Grafo& g) {
  std::map<std::string, int> grado;
  for (auto& k : g.orden_a) {
    grado[std::get<0>(k)]++;
    grado[std::get<1>(k)]++;
  }
  std::set<std::string> fuera;
  for (auto& [id, d] : g.nodos) {
    std::string type = jstr(d, "type");
    if ((type == "concepto" && grado[id] < 2) || (type == "categoria" && grado[id] == 0)) fuera.insert(id);
  }
  for (auto& id : fuera) g.nodos.erase(id);
  std::vector<std::string> orden;
  for (auto& id : g.orden)
    if (!fuera.count(id)) orden.push_back(id);
  g.orden = orden;
  decltype(g.orden_a) oa;
  for (auto& k : g.orden_a)
    if (g.tiene(std::get<0>(k)) && g.tiene(std::get<1>(k))) oa.push_back(k);
    else g.aristas.erase(k);
  g.orden_a = oa;
}

std::string xesc(const std::string& s, bool attr = false) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (attr && c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

std::string texto(const json& v) {   // str() de Python
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_float()) return pyfloat(v.get<double>());
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  return v.dump();
}

void guardar(Grafo& g, const fs::path& salida, const json& meta) {
  fs::create_directories(salida);
  std::set<std::string> claves_s;
  for (auto& id : g.orden)
    for (auto& [k, v] : g.nodos[id].items())
      if (k != "id") claves_s.insert(k);
  std::vector<std::string> claves(claves_s.begin(), claves_s.end());
  std::vector<std::string> out = {R"(<?xml version="1.0" encoding="UTF-8"?>)",
                                  R"(<graphml xmlns="http://graphml.graphdrawing.org/xmlns">)",
                                  "  <!-- " + xesc(jstr(meta, "fuente")) + " -->"};
  for (auto& k : claves) {
    bool all_int = true, all_num = true, any = false;
    for (auto& id : g.orden) {
      auto& n = g.nodos[id];
      if (!n.contains(k)) continue;
      any = true;
      all_int = all_int && n[k].is_number_integer();
      all_num = all_num && n[k].is_number();
    }
    std::string tipo = any && all_int ? "long" : any && all_num ? "double" : "string";
    out.push_back("  <key id=\"" + k + "\" for=\"node\" attr.name=\"" + k + "\" attr.type=\"" + tipo + "\"/>");
  }
  out.push_back(R"(  <key id="relation" for="edge" attr.name="relation" attr.type="string"/>)");
  out.push_back(R"(  <key id="ecapa" for="edge" attr.name="capa" attr.type="string"/>)");
  out.push_back(R"(  <key id="weight" for="edge" attr.name="weight" attr.type="double"/>)");
  out.push_back(R"(  <graph id="wikipedia-es" edgedefault="directed">)");
  json nodes = json::array(), links = json::array();
  for (auto& id : g.orden) {
    auto& n = g.nodos[id];
    nodes.push_back(n);
    out.push_back("    <node id=\"" + xesc(id, true) + "\">");
    for (auto& k : claves)
      if (n.contains(k)) out.push_back("      <data key=\"" + k + "\">" + xesc(texto(n[k])) + "</data>");
    out.push_back("    </node>");
  }
  for (auto& key : g.orden_a) {
    auto& e = g.aristas[key];
    links.push_back(e);
    out.push_back("    <edge source=\"" + xesc(jstr(e, "source"), true) + "\" target=\"" + xesc(jstr(e, "target"), true) +
                  "\"><data key=\"relation\">" + jstr(e, "relation") + "</data><data key=\"ecapa\">" + jstr(e, "capa") +
                  "</data><data key=\"weight\">" + texto(e["weight"]) + "</data></edge>");
  }
  out.push_back("  </graph>");
  out.push_back("</graphml>");
  escribir(salida / "grafo.graphml", join(out, "\n") + "\n");
  json nl = {{"directed", true}, {"multigraph", false}, {"graph", meta}, {"nodes", nodes}, {"links", links}};
  escribir(salida / "grafo.json", jdump(nl, 1) + "\n");

  std::map<std::string, int> capas_n, capas_e;
  std::vector<std::pair<std::string, int>> rels;
  for (auto& id : g.orden) capas_n[jstr(g.nodos[id], "capa")]++;
  for (auto& key : g.orden_a) {
    auto& e = g.aristas[key];
    capas_e[jstr(e, "capa")]++;
    std::string r = jstr(e, "relation");
    auto it = std::find_if(rels.begin(), rels.end(), [&](auto& p) { return p.first == r; });
    if (it == rels.end()) rels.emplace_back(r, 1);
    else it->second++;
  }
  std::stable_sort(rels.begin(), rels.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if (rels.size() > 15) rels.resize(15);
  std::vector<std::string> raices;
  for (auto& r : meta["raices"]) raices.push_back(r.get<std::string>());
  std::vector<std::string> lee = {
      "# Grafo multicapa de la Wikipedia en español", "",
      "Generado el " + jstr(meta, "fecha") +
          " con `nexus-construir grafo-wikipedia` desde la API pública de es.wikipedia.org y wikidata.org. Es un "
          "ejemplo de grafo de conocimiento para importar en Nexus-Office (**Ajustes > Gestor de conocimiento > "
          "Importar / exportar**, o `graph_import` desde el chat).",
      "",
      cat("- **", g.orden.size(), " nodos** y **", g.orden_a.size(), " relaciones**."),
      "- Raíces temáticas: " + join(raices, ", ") + ".", "",
      "| Capa | Qué contiene | Nodos | Relaciones |", "|---|---|---|---|"};
  std::vector<std::pair<std::string, std::string>> desc = {
      {"1-temas", "categorías de la Wikipedia y su jerarquía; cada artículo `pertenece_a` sus categorías"},
      {"2-articulos", "artículos, con el resumen de su introducción y su descripción"},
      {"3-enlaces", "enlaces `enlaza_a` entre artículos del grafo"},
      {"4-wikidata", "hechos de Wikidata: `es_un`, `pais`, `situado_en`, `autor`... y atributos como población o fechas"}};
  for (auto& [c, d] : desc) lee.push_back(cat("| `", c, "` | ", d, " | ", capas_n[c], " | ", capas_e[c], " |"));
  std::vector<std::string> top;
  for (auto& [r, k] : rels) top.push_back(cat("`", r, "` (", k, ")"));
  for (std::string s : {std::string(""), "Relaciones más frecuentes: " + join(top, ", ") + ".", std::string(""),
                        std::string("## Ficheros"),
                        std::string("- `grafo.graphml`: GraphML con los atributos `capa`, `type`, `resumen`, `url`, "
                                    "`wikidata`... Se abre en Gephi o yEd y se importa en Nexus-Office."),
                        std::string("- `grafo.json`: el mismo grafo en JSON node-link (D3, NetworkX, sigma.js)."),
                        std::string(""), std::string("## Cómo importarlo"), std::string("```"),
                        std::string("nexus-office grafo importar grafo.graphml --en auto"), std::string("```"),
                        std::string("Con `--en auto`, lo común va al grafo general y lo especializado al agente que "
                                    "corresponde. Los atributos de cada nodo (resumen, población, fechas) quedan en su "
                                    "nota y los agentes los usan al responder."),
                        std::string(""), std::string("## Licencia"),
                        std::string("Textos de la Wikipedia en español bajo [CC BY-SA 4.0]"
                                    "(https://creativecommons.org/licenses/by-sa/4.0/deed.es) (autores: colaboradores de "
                                    "la Wikipedia; cada nodo lleva su `url`). Datos de Wikidata bajo CC0."),
                        std::string("")})
    lee.push_back(s);
  escribir(salida / "LEEME.md", join(lee, "\n"));
  std::cerr << "[ok] " << g.orden.size() << " nodos, " << g.orden_a.size() << " relaciones en " << S(salida) << std::endl;
}

}  // namespace

int grafo_wikipedia(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {}, {});
  fs::path salida = P(a.val("salida", "ejemplos/grafo-wikipedia-es"));
  size_t articulos = std::stoul(a.val("articulos", "1500"));
  size_t por_categoria = std::stoul(a.val("por-categoria", "12"));
  int profundidad = std::stoi(a.val("profundidad", "1"));
  std::vector<std::string> raices;
  if (a.tiene("raices")) {
    for (auto& r : split(a.val("raices"), ","))
      if (!trim(r).empty()) raices.push_back(trim(r));
  } else {
    raices = RAICES;
  }
  Grafo g;
  auto arts = categorias(g, raices, por_categoria, articulos, profundidad);
  auto qids = detalles(g, arts);
  enlaces(g, arts);
  wikidata(g, qids);
  podar(g);
  json meta = {{"name", "Wikipedia en español (multicapa)"}, {"fecha", strftime_local("%Y-%m-%d")}, {"raices", raices},
               {"fuente", "es.wikipedia.org (CC BY-SA 4.0) y wikidata.org (CC0)"},
               {"capas", {"1-temas", "2-articulos", "3-enlaces", "4-wikidata"}}};
  guardar(g, salida, meta);
  return 0;
}

}  // namespace nc
