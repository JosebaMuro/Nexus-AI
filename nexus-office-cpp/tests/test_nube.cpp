// Agente Profesor e IA online (llm/cloud) contra un servidor falso (tests/test_nube.py): API compatible con OpenAI
// y Messages API de Anthropic en streaming, claves secretas, respaldo local sin clave y consultar_profesor.
#include <httplib.h>

#include <mutex>
#include <thread>

#include "agents/agents.h"
#include "check.h"
#include "core/config.h"
#include "core/services.h"
#include "llm/llm.h"
#include "memory/memory.h"
#include "server/app.h"
#include "tools/tools.h"
#include "util/fs.h"
#include "util/net.h"
#include "util/text.h"

using namespace nx;

namespace {

std::mutex seen_mu;
std::vector<json> seen;

json last_seen() {
  std::lock_guard<std::mutex> lk(seen_mu);
  return seen.empty() ? json() : seen.back();
}

void sse(httplib::Response& res, std::vector<std::string> events) {
  auto evs = std::make_shared<std::vector<std::string>>(std::move(events));
  res.set_chunked_content_provider("text/event-stream", [evs](size_t, httplib::DataSink& sink) {
    for (auto& e : *evs) sink.write(e.data(), e.size());
    sink.done();
    return true;
  });
}

struct Fake {
  httplib::Server srv;
  std::thread th;
  int port = 0;
  std::string url;
  Fake() {
    srv.Post(R"(/.*)", [](const httplib::Request& req, httplib::Response& res) {
      json body = jparse(req.body);
      json h = json::object();
      for (auto& [k, v] : req.headers) h[lower(k)] = v;
      {
        std::lock_guard<std::mutex> lk(seen_mu);
        seen.push_back({{"path", req.path}, {"headers", h}, {"body", body}});
      }
      if (contains(req.get_header_value("Authorization") + req.get_header_value("x-api-key"), "mala")) {
        res.status = 401;
        res.set_content(R"({"error": {"type": "authentication_error", "message": "invalid x-api-key"}})",
                        "application/json");
        return;
      }
      if (ends_with(req.path, "/chat/completions")) {
        sse(res, {"data: {\"choices\": [{\"delta\": {\"content\": \"Hola \"}}]}\n\n",
                  "data: {\"choices\": [{\"delta\": {\"content\": \"alumno\"}}], \"citations\": "
                  "[\"https://es.wikipedia.org/x\"]}\n\n",
                  "data: [DONE]\n\n"});
      } else if (ends_with(req.path, "/v1/messages")) {
        json msg = {{"id", "msg_1"}, {"type", "message"}, {"role", "assistant"}, {"model", body["model"]},
                    {"content", json::array()}, {"stop_reason", nullptr}, {"usage", {{"input_tokens", 5}}}};
        std::vector<std::pair<std::string, json>> evs = {
            {"message_start", {{"type", "message_start"}, {"message", msg}}},
            {"content_block_start", {{"type", "content_block_start"}, {"index", 0},
                                     {"content_block", {{"type", "thinking"}, {"thinking", ""}}}}},
            {"content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                                     {"delta", {{"type", "thinking_delta"}, {"thinking", "pienso"}}}}},
            {"content_block_stop", {{"type", "content_block_stop"}, {"index", 0}}},
            {"content_block_start", {{"type", "content_block_start"}, {"index", 1},
                                     {"content_block", {{"type", "text"}, {"text", ""}}}}},
            {"content_block_delta", {{"type", "content_block_delta"}, {"index", 1},
                                     {"delta", {{"type", "text_delta"}, {"text", "Soy Claude"}}}}},
            {"content_block_stop", {{"type", "content_block_stop"}, {"index", 1}}},
            {"message_delta", {{"type", "message_delta"}, {"delta", {{"stop_reason", "end_turn"}}},
                               {"usage", {{"output_tokens", 3}}}}},
            {"message_stop", {{"type", "message_stop"}}}};
        std::vector<std::string> out;
        for (auto& [n, d] : evs) out.push_back("event: " + n + "\ndata: " + d.dump() + "\n\n");
        sse(res, out);
      } else {
        res.status = 404;
      }
    });
    port = net::free_port(18990);
    url = "http://127.0.0.1:" + std::to_string(port);
    th = std::thread([this] { srv.listen("127.0.0.1", port); });
    srv.wait_until_ready();
    auto& o = llm::CloudBackend::base_override();
    o["gemini"] = url + "/v1beta/openai";
    o["perplexity"] = url;
    o["claude"] = url;
  }
  ~Fake() {
    llm::CloudBackend::base_override().clear();
    srv.stop();
    th.join();
  }
};

struct Env {
  std::unique_ptr<Services> s;
  Env() {
    auto dir = std::filesystem::temp_directory_path() / ("nexus-nube-" + std::to_string(::time(nullptr)));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    set_env("NEXUS_DATA_DIR", S(dir));
    reset_config();
    s = std::make_unique<Services>(false);
  }
  ~Env() { s->shutdown(); }
};

std::string stream(Services& s, const json& msgs, const std::string& spec, llm::ReasonFn on_reasoning = nullptr) {
  std::string acc;
  llm::Opts o;
  o.on_reasoning = std::move(on_reasoning);
  s.llm->stream(msgs, spec, o, [&](const std::string& p) { acc += p; });
  return acc;
}

json body(const httplib::Result& r) { return r ? jparse(r->body) : json(); }

}  // namespace

TEST("nube: el Profesor sin clave usa el modelo local y nunca es respaldo automatico") {
  Env e;
  Services& s = *e.s;
  CHECK(s.agents->get("profesor").has_value());
  CHECK(!s.llm->backend("nube")->available());
  CHECK(s.llm->select("nube:").first != "nube");
  CHECK(s.llm->select("default").first != "nube");
  CHECK_EQ(s.agents->route("enséñame a calcular el IVA de una factura"), std::string("profesor"));
}

TEST("nube: API compatible con OpenAI, citas de Perplexity y Claude con pensamiento adaptativo") {
  Fake f;
  Env e;
  Services& s = *e.s;
  s.config.set("nube.proveedor", "perplexity");
  s.config.set("nube.clave_perplexity", "pplx-1");
  CHECK(s.llm->select("nube:") == std::make_pair(std::string("nube"), std::string("")));
  std::string out = stream(s, json::parse(R"([{"role":"system","content":"sé breve"},{"role":"user","content":"hola"}])"),
                           "nube:");
  CHECK(starts_with(out, "Hola alumno") && contains(out, "https://es.wikipedia.org/x"));
  json req = last_seen();
  CHECK_EQ(jstr(req["headers"], "authorization"), std::string("Bearer pplx-1"));
  CHECK_EQ(jstr(req["body"], "model"), std::string("sonar"));
  CHECK(req["body"]["stream"] == true && req["body"]["messages"][0]["role"] == "system");
  s.config.set("nube.clave_gemini", "g-1");
  stream(s, json::parse(R"([{"role":"user","content":"hola"}])"), "nube:gemini:gemini-2.5-pro");
  req = last_seen();
  CHECK_EQ(jstr(req, "path"), std::string("/v1beta/openai/chat/completions"));
  CHECK_EQ(jstr(req["body"], "model"), std::string("gemini-2.5-pro"));

  s.config.set("nube.proveedor", "claude");
  s.config.set("nube.clave_claude", "sk-ant-1");
  std::string thoughts;
  out = stream(s, json::parse(R"([{"role":"system","content":"eres profesor"},{"role":"user","content":"hola"}])"),
               "nube:", [&](const std::string& t) { thoughts += t; });
  CHECK_EQ(out, std::string("Soy Claude"));
  CHECK(contains(thoughts, "pienso"));
  req = last_seen();
  CHECK_EQ(jstr(req, "path"), std::string("/v1/messages"));
  CHECK_EQ(jstr(req["headers"], "x-api-key"), std::string("sk-ant-1"));
  CHECK_EQ(jstr(req["body"], "model"), std::string("claude-opus-5-5"));
  CHECK_EQ(jstr(req["body"], "system"), std::string("eres profesor"));
  CHECK(req["body"]["thinking"] == json({{"type", "adaptive"}, {"display", "summarized"}}));
  CHECK(req["body"]["output_config"] == json({{"effort", "medium"}}));
  CHECK_EQ(jstr(req["body"], "fallbacks"), std::string("default"));
  CHECK(contains(jstr(req["headers"], "anthropic-beta"), "server-side-fallback-2026-07-01"));
  CHECK(!req["body"].contains("temperature"));
}

TEST("nube: /api/nube guarda las claves en secreto y prueba la conexion") {
  Fake f;
  Env e;
  Services& s = *e.s;
  server::App app(&s);
  int port = net::free_port(18995);
  std::thread th([&] { app.listen("127.0.0.1", port); });
  httplib::Client c("127.0.0.1", port);
  c.set_read_timeout(60);
  for (int i = 0; i < 100 && !c.Get("/api/ping"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  httplib::Headers h = {{"X-Admin-Token", jstr(body(c.Post("/api/admin/setup", R"({"pin":"1234"})", "application/json")),
                                               "token")}};
  auto bad = c.Put("/api/nube", h, R"({"proveedor":"x"})", "application/json");
  CHECK(bad && bad->status == 400);
  auto anon = c.Put("/api/nube", R"({"proveedor":"claude","clave":"k"})", "application/json");
  CHECK(anon && (anon->status == 401 || anon->status == 403));
  json st = body(c.Put("/api/nube", h, R"({"proveedor":"claude","clave":"sk-ant-9"})", "application/json"));
  bool con_clave = false;
  for (auto& p : st["proveedores"]) con_clave |= p["id"] == "claude" && truthy(p["con_clave"]);
  CHECK(truthy(st["listo"]) && con_clave);
  CHECK(!contains(c.Get("/api/nube")->body, "sk-ant-9"));
  CHECK_EQ(jstr(body(c.Get("/api/config", h))["nube"], "clave_claude"), std::string("********"));
  c.Put("/api/config", h, R"({"nube.clave_claude":"********"})", "application/json");
  CHECK_EQ(s.config.str("nube.clave_claude"), std::string("sk-ant-9"));
  json r = body(c.Post("/api/nube/probar", h, "{}", "application/json"));
  CHECK(truthy(r["ok"]) && jstr(r, "respuesta") == "Soy Claude");
  s.config.set("nube.clave_claude", "mala");
  r = body(c.Post("/api/nube/probar", h, "{}", "application/json"));
  CHECK(!truthy(r["ok"]) && contains(jstr(r, "error"), "clave"));
  st = body(c.Put("/api/nube", h, R"({"borrar_claves":["claude"]})", "application/json"));
  CHECK(!truthy(st["listo"]));
  app.stop();
  th.join();
}

TEST("nube: consultar_profesor guarda la leccion en la memoria del agente que pregunta") {
  Fake f;
  Env e;
  Services& s = *e.s;
  s.config.set("nube.proveedor", "perplexity");
  s.config.set("nube.clave_perplexity", "pplx-1");
  tools::ToolContext ctx;
  ctx.services = &s;
  ctx.agent = "investigador";
  ctx.session_id = s.memory->new_session();
  json r = s.supervisor->execute("consultar_profesor", {{"pregunta", "¿qué es el IVA?"}}, ctx);
  json res = r.contains("result") ? r["result"] : r;
  CHECK(truthy(res["online"]) && contains(jstr(res, "answer"), "Hola alumno") && truthy(res["memoria"]));
  auto mem = s.memory->list("leccion", 100, 0, std::string("investigador"));
  CHECK(!mem.empty() && contains(jstr(mem[0], "content"), "IVA"));
}
