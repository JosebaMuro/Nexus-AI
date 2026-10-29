// Motor LiteRT-LM (Google) por su API C (c/engine.h, c/conversation.h, version 1.x), cargada al ejecutar.
//
// Una conversacion de LiteRT-LM guarda su cache KV: si la peticion nueva es la anterior mas la respuesta y un mensaje
// nuevo (lo normal en un chat), se reutiliza y solo se procesa el mensaje nuevo; si no, se crea otra con el historial.
#include <condition_variable>
#include <deque>
#include <mutex>

#include "llm/motores.h"
#include "util/crypto.h"
#include "util/dynlib.h"
#include "util/log.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::motores {

namespace {

const auto L = log::get("litert");

// tipos opacos de la API C
struct Engine;
struct EngineSettings;
struct SessionConfig;
struct SamplerParams;
struct ConvConfig;
struct Conversation;
struct StreamChunk;
using Status = int;
using StreamCb = void (*)(void*, const StreamChunk*);

struct Api {
  DynLib lib;
  Status (*get_api_version)(int*, int*, int*) = nullptr;
  Status (*set_min_log_level)(int) = nullptr;
  const char* (*last_error)() = nullptr;
  Status (*settings_create)(const char*, const char*, const char*, const char*, EngineSettings**) = nullptr;
  void (*settings_delete)(EngineSettings*) = nullptr;
  Status (*settings_max_tokens)(EngineSettings*, int) = nullptr;
  Status (*settings_threads)(EngineSettings*, int) = nullptr;
  Status (*settings_cache_dir)(EngineSettings*, const char*) = nullptr;
  Status (*settings_dispatch_dir)(EngineSettings*, const char*) = nullptr;
  Status (*engine_create)(const EngineSettings*, Engine**) = nullptr;
  void (*engine_delete)(Engine*) = nullptr;
  Status (*sampler_create)(int, SamplerParams**) = nullptr;
  void (*sampler_delete)(SamplerParams*) = nullptr;
  Status (*sampler_top_k)(SamplerParams*, int32_t) = nullptr;
  Status (*sampler_top_p)(SamplerParams*, float) = nullptr;
  Status (*sampler_temperature)(SamplerParams*, float) = nullptr;
  Status (*sampler_seed)(SamplerParams*, int32_t) = nullptr;
  Status (*session_config_create)(SessionConfig**) = nullptr;
  void (*session_config_delete)(SessionConfig*) = nullptr;
  Status (*session_config_max_output)(SessionConfig*, int) = nullptr;
  Status (*session_config_sampler)(SessionConfig*, const SamplerParams*) = nullptr;
  Status (*conv_config_create)(ConvConfig**) = nullptr;
  void (*conv_config_delete)(ConvConfig*) = nullptr;
  Status (*conv_config_session)(ConvConfig*, const SessionConfig*) = nullptr;
  Status (*conv_config_system)(ConvConfig*, const char*) = nullptr;
  Status (*conv_config_messages)(ConvConfig*, const char*) = nullptr;
  Status (*conv_create)(Engine*, ConvConfig*, Conversation**) = nullptr;
  void (*conv_delete)(Conversation*) = nullptr;
  Status (*conv_send_stream)(Conversation*, const char*, const char*, const void*, StreamCb, void*) = nullptr;
  Status (*conv_cancel)(Conversation*) = nullptr;
  Status (*chunk_text)(const StreamChunk*, const char**) = nullptr;
  Status (*chunk_final)(const StreamChunk*, bool*) = nullptr;
  Status (*chunk_error)(const StreamChunk*, const char**) = nullptr;

  std::string error(const std::string& what) const {
    const char* e = last_error ? last_error() : nullptr;
    return what + (e && *e ? ": " + std::string(e) : "");
  }
};

void load(Api& a, const fs::path& lib) {
  if (!a.lib.open(lib)) throw std::runtime_error("no se pudo cargar LiteRT-LM: " + a.lib.error());
  std::vector<std::string> missing;
  auto need = [&](const char* name, auto& f) {
    if (!a.lib.get(name, f)) missing.push_back(name);
  };
  need("litert_lm_get_api_version", a.get_api_version);
  need("litert_lm_set_min_log_level", a.set_min_log_level);
  a.lib.get("litert_lm_get_last_error_message", a.last_error);
  need("litert_lm_engine_settings_create", a.settings_create);
  need("litert_lm_engine_settings_delete", a.settings_delete);
  need("litert_lm_engine_settings_set_max_num_tokens", a.settings_max_tokens);
  need("litert_lm_engine_settings_set_num_threads", a.settings_threads);
  need("litert_lm_engine_settings_set_cache_dir", a.settings_cache_dir);
  need("litert_lm_engine_settings_set_litert_dispatch_lib_dir", a.settings_dispatch_dir);
  need("litert_lm_engine_create", a.engine_create);
  need("litert_lm_engine_delete", a.engine_delete);
  need("litert_lm_sampler_params_create", a.sampler_create);
  need("litert_lm_sampler_params_delete", a.sampler_delete);
  need("litert_lm_sampler_params_set_top_k", a.sampler_top_k);
  need("litert_lm_sampler_params_set_top_p", a.sampler_top_p);
  need("litert_lm_sampler_params_set_temperature", a.sampler_temperature);
  need("litert_lm_sampler_params_set_seed", a.sampler_seed);
  need("litert_lm_session_config_create", a.session_config_create);
  need("litert_lm_session_config_delete", a.session_config_delete);
  need("litert_lm_session_config_set_max_output_tokens", a.session_config_max_output);
  need("litert_lm_session_config_set_sampler_params", a.session_config_sampler);
  need("litert_lm_conversation_config_create", a.conv_config_create);
  need("litert_lm_conversation_config_delete", a.conv_config_delete);
  need("litert_lm_conversation_config_set_session_config", a.conv_config_session);
  need("litert_lm_conversation_config_set_system_message", a.conv_config_system);
  need("litert_lm_conversation_config_set_messages", a.conv_config_messages);
  need("litert_lm_conversation_create", a.conv_create);
  need("litert_lm_conversation_delete", a.conv_delete);
  need("litert_lm_conversation_send_message_stream", a.conv_send_stream);
  need("litert_lm_conversation_cancel_process", a.conv_cancel);
  need("litert_lm_stream_chunk_get_text", a.chunk_text);
  need("litert_lm_stream_chunk_is_final", a.chunk_final);
  need("litert_lm_stream_chunk_get_error", a.chunk_error);
  if (!missing.empty())
    throw std::runtime_error("LiteRT-LM incompatible (le falta " + missing[0] + "): se necesita la version 0.18 o posterior");
  int major = 0, minor = 0, patch = 0;
  if (a.get_api_version(&major, &minor, &patch) != 0 || major != 1)
    throw std::runtime_error(cat("API C de LiteRT-LM ", major, ".", minor, " no compatible (se usa la 1.x)"));
  L.info("LiteRT-LM API ", major, ".", minor, ".", patch, " (", S(lib), ")");
}

// texto de un trozo: LiteRT-LM manda el mensaje en JSON ({"role", "content": [{"type": "text", "text"}]}) o texto
std::string chunk_to_text(const std::string& s) {
  json m = jparse(s);
  if (!m.is_object()) return s;
  json c = m.value("content", json());
  if (c.is_string()) return c.get<std::string>();
  std::string out;
  if (c.is_array())
    for (auto& p : c)
      if (p.is_object() && jstr(p, "type", "text") == "text") out += jstr(p, "text");
      else if (p.is_string()) out += p.get<std::string>();
  return out;
}

class LiteRt : public Motor {
 public:
  explicit LiteRt(const Config& c) : acel_(c.acelerador) {
    load(api_, c.lib);
    api_.set_min_log_level(3);   // avisos y errores
    EngineSettings* s = nullptr;
    std::string model = S(c.modelo);
    if (api_.settings_create(model.c_str(), acel_.c_str(), nullptr, nullptr, &s) != 0 || !s)
      throw std::runtime_error(api_.error("LiteRT-LM no acepta el modelo con " + acel_));
    api_.settings_max_tokens(s, std::max(512, c.n_ctx));
    if (acel_ == "cpu" && c.hilos > 0) api_.settings_threads(s, c.hilos);
    // cache de programas de GPU y NPU compilados: la segunda carga del modelo es mucho mas rapida
    std::string cache = S(c.cache);
    if (!cache.empty()) {
      mkdirs(c.cache);
      api_.settings_cache_dir(s, cache.c_str());
    }
    std::string dispatch = S(c.dispatch);
    if (acel_ == "npu" && !dispatch.empty()) api_.settings_dispatch_dir(s, dispatch.c_str());
    Status st = api_.engine_create(s, &engine_);
    std::string err = st != 0 || !engine_ ? api_.error("LiteRT-LM no pudo cargar el modelo en " + acel_) : "";
    api_.settings_delete(s);
    if (!err.empty()) throw std::runtime_error(err);
  }

  ~LiteRt() override {
    if (conv_) api_.conv_delete(conv_);
    if (engine_) api_.engine_delete(engine_);
  }

  std::string acelerador() const override { return acel_; }

  void generar(const json& messages_in, const Params& p, const std::function<bool(const std::string&)>& on_piece) override {
    json msgs = mensajes_simples(messages_in);
    if (msgs.empty() || jstr(msgs.back(), "role") != "user") throw std::invalid_argument("falta el mensaje del usuario");
    std::string system;
    json turns = json::array();
    for (auto& m : msgs) {
      if (jstr(m, "role") == "system") system += (system.empty() ? "" : "\n\n") + jstr(m, "content");
      else turns.push_back(m);
    }
    json last = turns.back();
    turns.erase(turns.size() - 1);
    std::string key = cat(p.max_tokens, "|", p.temperature, "|", p.top_p, "|", p.top_k, "|", system);
    // reutilizar la conversacion (y su cache KV) si la peticion continua la anterior
    if (!(conv_ && key == conv_key_ && turns == hist_)) nueva(system, turns, p, key);
    std::string msg = jdump({{"role", "user"}, {"content", jstr(last, "content")}});
    struct Ctx {
      const Api* api;
      std::mutex mu;
      std::condition_variable cv;
      std::deque<std::string> pieces;
      std::string error;
      bool done = false;
    } ctx;
    ctx.api = &api_;
    auto cb = [](void* data, const StreamChunk* chunk) {
      auto* c = static_cast<Ctx*>(data);
      const char* err = nullptr;
      const char* text = nullptr;
      bool final_ = false;
      c->api->chunk_error(chunk, &err);
      if (!err) c->api->chunk_text(chunk, &text);
      c->api->chunk_final(chunk, &final_);
      std::lock_guard<std::mutex> lk(c->mu);
      if (err && *err) c->error = err;
      else if (text && *text) c->pieces.push_back(chunk_to_text(text));
      if (final_ || (err && *err)) c->done = true;
      c->cv.notify_one();
    };
    if (api_.conv_send_stream(conv_, msg.c_str(), nullptr, nullptr, cb, &ctx) != 0) {
      reset();
      throw std::runtime_error(api_.error("LiteRT-LM no pudo procesar el mensaje"));
    }
    std::string answer;
    bool cancelled = false;
    while (true) {
      std::unique_lock<std::mutex> lk(ctx.mu);
      ctx.cv.wait(lk, [&] { return ctx.done || !ctx.pieces.empty(); });
      while (!ctx.pieces.empty()) {
        std::string piece = std::move(ctx.pieces.front());
        ctx.pieces.pop_front();
        if (cancelled) continue;
        answer += piece;
        lk.unlock();
        bool more = on_piece(piece);
        lk.lock();
        if (!more && !ctx.done) {
          cancelled = true;
          api_.conv_cancel(conv_);   // tras cancelar, la conversacion no se puede reutilizar
        }
      }
      if (ctx.done) break;
    }
    std::string err = ctx.error;
    // «Max number of tokens reached» es el final normal al llegar al limite
    bool limit = contains(err, "Max number of tokens") || contains(err, "CANCELLED");
    if (cancelled || !err.empty()) {
      reset();
      if (!err.empty() && !limit && answer.empty()) throw std::runtime_error("LiteRT-LM: " + err);
      return;
    }
    hist_ = turns;
    hist_.push_back(last);
    hist_.push_back({{"role", "assistant"}, {"content", answer}});
  }

 private:
  void reset() {
    if (conv_) api_.conv_delete(conv_);
    conv_ = nullptr;
    hist_ = json::array();
  }

  void nueva(const std::string& system, const json& turns, const Params& p, const std::string& key) {
    reset();
    SessionConfig* sc = nullptr;
    SamplerParams* sp = nullptr;
    ConvConfig* cc = nullptr;
    if (api_.session_config_create(&sc) != 0) throw std::runtime_error(api_.error("LiteRT-LM: configuracion de sesion"));
    api_.session_config_max_output(sc, std::max(16, p.max_tokens));
    bool greedy = p.temperature <= 0.01;
    if (api_.sampler_create(greedy ? 3 : 2, &sp) == 0 && sp) {
      if (!greedy) {
        api_.sampler_top_k(sp, std::max(1, p.top_k));
        api_.sampler_top_p(sp, (float)p.top_p);
        api_.sampler_temperature(sp, (float)p.temperature);
      }
      if (p.seed >= 0) api_.sampler_seed(sp, (int32_t)(p.seed & 0x7fffffff));
      api_.session_config_sampler(sc, sp);
    }
    // el sistema va como cadena JSON (si no, un texto que pareciera JSON se interpretaria)
    std::string sys_json = jdump(json(system));
    std::string hist_json = jdump(turns);
    Status st = api_.conv_config_create(&cc);
    if (st == 0) {
      api_.conv_config_session(cc, sc);
      if (!system.empty()) api_.conv_config_system(cc, sys_json.c_str());
      if (!turns.empty()) api_.conv_config_messages(cc, hist_json.c_str());
      st = api_.conv_create(engine_, cc, &conv_);
    }
    std::string err = st != 0 || !conv_ ? api_.error("LiteRT-LM no pudo crear la conversacion") : "";
    if (cc) api_.conv_config_delete(cc);
    if (sp) api_.sampler_delete(sp);
    api_.session_config_delete(sc);
    if (!err.empty()) {
      conv_ = nullptr;
      throw std::runtime_error(err);
    }
    hist_ = turns;
    conv_key_ = key;
  }

  Api api_;
  std::string acel_;
  Engine* engine_ = nullptr;
  Conversation* conv_ = nullptr;
  json hist_ = json::array();
  std::string conv_key_;
};

}  // namespace

std::unique_ptr<Motor> abrir_litert(const Config& c) { return std::make_unique<LiteRt>(c); }

}  // namespace nx::motores
