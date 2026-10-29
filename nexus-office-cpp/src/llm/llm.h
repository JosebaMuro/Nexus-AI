// LLM (nexus/llm): backends intercambiables con streaming, protocolo de herramientas por texto, separacion del
// razonamiento (<think>) y gestor que elige backend y modelo.
// Mensajes: {"role": "system|user|assistant", "content": str, "images": [base64...]}.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "core/config.h"
#include "util/json.h"
#include "util/proc.h"

namespace nx::hw {
class Monitor;
}
namespace nx::models {
class Catalog;
}
namespace nx::runtimes {
class RuntimeManager;
}

namespace nx::llm {

using Sink = std::function<void(const std::string&)>;
using ReasonFn = std::function<void(const std::string&)>;

// opciones de una llamada (claves como en Python: temperature, max_tokens, tools, tool_specs, reasoning,
// reasoning_budget, think_mode, extra_body, n_ctx, n_threads, n_batch, gpu_layers, flash_attn, device)
struct Opts {
  json o = json::object();
  ReasonFn on_reasoning;
};

class ToolsUnsupported : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- protocolo de herramientas
struct ToolCall {
  std::string name;
  json args;
  std::string pre;
};
std::optional<ToolCall> parse_tool_call(const std::string& text);
std::string format_result(const std::string& name, const json& result);
std::string tool_instructions(const json& tools);
std::string tool_call_text(const std::string& name, const json& args);   // <tool>{...}</tool>

// ---------------------------------------------------------------- razonamiento
bool is_reasoning_model(const json& entry_or_name);
json merge_roles(const json& messages);
std::string reasoning_fallback(const std::string& reasoning);

class ThinkSplitter {
 public:
  explicit ThinkSplitter(const std::string& mode = "tags", size_t hold = 6000);
  void server_split() { pending_ = false; }
  std::pair<std::string, std::string> feed(const std::string& piece);
  std::pair<std::string, std::string> flush();
  std::string reasoning() const;
  void add_thought(const std::string& t) { thought_.push_back(t); }
  size_t visible = 0;
  size_t raw = 0;

 private:
  std::string mode_;
  size_t hold_;
  bool inside_ = false, pending_;
  std::string buf_;
  std::vector<std::string> thought_;
  std::pair<std::string, std::string> out(const std::string& v, const std::string& t);
};

struct Stats {
  long tokens = 0;
  double seconds = 0, ttft = 0;
};

// ---------------------------------------------------------------- backends
class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string name() const = 0;
  virtual bool available() { return false; }
  virtual std::vector<std::string> models() { return {}; }
  virtual void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) = 0;
  std::string complete(const std::string& prompt, const std::string& model = "", const Opts& opts = {});
  Stats last_stats;
  std::string last_reasoning;

 protected:
  // tuberia comun: razonamiento separado, tiempos y respuesta de reserva si solo hubo razonamiento
  struct Pipe {
    Backend& b;
    ThinkSplitter sp;
    const Sink& out;
    ReasonFn on_reasoning;
    double t0, first = 0;
    long n = 0;
    Pipe(Backend& be, const std::string& mode, const Sink& o, ReasonFn r);
    void piece(const std::string& p);
    void finish(long tokens = -1);
  };
};

class OllamaBackend : public Backend {
 public:
  OllamaBackend(std::string url, double timeout);
  std::string name() const override { return "ollama"; }
  bool available() override;
  std::vector<std::string> models() override;
  void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) override;
  void pull(const std::string& name, const std::function<bool(const json&)>& on_event);

 private:
  std::string url_;
  double timeout_;
};

class OpenAICompatBackend : public Backend {
 public:
  OpenAICompatBackend(std::string base_url, std::string api_key = "", std::string default_model = "",
                      double timeout = 180);
  std::string name() const override { return "openai"; }
  bool available() override;
  std::vector<std::string> models() override;
  void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) override;

 protected:
  std::string base_url_, api_key_, default_model_;
  double timeout_;
  std::vector<std::pair<std::string, std::string>> headers() const;
};

class LlamaServerBackend : public OpenAICompatBackend {
 public:
  using Resolver = std::function<std::optional<std::pair<std::string, json>>(const std::string&)>;
  LlamaServerBackend(runtimes::RuntimeManager* rt, Resolver resolve, std::function<json()> profile,
                     std::function<std::string()> variant);
  ~LlamaServerBackend() override;
  std::string name() const override { return "llamaserver"; }
  bool available() override;
  std::vector<std::string> models() override;
  bool supports_vision(const std::string& model = "");
  void ensure(const std::string& model, const json& opts);
  void stop();
  void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) override;
  json effective();
  json gpu();                // GPU de llama.cpp elegida (cuando hay varias)
  std::string gpu_error();   // por que la GPU elegida no arranco (y se sigue en la CPU)
  std::string variant;
  int port = 0;
  // Android: la NPU o la GPU no arrancan con el driver del movil; el gestor cambia gpu_layers/device del perfil
  std::function<void(const json&)> set_llm_opts;

 private:
  void ensure_once(const std::string& model, const json& opts);
  json gpu_failed(const std::string& reason, const std::string& device);
  runtimes::RuntimeManager* rt_;
  Resolver resolve_;
  std::function<json()> profile_;
  std::function<std::string()> variant_getter_;
  std::unique_ptr<proc::Process> proc_;
  std::string key_, device_;
  json effective_ = json::object();
  json gpu_ = json::object();
  std::string gpu_error_;
  bool kv_quant_failed_ = false;   // este llama-server no arranca con la cache KV cuantizada
  std::recursive_mutex mu_;
};

// motor integrado sin modelo (reglas)
class BuiltinBackend : public Backend {
 public:
  std::string name() const override { return "builtin"; }
  bool available() override { return true; }
  std::vector<std::string> models() override { return {"nexus-core (reglas, sin LLM)"}; }
  void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) override;
  static std::string after_tool(const std::string& name, const std::string& body);
  std::string respond(const json& messages, const std::set<std::string>& tools);
};

// motor en el proceso (llama-cpp-python en la version Python): en la edicion C++ no esta incluido
class UnavailableBackend : public Backend {
 public:
  explicit UnavailableBackend(std::string n) : n_(std::move(n)) {}
  std::string name() const override { return n_; }
  void stream_chat(const json&, const std::string&, const Opts&, const Sink&) override {
    throw std::runtime_error("El motor '" + n_ + "' no esta disponible en la edicion C++: usa llama-server");
  }

 private:
  std::string n_;
};

// IA online para el agente Profesor (llm/cloud.py): Gemini, ChatGPT, Perplexity, Claude, Mistral... Solo con la
// clave que pone un administrador y solo si un agente la pide ("nube:", "nube:<proveedor>[:<modelo>]").
struct CloudProvider {
  std::string id, nombre, estilo, base_url, modelo, clave_en;
};
const std::vector<CloudProvider>& cloud_providers();
extern const std::vector<std::string> CLOUD_EFFORTS;

class CloudError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class CloudBackend : public Backend {
 public:
  explicit CloudBackend(Config& config, double timeout = 180) : config_(config), timeout_(timeout) {}
  std::string name() const override { return "nube"; }
  bool available() override { return ready(); }
  std::vector<std::string> models() override;
  void stream_chat(const json& messages, const std::string& model, const Opts& opts, const Sink& out) override;
  std::pair<std::string, std::string> resolve(const std::string& spec = "") const;
  bool ready(const std::string& spec = "") const;
  std::string key(const std::string& prov) const;
  std::string base_url(const std::string& prov) const;
  json status() const;
  json test(const std::string& spec = "");
  static std::map<std::string, std::string>& base_override();   // pruebas: URL de un servidor falso

 private:
  Config& config_;
  double timeout_;
  void openai(const std::string& prov, const json& messages, const std::string& model, const Opts& opts, Pipe& pipe);
  void anthropic(const json& messages, const std::string& model, const Opts& opts, Pipe& pipe);
};

// intenciones inequivocas que se resuelven con una herramienta sin esperar al LLM
std::optional<std::pair<std::string, json>> detect_intent(const std::string& text, const std::set<std::string>& tools);

// ---------------------------------------------------------------- gestor
extern const char* REASONING_STOP;
json fit_context(json messages, int n_ctx, int reply_tokens);

class LLMManager {
 public:
  LLMManager(Config& config, models::Catalog& catalog, json& profile, hw::Monitor* monitor, runtimes::RuntimeManager* rt);
  void rebuild();
  bool is_available(const std::string& name);
  std::pair<std::string, std::string> select(const std::string& spec = "default");
  json status();
  json options(const json& agent_opts = json::object());
  // perfil de llama.cpp (hilos, contexto, GPU) con el cerrojo del gestor: lo cambia el autoajuste en vivo
  json llm_profile();
  void update_llm_profile(const json& changes);
  bool reasoning_model(const std::string& backend, const std::string& model);
  bool thinking_only(const std::string& backend, const std::string& model);
  void stream(json messages, const std::string& spec, Opts opts, const Sink& out);
  std::string complete(const std::string& prompt, const std::string& spec = "default", Opts opts = {});
  bool native_tools(const std::string& spec = "default");
  bool has_real_llm();
  LlamaServerBackend::Resolver resolver(const std::string& fmt);
  std::shared_ptr<Backend> backend(const std::string& name);
  std::vector<std::string> backend_names();
  std::set<std::string> native_off;
  std::atomic<int> inflight{0};
  // "peer:<nodo>": inferencia en otro nodo de la red cooperativa
  std::function<void(const std::string&, const json&, const Opts&, const Sink&)> remote;
  std::pair<std::string, std::string> last_used;

 private:
  Config& config_;
  models::Catalog& catalog_;
  json& profile_;
  hw::Monitor* monitor_;
  runtimes::RuntimeManager* rt_;
  std::recursive_mutex mu_;
  std::vector<std::pair<std::string, std::shared_ptr<Backend>>> backends_;
  std::map<std::string, std::pair<double, bool>> avail_;
};

}  // namespace nx::llm
