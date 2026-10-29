// Hilo de las conversaciones largas (ver hilo.h): resumen progresivo, notas fijas, recuperacion de lo antiguo y
// presupuesto de tokens por turno.
#include "memory/hilo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"
#include "util/crypto.h"

namespace nx::mem {

static const auto L = log::get("hilo");

int Hilo::tokens(const std::string& s) { return (int)std::ceil(ulen(s) / 3.2) + 4; }

std::string Hilo::recortar(const std::string& s, int max_tokens) {
  if (tokens(s) <= max_tokens) return s;
  size_t chars = (size_t)std::max(40.0, (max_tokens - 8) * 3.2);
  size_t head = chars * 2 / 3, tail = chars - head;
  return trim(uleft(s, head)) + "\n[…]\n" + trim(uright(s, tail));
}

std::string HiloPlan::texto() const {
  std::string out;
  if (!notas.empty())
    out += "## Notas fijas del proyecto (las escribio el usuario: tenlas siempre presentes)\n" + notas + "\n\n";
  if (!resumen.empty())
    out += "## Hilo de esta conversacion (resumen de " + std::to_string(plegados) + " mensajes anteriores)\n" + resumen +
           "\n\n";
  if (!recuerdos.empty()) {
    out += "## Fragmentos anteriores de esta conversacion relacionados con la pregunta\n";
    for (auto& r : recuerdos) out += "- " + r + "\n";
    out += "\n";
  }
  if (!out.empty())
    out += "Mantén la continuidad del proyecto: respeta lo ya decidido, usa los datos anteriores y retoma lo pendiente. "
           "Si algo de lo hablado antes no aparece aqui, dilo en vez de inventarlo.";
  return out;
}

Hilo::Hilo(DB& db, std::function<bool()> busy) : db_(db), busy_(std::move(busy)) {
  db_.script(R"SQL(
CREATE TABLE IF NOT EXISTS hilos (
    session_id TEXT PRIMARY KEY, resumen TEXT NOT NULL DEFAULT '', notas TEXT NOT NULL DEFAULT '',
    hasta INTEGER NOT NULL DEFAULT 0, plegados INTEGER NOT NULL DEFAULT 0, updated REAL);
)SQL");
  th_ = std::thread([this] { loop(); });
}

Hilo::~Hilo() {
  {
    std::lock_guard<std::mutex> lk(qmu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

Row Hilo::fila(const std::string& sid) {
  auto r = db_.one("SELECT * FROM hilos WHERE session_id=?", {sid});
  if (r) return *r;
  return json{{"session_id", sid}, {"resumen", ""}, {"notas", ""}, {"hasta", 0}, {"plegados", 0}};
}

static long long jll(const json& r, const char* k) {
  auto v = r.value(k, json());
  return v.is_number() ? v.get<long long>() : 0;
}

static std::string linea(const json& m, int max_tokens) {
  std::string who = jstr(m, "role") == "user" ? "Usuario" : "Asistente";
  return who + ": " + Hilo::recortar(trim(jstr(m, "content")), max_tokens);
}

HiloPlan Hilo::plan(const std::string& sid, const std::string& query, int budget) {
  HiloPlan p;
  budget = std::max(budget, 160);
  Row r = fila(sid);
  long long hasta = jll(r, "hasta");
  p.plegados = jll(r, "plegados");
  if (!trim(jstr(r, "notas")).empty()) p.notas = recortar(trim(jstr(r, "notas")), std::max(60, budget * 15 / 100));
  if (!trim(jstr(r, "resumen")).empty())
    p.resumen = recortar(trim(jstr(r, "resumen")), std::max(80, budget * 30 / 100));
  int fixed = (p.notas.empty() ? 0 : tokens(p.notas)) + (p.resumen.empty() ? 0 : tokens(p.resumen));
  int rest = std::max(120, budget - fixed);

  // lo reciente, literal, de lo mas nuevo hacia atras; lo ya plegado solo entra como resumen o recuerdo
  auto rows = db_.query(
      "SELECT id, role, content FROM messages WHERE session_id=? AND id>? AND role IN ('user','assistant') "
      "ORDER BY id DESC LIMIT 400",
      {sid, hasta});
  bool older = hasta > 0 || rows.size() == 400;
  int recent_budget = older || rows.size() > 6 ? rest * 75 / 100 : rest;
  int per_msg = std::max(200, recent_budget / 3);
  int used = 0;
  long long first_id = 0;
  std::vector<json> picked;
  for (auto& m : rows) {
    std::string c = trim(jstr(m, "content"));
    if (c.empty()) continue;
    std::string cut = recortar(c, per_msg);
    int t = tokens(cut);
    if (used + t > recent_budget) {
      if (picked.empty()) {   // el ultimo mensaje siempre entra, aunque sea recortado
        cut = recortar(c, std::max(60, recent_budget - 8));
        t = tokens(cut);
      } else {
        break;
      }
    }
    picked.push_back({{"role", m["role"]}, {"content", cut}});
    used += t;
    first_id = jll(m, "id");
  }
  std::reverse(picked.begin(), picked.end());
  for (auto& m : picked) p.recientes.push_back(m);

  // lo que no entra literal: recuperar los trozos que tienen que ver con la pregunta
  long long limit_id = first_id ? first_id : std::numeric_limits<long long>::max();
  p.omitidos = db_.scalar("SELECT COUNT(*) FROM messages WHERE session_id=? AND id<? AND role IN ('user','assistant')",
                          {sid, limit_id})
                   .get<long long>();
  int recall_budget = rest - used;
  if (p.omitidos > 0 && recall_budget > 60 && !trim(query).empty()) {
    auto old = db_.query(
        "SELECT id, role, content FROM messages WHERE session_id=? AND id<? AND role IN ('user','assistant') "
        "ORDER BY id DESC LIMIT 300",
        {sid, limit_id});
    struct Trozo {
      long long id;
      size_t idx;
      std::string text;
      float score;
    };
    std::vector<Trozo> trozos;
    Vec q = fast_.one(query);
    for (auto& m : old) {
      std::string who = jstr(m, "role") == "user" ? "Usuario: " : "Asistente: ";
      auto parts = chunks(jstr(m, "content"), 500);
      for (size_t i = 0; i < parts.size() && i < 12; ++i)
        trozos.push_back({jll(m, "id"), i, who + trim(parts[i]), dot(q, fast_.one(parts[i]))});
    }
    std::sort(trozos.begin(), trozos.end(), [](const Trozo& a, const Trozo& b) { return a.score > b.score; });
    std::vector<Trozo> sel;
    int rt = 0;
    for (auto& t : trozos) {
      if (t.score < 0.12f || sel.size() >= 6) break;
      int k = tokens(t.text);
      if (rt + k > recall_budget) continue;
      sel.push_back(t);
      rt += k;
    }
    std::sort(sel.begin(), sel.end(), [](const Trozo& a, const Trozo& b) {
      return a.id != b.id ? a.id < b.id : a.idx < b.idx;
    });
    for (auto& t : sel) p.recuerdos.push_back(t.text);
    used += rt;
  }
  p.tokens = fixed + used;
  return p;
}

std::string resumen_extractivo(const std::string& previo, const json& mensajes, int resumen_tokens) {
  static const Rx sentence(R"(^(.+?[.!?])(\s|$))", Rx::S);
  std::vector<std::string> lines;
  for (auto& l : splitlines(previo))
    if (!trim(l).empty()) lines.push_back(trim(l));
  std::vector<std::string> users;
  for (auto& m : mensajes) {
    std::string c = trim(jstr(m, "content"));
    if (c.empty()) continue;
    std::string first = c;
    if (auto mm = sentence.search(c)) first = mm->str(1);
    first = uleft(first, 180);
    for (auto& ch : first)
      if (ch == '\n') ch = ' ';
    bool user = jstr(m, "role") == "user";
    if (user) users.push_back(c);
    lines.push_back(std::string(user ? "- Pidio: " : "- Respuesta: ") + first);
  }
  if (!users.empty()) {
    auto kw = KnowledgeGraph::keywords(join(users, " "), 6);
    if (!kw.empty()) lines.push_back("- Temas: " + join(kw, ", "));
  }
  // si no cabe: se conservan las dos primeras lineas (el arranque del proyecto) y lo mas reciente
  auto total = [&] { return Hilo::tokens(join(lines, "\n")); };
  while (total() > resumen_tokens && lines.size() > 3) lines.erase(lines.begin() + 2);
  return Hilo::recortar(join(lines, "\n"), resumen_tokens);
}

std::string Hilo::resumir(const std::string& previo, const std::string& nuevos, int resumen_tokens,
                          const Completar& llm) {
  if (!llm) return "";
  int words = std::max(60, resumen_tokens * 6 / 10);
  std::string prompt =
      "Eres el secretario de un proyecto. Actualiza el RESUMEN DEL HILO con los mensajes nuevos de la conversacion. "
      "Conserva: el objetivo del proyecto, las decisiones tomadas, los datos concretos (nombres, cifras, fechas, "
      "rutas, versiones, codigo clave), lo ya hecho y lo pendiente. Quita lo repetido o superado; no inventes nada. "
      "Escribe en espanol, en vinetas breves agrupadas en: Objetivo, Decisiones, Datos, Hecho, Pendiente. Maximo " +
      std::to_string(words) + " palabras. Responde SOLO con el resumen.\n\nRESUMEN ACTUAL:\n" +
      (trim(previo).empty() ? "(vacio)" : previo) + "\n\nMENSAJES NUEVOS:\n" + nuevos + "\n\nRESUMEN ACTUALIZADO:\n";
  try {
    static const Rx think(R"(<think>.*?</think>)", Rx::S | Rx::I);
    std::string out = trim(think.sub(llm(prompt, resumen_tokens + 64), ""));
    if (ulen(out) < 20) return "";
    return recortar(out, resumen_tokens * 13 / 10);
  } catch (const std::exception& e) {
    L.warning("Resumen del hilo con LLM fallido: ", e.what());
    return "";
  }
}

int Hilo::plegar(const std::string& sid, int recent_budget, int resumen_tokens, const Completar& llm) {
  std::lock_guard<std::mutex> lk(mu_);
  recent_budget = std::max(recent_budget, 200);
  resumen_tokens = std::max(resumen_tokens, 80);
  Row r = fila(sid);
  long long hasta = jll(r, "hasta");
  auto rows = db_.query(
      "SELECT id, role, content FROM messages WHERE session_id=? AND id>? AND role IN ('user','assistant') ORDER BY id",
      {sid, hasta});
  int per_msg = std::max(200, recent_budget / 3);
  std::vector<int> cost;
  int total = 0;
  for (auto& m : rows) {
    cost.push_back(tokens(recortar(trim(jstr(m, "content")), per_msg)));
    total += cost.back();
  }
  if (total <= recent_budget || rows.size() <= 2) return 0;
  // se pliega hasta dejar libre casi dos tercios del presupuesto: menos llamadas al LLM y en tandas grandes
  int target = recent_budget * 35 / 100;
  size_t n = 0;
  int left = total;
  while (n + 2 < rows.size() && left > target) left -= cost[n++];
  std::string resumen = trim(jstr(r, "resumen"));
  long long plegados = jll(r, "plegados");
  int batch_max = std::max(600, resumen_tokens * 3);
  size_t i = 0;
  int hechos = 0;
  while (i < n) {
    json lote = json::array();
    std::vector<std::string> lines;
    int bt = 0;
    long long last = hasta;
    while (i < n) {
      std::string l = linea(rows[i], std::max(150, resumen_tokens));
      int t = tokens(l);
      if (!lines.empty() && bt + t > batch_max) break;
      lines.push_back(l);
      lote.push_back(rows[i]);
      bt += t;
      last = jll(rows[i], "id");
      ++i;
    }
    std::string nuevo = llm && llm_real() ? resumir(resumen, join(lines, "\n"), resumen_tokens, llm) : "";
    if (nuevo.empty()) nuevo = resumen_extractivo(resumen, lote, resumen_tokens);
    resumen = nuevo;
    plegados += (long long)lote.size();
    hechos += (int)lote.size();
    db_.execute(
        "INSERT INTO hilos(session_id, resumen, hasta, plegados, updated) VALUES(?,?,?,?,?) "
        "ON CONFLICT(session_id) DO UPDATE SET resumen=excluded.resumen, hasta=excluded.hasta, "
        "plegados=excluded.plegados, updated=excluded.updated",
        {sid, resumen, last, plegados, now()});
  }
  L.info("Hilo ", sid, ": ", hechos, " mensajes plegados en el resumen (", tokens(resumen), " tokens)");
  return hechos;
}

void Hilo::encolar(const std::string& sid, int recent_budget, int resumen_tokens) {
  {
    std::lock_guard<std::mutex> lk(qmu_);
    for (auto& t : cola_)
      if (t.sid == sid) {
        t.recent = recent_budget;
        t.resumen = resumen_tokens;
        return;
      }
    cola_.push_back({sid, recent_budget, resumen_tokens});
  }
  cv_.notify_all();
}

void Hilo::loop() {
  for (;;) {
    Tarea t;
    {
      std::unique_lock<std::mutex> lk(qmu_);
      cv_.wait(lk, [this] { return stop_ || !cola_.empty(); });
      if (stop_) return;
      t = cola_.front();
      cola_.pop_front();
    }
    // el resumen usa el mismo LLM: se espera a que termine de responder (como mucho 2 minutos)
    for (int k = 0; k < 240 && busy_ && busy_(); ++k) {
      std::unique_lock<std::mutex> lk(qmu_);
      if (cv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return stop_; })) return;
    }
    try {
      plegar(t.sid, t.recent, t.resumen, llm_complete);
    } catch (const std::exception& e) {
      L.warning("No se pudo plegar el hilo ", t.sid, ": ", e.what());
    }
  }
}

json Hilo::estado(const std::string& sid) {
  Row r = fila(sid);
  long long total =
      db_.scalar("SELECT COUNT(*) FROM messages WHERE session_id=? AND role IN ('user','assistant')", {sid})
          .get<long long>();
  return {{"session_id", sid},
          {"resumen", jstr(r, "resumen")},
          {"notas", jstr(r, "notas")},
          {"plegados", jll(r, "plegados")},
          {"mensajes", total},
          {"resumen_tokens", tokens(jstr(r, "resumen"))},
          {"notas_tokens", tokens(jstr(r, "notas"))}};
}

void Hilo::guardar(const std::string& sid, const json& cambios) {
  std::lock_guard<std::mutex> lk(mu_);
  Row r = fila(sid);
  std::string resumen = cambios.contains("resumen") ? jstr(cambios, "resumen") : jstr(r, "resumen");
  std::string notas = cambios.contains("notas") ? jstr(cambios, "notas") : jstr(r, "notas");
  db_.execute(
      "INSERT INTO hilos(session_id, resumen, notas, hasta, plegados, updated) VALUES(?,?,?,?,?,?) "
      "ON CONFLICT(session_id) DO UPDATE SET resumen=excluded.resumen, notas=excluded.notas, updated=excluded.updated",
      {sid, resumen, notas, jll(r, "hasta"), jll(r, "plegados"), now()});
}

void Hilo::olvidar(const std::string& sid) {
  std::lock_guard<std::mutex> lk(mu_);
  db_.execute("DELETE FROM hilos WHERE session_id=?", {sid});
}

}  // namespace nx::mem
