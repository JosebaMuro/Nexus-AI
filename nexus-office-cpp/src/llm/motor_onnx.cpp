// Motor ONNX Runtime GenAI (Microsoft) por su API C (ort_genai_c.h), cargada al ejecutar.
//
// El modelo es una carpeta con genai_config.json, preparada para un proveedor (DirectML, QNN, OpenVINO, VitisAI,
// CUDA o CPU). Por defecto se usan los proveedores del propio modelo; el acelerador «cpu» los quita y otro nombre
// (dml, qnn, OpenVINO, VitisAI, cuda, webgpu) los sustituye por ese.
#include "llm/motores.h"
#include "util/dynlib.h"
#include "util/log.h"
#include "util/text.h"

#ifdef _WIN32
#define OGA_CALL __stdcall
#else
#define OGA_CALL
#endif

namespace nx::motores {

namespace {

const auto L = log::get("onnx");

struct OgaResult;
struct OgaConfig;
struct OgaModel;
struct OgaTokenizer;
struct OgaTokenizerStream;
struct OgaSequences;
struct OgaGeneratorParams;
struct OgaGenerator;
using R = OgaResult*;

struct Api {
  DynLib lib;
  const char*(OGA_CALL* result_error)(const OgaResult*) = nullptr;
  void(OGA_CALL* destroy_result)(OgaResult*) = nullptr;
  void(OGA_CALL* destroy_string)(const char*) = nullptr;
  R(OGA_CALL* create_config)(const char*, OgaConfig**) = nullptr;
  R(OGA_CALL* clear_providers)(OgaConfig*) = nullptr;
  R(OGA_CALL* append_provider)(OgaConfig*, const char*) = nullptr;
  void(OGA_CALL* destroy_config)(OgaConfig*) = nullptr;
  R(OGA_CALL* create_model)(const OgaConfig*, OgaModel**) = nullptr;
  void(OGA_CALL* destroy_model)(OgaModel*) = nullptr;
  R(OGA_CALL* create_tokenizer)(const OgaModel*, OgaTokenizer**) = nullptr;
  void(OGA_CALL* destroy_tokenizer)(OgaTokenizer*) = nullptr;
  R(OGA_CALL* chat_template)(const OgaTokenizer*, const char*, const char*, const char*, bool, const char**) = nullptr;
  R(OGA_CALL* create_sequences)(OgaSequences**) = nullptr;
  void(OGA_CALL* destroy_sequences)(OgaSequences*) = nullptr;
  size_t(OGA_CALL* sequence_count)(const OgaSequences*, size_t) = nullptr;
  R(OGA_CALL* encode)(const OgaTokenizer*, const char*, OgaSequences*) = nullptr;
  R(OGA_CALL* create_params)(const OgaModel*, OgaGeneratorParams**) = nullptr;
  void(OGA_CALL* destroy_params)(OgaGeneratorParams*) = nullptr;
  R(OGA_CALL* set_number)(OgaGeneratorParams*, const char*, double) = nullptr;
  R(OGA_CALL* set_bool)(OgaGeneratorParams*, const char*, bool) = nullptr;
  R(OGA_CALL* create_generator)(const OgaModel*, const OgaGeneratorParams*, OgaGenerator**) = nullptr;
  void(OGA_CALL* destroy_generator)(OgaGenerator*) = nullptr;
  R(OGA_CALL* append_sequences)(OgaGenerator*, const OgaSequences*) = nullptr;
  bool(OGA_CALL* is_done)(OgaGenerator*) = nullptr;
  R(OGA_CALL* next_token)(OgaGenerator*) = nullptr;
  R(OGA_CALL* get_next_tokens)(const OgaGenerator*, const int32_t**, size_t*) = nullptr;
  R(OGA_CALL* create_stream)(const OgaTokenizer*, OgaTokenizerStream**) = nullptr;
  void(OGA_CALL* destroy_stream)(OgaTokenizerStream*) = nullptr;
  R(OGA_CALL* stream_decode)(OgaTokenizerStream*, int32_t, const char**) = nullptr;
  void(OGA_CALL* register_ep)(const char*, const char*) = nullptr;   // opcional (0.9+)

  // lanza si la llamada devolvio error
  void check(R r, const std::string& what) const {
    if (!r) return;
    std::string e = result_error ? result_error(r) : "";
    destroy_result(r);
    throw std::runtime_error("ONNX Runtime GenAI, " + what + ": " + e);
  }
};

void load(Api& a, const fs::path& lib) {
  if (!a.lib.open(lib)) throw std::runtime_error("no se pudo cargar ONNX Runtime GenAI: " + a.lib.error());
  std::vector<std::string> missing;
  auto need = [&](const char* name, auto& f) {
    if (!a.lib.get(name, f)) missing.push_back(name);
  };
  need("OgaResultGetError", a.result_error);
  need("OgaDestroyResult", a.destroy_result);
  need("OgaDestroyString", a.destroy_string);
  need("OgaCreateConfig", a.create_config);
  need("OgaConfigClearProviders", a.clear_providers);
  need("OgaConfigAppendProvider", a.append_provider);
  need("OgaDestroyConfig", a.destroy_config);
  need("OgaCreateModelFromConfig", a.create_model);
  need("OgaDestroyModel", a.destroy_model);
  need("OgaCreateTokenizer", a.create_tokenizer);
  need("OgaDestroyTokenizer", a.destroy_tokenizer);
  need("OgaTokenizerApplyChatTemplate", a.chat_template);
  need("OgaCreateSequences", a.create_sequences);
  need("OgaDestroySequences", a.destroy_sequences);
  need("OgaSequencesGetSequenceCount", a.sequence_count);
  need("OgaTokenizerEncode", a.encode);
  need("OgaCreateGeneratorParams", a.create_params);
  need("OgaDestroyGeneratorParams", a.destroy_params);
  need("OgaGeneratorParamsSetSearchNumber", a.set_number);
  need("OgaGeneratorParamsSetSearchBool", a.set_bool);
  need("OgaCreateGenerator", a.create_generator);
  need("OgaDestroyGenerator", a.destroy_generator);
  need("OgaGenerator_AppendTokenSequences", a.append_sequences);
  need("OgaGenerator_IsDone", a.is_done);
  need("OgaGenerator_GenerateNextToken", a.next_token);
  need("OgaGenerator_GetNextTokens", a.get_next_tokens);
  need("OgaCreateTokenizerStream", a.create_stream);
  need("OgaDestroyTokenizerStream", a.destroy_stream);
  need("OgaTokenizerStreamDecode", a.stream_decode);
  a.lib.get("OgaRegisterExecutionProviderLibrary", a.register_ep);
  if (!missing.empty())
    throw std::runtime_error("ONNX Runtime GenAI incompatible (le falta " + missing[0] + "): se necesita la 0.8 o posterior");
  L.info("ONNX Runtime GenAI cargado (", S(lib), ")");
  // proveedores que van como «plugin» (QNN de onnxruntime-qnn 2.x, CUDA, TensorRT-RTX...): se registran los que
  // haya junto a la biblioteca para que el proveedor del modelo los encuentre. Se reconocen por CreateEpFactories.
  if (!a.register_ep) return;
  for (auto& f : glob(lib.parent_path(), "*onnxruntime_providers_*")) {
    std::string n = S(f.filename());
    if (!is_file(f) || contains(n, "_shared") || !(ends_with(n, ".dll") || ends_with(n, ".so") || ends_with(n, ".dylib")))
      continue;
    static std::vector<std::unique_ptr<DynLib>> keep;
    auto probe = std::make_unique<DynLib>();
    if (!probe->open(f) || !probe->sym("CreateEpFactories")) continue;
    std::string reg = stem(f);
    if (starts_with(reg, "lib")) reg = reg.substr(3);
    a.register_ep(reg.c_str(), S(f).c_str());
    keep.push_back(std::move(probe));
    L.info("Proveedor plugin registrado: ", reg);
  }
}

class Onnx : public Motor {
 public:
  explicit Onnx(const Config& c) : acel_(c.acelerador), n_ctx_(c.n_ctx) {
    load(api_, c.lib);
    OgaConfig* cfg = nullptr;
    api_.check(api_.create_config(S(c.modelo).c_str(), &cfg), "genai_config.json de " + S(c.modelo));
    try {
      if (!acel_.empty() && acel_ != "modelo") {
        api_.check(api_.clear_providers(cfg), "proveedores");
        if (acel_ != "cpu") api_.check(api_.append_provider(cfg, acel_.c_str()), "proveedor " + acel_);
      }
      api_.check(api_.create_model(cfg, &model_), "cargar el modelo" + (acel_.empty() ? "" : " con " + acel_));
    } catch (...) {
      api_.destroy_config(cfg);
      throw;
    }
    api_.destroy_config(cfg);
    api_.check(api_.create_tokenizer(model_, &tok_), "tokenizador");
  }

  ~Onnx() override {
    if (tok_) api_.destroy_tokenizer(tok_);
    if (model_) api_.destroy_model(model_);
  }

  std::string acelerador() const override { return acel_.empty() ? "modelo" : acel_; }

  void generar(const json& messages, const Params& p, const std::function<bool(const std::string&)>& on_piece) override {
    json msgs = mensajes_simples(messages);
    const char* prompt = nullptr;
    api_.check(api_.chat_template(tok_, nullptr, jdump(msgs).c_str(), nullptr, true, &prompt), "plantilla de chat");
    std::string text = prompt ? prompt : "";
    if (prompt) api_.destroy_string(prompt);
    OgaSequences* seq = nullptr;
    OgaGeneratorParams* gp = nullptr;
    OgaGenerator* gen = nullptr;
    OgaTokenizerStream* stream = nullptr;
    auto cleanup = [&] {
      if (stream) api_.destroy_stream(stream);
      if (gen) api_.destroy_generator(gen);
      if (gp) api_.destroy_params(gp);
      if (seq) api_.destroy_sequences(seq);
    };
    try {
      api_.check(api_.create_sequences(&seq), "secuencias");
      api_.check(api_.encode(tok_, text.c_str(), seq), "tokenizar");
      size_t n_prompt = api_.sequence_count(seq, 0);
      if ((int)n_prompt >= n_ctx_) throw std::runtime_error(cat("la conversacion (", n_prompt, " tokens) no cabe en el contexto (", n_ctx_, ")"));
      api_.check(api_.create_params(model_, &gp), "parametros");
      api_.check(api_.set_number(gp, "max_length", (double)std::min<size_t>(n_prompt + std::max(16, p.max_tokens), n_ctx_)), "max_length");
      bool sample = p.temperature > 0.01;
      api_.check(api_.set_bool(gp, "do_sample", sample), "do_sample");
      if (sample) {
        api_.check(api_.set_number(gp, "temperature", p.temperature), "temperature");
        api_.check(api_.set_number(gp, "top_p", p.top_p), "top_p");
        api_.check(api_.set_number(gp, "top_k", std::max(1, p.top_k)), "top_k");
        if (p.seed >= 0) api_.set_number(gp, "random_seed", (double)(p.seed & 0x7fffffff));
      }
      api_.check(api_.create_generator(model_, gp, &gen), "generador");
      api_.check(api_.append_sequences(gen, seq), "prompt");
      api_.check(api_.create_stream(tok_, &stream), "decodificador");
      while (!api_.is_done(gen)) {
        api_.check(api_.next_token(gen), "generar");
        const int32_t* toks = nullptr;
        size_t n = 0;
        api_.check(api_.get_next_tokens(gen, &toks, &n), "token");
        if (!n) break;
        const char* piece = nullptr;
        api_.check(api_.stream_decode(stream, toks[0], &piece), "decodificar");
        if (piece && *piece && !on_piece(piece)) break;
      }
    } catch (...) {
      cleanup();
      throw;
    }
    cleanup();
  }

 private:
  Api api_;
  std::string acel_;
  int n_ctx_;
  OgaModel* model_ = nullptr;
  OgaTokenizer* tok_ = nullptr;
};

}  // namespace

std::unique_ptr<Motor> abrir_onnx(const Config& c) { return std::make_unique<Onnx>(c); }

}  // namespace nx::motores
