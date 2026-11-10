// Motor Qualcomm Genie (QAIRT) por su API C (GenieDialog.h), cargada al ejecutar.
//
// El modelo es una carpeta de Qualcomm AI Hub: genie_config.json, tokenizer.json, los binarios de contexto QNN
// (*.bin) compilados para la NPU Hexagon y htp_backend_ext_config.json. Las rutas del genie_config.json son relativas
// a la carpeta del modelo, asi que el proceso del motor trabaja dentro de ella. Genie no aplica la plantilla de chat:
// se elige por los tokens especiales del tokenizer (Llama 3, ChatML de Qwen, Phi, Granite, Mistral).
#include <cstring>

#include "llm/motores.h"
#include "util/dynlib.h"
#include "util/log.h"
#include "util/text.h"

namespace nx::motores {

namespace {

const auto L = log::get("genie");

using Status = int32_t;   // Genie_Status_t: 0 = GENIE_STATUS_SUCCESS
using Handle = void*;
using QueryCb = void (*)(const char*, int, const void*);   // respuesta, GenieDialog_SentenceCode_t, datos
constexpr int SENTENCE_COMPLETE = 0;

struct Api {
  DynLib lib;
  Status (*config_from_json)(const char*, Handle*) = nullptr;
  Status (*config_free)(Handle) = nullptr;
  Status (*dialog_create)(Handle, Handle*) = nullptr;
  Status (*dialog_query)(Handle, const char*, int, QueryCb, const void*) = nullptr;
  Status (*dialog_reset)(Handle) = nullptr;
  Status (*dialog_free)(Handle) = nullptr;
  Status (*dialog_signal)(Handle, int) = nullptr;               // opcional: GENIE_DIALOG_ACTION_ABORT = 1
  Status (*dialog_get_sampler)(Handle, Handle*) = nullptr;      // opcionales (QAIRT 2.29+): temperatura, top-k...
  Status (*sampler_config_from_json)(const char*, Handle*) = nullptr;
  Status (*sampler_config_set)(Handle, const char*, const char*) = nullptr;
  Status (*sampler_apply)(Handle, Handle) = nullptr;
  Status (*sampler_config_free)(Handle) = nullptr;
  const char* (*api_version)() = nullptr;
};

void load(Api& a, const fs::path& lib) {
  if (!a.lib.open(lib)) throw std::runtime_error("no se pudo cargar Genie: " + a.lib.error());
  std::vector<std::string> missing;
  auto need = [&](const char* name, auto& f) {
    if (!a.lib.get(name, f)) missing.push_back(name);
  };
  need("GenieDialogConfig_createFromJson", a.config_from_json);
  need("GenieDialogConfig_free", a.config_free);
  need("GenieDialog_create", a.dialog_create);
  need("GenieDialog_query", a.dialog_query);
  need("GenieDialog_reset", a.dialog_reset);
  need("GenieDialog_free", a.dialog_free);
  a.lib.get("GenieDialog_signal", a.dialog_signal);
  a.lib.get("GenieDialog_getSampler", a.dialog_get_sampler);
  a.lib.get("GenieSamplerConfig_createFromJson", a.sampler_config_from_json);
  a.lib.get("GenieSamplerConfig_setParam", a.sampler_config_set);
  a.lib.get("GenieSampler_applyConfig", a.sampler_apply);
  a.lib.get("GenieSamplerConfig_free", a.sampler_config_free);
  if (!missing.empty()) throw std::runtime_error("Genie incompatible (le falta " + missing[0] + ")");
  L.info("Genie cargado (", S(lib), ")");
}

// plantilla de chat por los tokens especiales del tokenizer del modelo
enum class Plantilla { Llama3, ChatML, Phi, Granite, Mistral, Simple };

Plantilla detectar(const fs::path& dir) {
  std::string t = read_text(dir / "tokenizer.json") + read_text(dir / "tokenizer_config.json");
  if (contains(t, "<|start_header_id|>")) return Plantilla::Llama3;
  if (contains(t, "<|im_start|>")) return Plantilla::ChatML;
  if (contains(t, "<|start_of_role|>")) return Plantilla::Granite;
  if (contains(t, "<|assistant|>")) return Plantilla::Phi;
  if (contains(t, "[INST]")) return Plantilla::Mistral;
  return Plantilla::Simple;
}

std::string formatear(Plantilla pl, const json& msgs) {
  std::string out;
  bool first_user = true;
  std::string pending_sys;
  for (auto& m : msgs) {
    std::string r = jstr(m, "role"), c = jstr(m, "content");
    switch (pl) {
      case Plantilla::Llama3:
        if (out.empty()) out = "<|begin_of_text|>";
        out += "<|start_header_id|>" + r + "<|end_header_id|>\n\n" + c + "<|eot_id|>";
        break;
      case Plantilla::ChatML: out += "<|im_start|>" + r + "\n" + c + "<|im_end|>\n"; break;
      case Plantilla::Granite: out += "<|start_of_role|>" + r + "<|end_of_role|>" + c + "<|end_of_text|>\n"; break;
      case Plantilla::Phi: out += "<|" + r + "|>\n" + c + "<|end|>\n"; break;
      case Plantilla::Mistral:
        // Mistral no tiene papel de sistema: va delante del primer mensaje del usuario
        if (r == "system") pending_sys += c + "\n\n";
        else if (r == "user") {
          out += std::string(first_user ? "<s>" : "") + "[INST] " + pending_sys + c + " [/INST]";
          pending_sys.clear();
          first_user = false;
        } else
          out += " " + c + "</s>";
        break;
      case Plantilla::Simple: out += (r == "user" ? "Usuario: " : r == "system" ? "" : "Asistente: ") + c + "\n\n"; break;
    }
  }
  switch (pl) {
    case Plantilla::Llama3: out += "<|start_header_id|>assistant<|end_header_id|>\n\n"; break;
    case Plantilla::ChatML: out += "<|im_start|>assistant\n"; break;
    case Plantilla::Granite: out += "<|start_of_role|>assistant<|end_of_role|>"; break;
    case Plantilla::Phi: out += "<|assistant|>\n"; break;
    case Plantilla::Mistral: break;
    case Plantilla::Simple: out += "Asistente: "; break;
  }
  return out;
}

class Genie : public Motor {
 public:
  explicit Genie(const Config& c) {
    load(api_, c.lib);
    fs::path dir = is_dir(c.modelo) ? c.modelo : c.modelo.parent_path();
    std::string cfg_text = read_text(dir / "genie_config.json");
    if (cfg_text.empty()) throw std::runtime_error("falta genie_config.json en " + S(dir));
    // las rutas del genie_config.json (tokenizer, *.bin, extensiones de HTP) son relativas a la carpeta del modelo
    std::error_code ec;
    fs::current_path(dir, ec);
    plantilla_ = detectar(dir);
    Handle cfg = nullptr;
    if (api_.config_from_json(cfg_text.c_str(), &cfg) != 0 || !cfg)
      throw std::runtime_error("Genie no acepta genie_config.json de " + S(dir));
    Status st = api_.dialog_create(cfg, &dialog_);
    api_.config_free(cfg);
    if (st != 0 || !dialog_)
      throw std::runtime_error(cat("Genie no pudo cargar el modelo en la NPU (estado ", st,
                                   "): revisa que los .bin sean para este Snapdragon y que esten las bibliotecas QNN HTP"));
  }

  ~Genie() override {
    if (dialog_) api_.dialog_free(dialog_);
  }

  std::string acelerador() const override { return "npu"; }

  void generar(const json& messages, const Params& p, const std::function<bool(const std::string&)>& on_piece) override {
    json msgs = mensajes_simples(messages);
    if (msgs.empty() || jstr(msgs.back(), "role") != "user") throw std::invalid_argument("falta el mensaje del usuario");
    muestreo(p);
    // cada peticion lleva toda la conversacion: se empieza de cero para que la cache KV no la duplique
    api_.dialog_reset(dialog_);
    std::string prompt = formatear(plantilla_, msgs);
    struct Ctx {
      Genie* self;
      const std::function<bool(const std::string&)>* on;
      size_t sent = 0;
      int max;
      bool stop = false;
    } ctx{this, &on_piece, 0, std::max(16, p.max_tokens) * 8};
    auto cb = [](const char* resp, int, const void* data) {
      auto* c = static_cast<Ctx*>(const_cast<void*>(data));
      if (c->stop || !resp || !*resp) return;
      std::string piece = resp;
      c->sent += piece.size();
      if (!(*c->on)(piece) || (int)c->sent > c->max) {
        c->stop = true;
        if (c->self->api_.dialog_signal) c->self->api_.dialog_signal(c->self->dialog_, 1);
      }
    };
    Status st = api_.dialog_query(dialog_, prompt.c_str(), SENTENCE_COMPLETE, cb, &ctx);
    if (st != 0 && !ctx.stop && ctx.sent == 0) throw std::runtime_error(cat("Genie no pudo generar la respuesta (estado ", st, ")"));
  }

 private:
  void muestreo(const Params& p) {
    std::string key = cat(p.temperature, "|", p.top_k, "|", p.top_p, "|", p.seed);
    if (key == muestreo_ || !api_.dialog_get_sampler || !api_.sampler_config_from_json || !api_.sampler_config_set ||
        !api_.sampler_apply)
      return;
    muestreo_ = key;
    Handle sampler = nullptr, sc = nullptr;
    if (api_.dialog_get_sampler(dialog_, &sampler) != 0 || !sampler) return;
    if (api_.sampler_config_from_json(R"({"sampler":{"version":1}})", &sc) != 0 || !sc) return;
    api_.sampler_config_set(sc, "temp", fnum(std::max(0.0, p.temperature), 2).c_str());
    api_.sampler_config_set(sc, "top-k", std::to_string(std::max(1, p.top_k)).c_str());
    api_.sampler_config_set(sc, "top-p", fnum(p.top_p, 2).c_str());
    if (p.seed >= 0) api_.sampler_config_set(sc, "seed", std::to_string(p.seed & 0x7fffffff).c_str());
    if (api_.sampler_apply(sampler, sc) != 0) L.warning("Genie no acepto los parametros de muestreo");
    if (api_.sampler_config_free) api_.sampler_config_free(sc);
  }

  Api api_;
  Handle dialog_ = nullptr;
  Plantilla plantilla_ = Plantilla::Simple;
  std::string muestreo_;
};

}  // namespace

std::unique_ptr<Motor> abrir_genie(const Config& c) { return std::make_unique<Genie>(c); }

}  // namespace nx::motores
