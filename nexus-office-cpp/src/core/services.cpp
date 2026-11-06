// Ensamblado de todos los servicios de Nexus-Office (core.py).
#include "core/services.h"
#include "util/crypto.h"
#include "hal/matriz.h"
#include "media/difusion_onnx.h"
#include "llm/motores.h"
#include "hal/zerocopy.h"

#include <thread>

#include "core/android_tune.h"
#include "hardware/hw_profiles.h"

#include "agents/agents.h"
#include "coop/coop.h"
#include "remote/remote.h"
#include "llm/llm.h"
#include "media/media.h"
#include "models/models.h"
#include "models/runtimes.h"
#include "office/office.h"
#include "mcp/mcp.h"
#include "pipeline/pipeline.h"
#include "rag/rag.h"
#include "tools/tools.h"
#include "util/log.h"

namespace nx {

static auto L = log::get("core");

Services::Services(bool detect_hardware) : config(get_config()) {
  log::setup();
  Config& cfg = config;
  monitor = std::make_unique<hw::Monitor>(cfg.num("hardware.monitor_interval_s", 2.0));
  if (monitor->previous_unclean)
    L.warning("La sesion anterior no se cerro correctamente; se aplicara un perfil mas conservador");
  hw::profiles::al_arrancar(cfg);   // GPU elegida antes de calcular el perfil
  if (detect_hardware) {
    auto st = hw::startup(cfg.str("hardware.profile_override", ""), truthy(cfg.get("hardware.adaptive_tuning", true)),
                          cfg.str("llm.ollama_url", "http://127.0.0.1:11434"), monitor->previous_unclean,
                          monitor->previous_info);
    hw = st.hw;
    profile = st.profile;
    tuning_reasons = st.reasons;
  } else {   // modo rapido para pruebas
    if (!hw::load_cached(hw)) hw = hw::detect("", false);
    profile = hw::build_profile(hw);
  }
  std::string accel = cfg.str("hardware.accel", "auto");
  if (detect_hardware && accel != "auto") {   // CPU/GPU/NPU elegido a mano al arrancar
    auto [prof, plan] = hw::build_for(hw, accel);
    profile = prof;
    if (truthy(plan.value("note", json())))
      L.warning("Acelerador ", accel, ": ", jstr(plan, "note"));
  }
  hw::profiles::aplicar_al_perfil(*this);
  json st = json::object();
  for (const char* k : {"n_ctx", "n_threads", "n_batch", "gpu_layers"}) st[k] = profile["llm"][k];
  monitor->settings = st;
  auto profile_getter = [this] { return profile; };
  runtimes = std::make_unique<runtimes::RuntimeManager>();
  runtimes->import_found();   // paquetes de runtimes sin conexion (CUDA 13, ROCm, SYCL...) junto al programa
  db = std::make_unique<DB>();
  models = std::make_unique<models::ModelManager>(cfg, profile_getter);
  embedder_ = make_embedder();
  memory = std::make_unique<mem::MemoryStore>(*db, embedder_getter(), cfg.num("memory.decay_days", 30));
  graph = std::make_unique<mem::KnowledgeGraph>(*db);
  // que es comun (grafo general) y que es saber hacer de cada agente (su subgrafo)
  organizer = std::make_unique<mem::Organizer>(
      *graph, embedder_getter(),
      [this] { return agents ? agents->list() : std::vector<agents::AgentSpec>{}; }, &config);
  learner = std::make_unique<mem::Learner>(*db, *memory, *graph, *organizer);
  estilo = std::make_unique<mem::StyleLearner>(*db);
  rag = std::make_unique<rag::RAGStore>(*db, embedder_getter());
  rag->graph = graph.get();   // cada documento se enlaza en el grafo general o en el de su agente
  llm = std::make_unique<llm::LLMManager>(cfg, models->catalog, profile, monitor.get(), runtimes.get());
  // vectores semanticos en segundo plano con modelos lentos en CPU
  embed_worker = std::make_unique<mem::EmbedWorker>(
      *db, embedder_getter(), *memory, [this] { return llm && llm->inflight > 0; },
      [this] {
        if (rag) rag->invalidate();
      });
  rag->on_pending = memory->on_pending = [this] { embed_worker->wake(); };
  models->on_activate.push_back([this](const json& entry) {
    if (jstr(entry, "kind") == "embed") set_embedder();
  });
  models->ollama_pull = [this](const std::string& name, const std::function<bool(const json&)>& on_event) {
    auto b = std::dynamic_pointer_cast<llm::OllamaBackend>(llm->backend("ollama"));
    if (!b) throw std::runtime_error("Ollama no disponible");
    b->pull(name, on_event);
  };
  hilo = std::make_unique<mem::Hilo>(*db, [this] { return llm && llm->inflight > 0; });
  hilo->llm_complete = [this](const std::string& p, int max_tokens) {
    llm::Opts o;
    o.o = {{"max_tokens", max_tokens}, {"temperature", 0.2}, {"reasoning", "off"}};
    return llm->complete(p, "default", o);
  };
  hilo->llm_real = [this] { return llm->has_real_llm(); };
  learner->llm_complete = [this](const std::string& p) {
    llm::Opts o;
    o.o = {{"max_tokens", 300}, {"temperature", 0.1}};
    return llm->complete(p, "default", o);
  };
  // memoria de los tensores de los motores segun el acelerador (CUDA pinned, HIP, dma-buf...) y motores LiteRT-LM,
  // ONNX GenAI y Genie dentro del proceso (motores.en_proceso, por defecto si)
  hal::fijar_memoria_preferida(hal::memoria_para(profile_getter()));
  llm::MotorBackend::en_proceso = cfg.get("motores.en_proceso").is_null() || truthy(cfg.get("motores.en_proceso"));
  images = std::make_unique<media::ImageEngine>(cfg, models->catalog, *runtimes, profile_getter, llm.get());
  {
    pipeline::Dependencias d;
    d.db = db.get();
    d.embedder = [this] { return embedder(); };
    d.vision_disponible = [this] {
      try {
        auto [backend, model] = llm->select();
        auto b = std::dynamic_pointer_cast<llm::LlamaServerBackend>(llm->backend(backend));
        return b && b->supports_vision(model);
      } catch (...) {
        return false;
      }
    };
    d.analizar = [this](const std::string& bytes) { return images->analyze(bytes); };
    d.vision_vector = [this](const uint8_t* rgb, int w, int h) -> pipeline::Dependencias::Vector {
      auto c = codificador_vision();
      if (!c) return {};
      return {c->id(), c->imagen(rgb, w, h)};
    };
    d.vision_texto = [this](const std::string& q) -> pipeline::Dependencias::Vector {
      auto c = codificador_vision();
      if (!c || !c->tiene_texto()) return {};
      return {c->id(), c->texto(q)};
    };
    d.generar = [this](const pipeline::DescriptorTrabajo& t, const pipeline::VistaPrevia& vp) {
      return images->generate(t.prompt, t.negativo, t.tam ? std::optional<int>(t.tam) : std::nullopt,
                              t.pasos ? std::optional<int>(t.pasos) : std::nullopt, t.semilla, t.idea, vp);
    };
    d.emitir = [this](const json& e) {
      if (emitir_pipeline) emitir_pipeline(e);
    };
    pipeline::Opciones o;
    o.vision_cada_s = cfg.num("pipeline.vision_cada_s", 10.0);
    o.guardar_frames = cfg.flag("pipeline.guardar_frames", true);
    pipeline = std::make_unique<pipeline::Pipeline>(std::move(d), o);
  }
  music = std::make_unique<media::MusicEngine>(cfg, profile_getter);
  voice = std::make_unique<media::VoiceEngine>(cfg, models->catalog, profile_getter, runtimes.get());
  tools = std::make_unique<tools::ToolRegistry>(truthy(cfg.get("tools.sandbox_community_tools", true)));
  supervisor = std::make_unique<tools::Supervisor>(*tools, *db, cfg);
  // IA de apoyo: el embed multilingue ayuda a todas las herramientas
  assist = std::make_unique<mem::SemanticAssist>(embedder_getter(), &config);
  supervisor->assist = assist.get();
  proposals = std::make_unique<tools::ProposalManager>(*tools, *supervisor, learner.get());
  agents = std::make_unique<agents::AgentManager>(*this);
  graph->namer = [this](const std::string& aid) {   // ancla de cada subgrafo
    auto a = agents->get(aid);
    return a && !a->name.empty() ? a->name : aid;
  };
  learner->namer = graph->namer;
  // una aprobacion que caduco (o sobrevivio a un reinicio) se aprueba despues: la tarea se reanuda
  supervisor->on_deferred = [this](const json& row, bool approved, const std::string& approver) {
    if (agents) agents->resume_deferred(row, approved, approver);
  };
  try {   // oficina de pyme: cada agente de la oficina estrena su propio subgrafo sembrado
    office::ensure(*this);
  } catch (const std::exception& e) {
    L.warning("No se pudo sembrar el grafo de los agentes de la oficina: ", e.what());
  }
  lounge = std::make_unique<agents::Lounge>(*this);
  coop = std::make_unique<coop::CoopNode>(*this);
  remote = std::make_unique<remote::RemoteAccess>(cfg);
  vpn = std::make_unique<remote::VPN>(cfg);
  llm->remote = [this](const std::string& node, const json& messages, const llm::Opts& opts, const llm::Sink& out) {
    coop->remote_llm(node, messages, opts.o, out);
  };
}

Services::~Services() {
  try {
    shutdown();
  } catch (...) {
  }
}

mem::EmbedderPtr Services::make_embedder() {
  // el modelo GGUF de embeddings activo (Modelos > Embeddings) o, si no hay, Ollama y en ultimo caso hashing
  Config& cfg = config;
  std::string em = cfg.str("memory.embed_model", "");
  json entry = em.empty() ? json() : models->catalog.get(em);
  if (!entry.is_null() && jstr(entry, "kind") != "embed") entry = nullptr;
  int threads = (int)cfg.num("memory.embed_threads", 0);
  if (!threads) threads = std::max(1, (int)jnum(profile["llm"]["n_threads"], 2));
  return mem::get_embedder(cfg.str("memory.embedder", "auto"), cfg.str("llm.ollama_url"), cfg.str("memory.ollama_embed_model"),
                           entry,
                           [this]() -> std::optional<std::pair<fs::path, std::string>> {
                             auto f = runtimes->exe("llama", profile, "cpu");
                             if (!f) return std::nullopt;
                             return std::make_pair(f->exe, f->variant);
                           },
                           threads, (int)cfg.num("memory.embed_dims", 0));
}

mem::EmbedderPtr Services::embedder() {
  std::lock_guard<std::mutex> lk(emb_mu_);
  return embedder_;
}

mem::EmbedderGetter Services::embedder_getter() {
  return [this] { return embedder(); };
}

mem::EmbedderPtr Services::set_embedder(mem::EmbedderPtr e) {
  // cambio en caliente: nada se bloquea; lo ya indexado se recalcula en segundo plano
  mem::EmbedderPtr fresh = e ? e : make_embedder();
  mem::EmbedderPtr old;
  {
    std::lock_guard<std::mutex> lk(emb_mu_);
    old = embedder_;
    embedder_ = fresh;
  }
  memory->invalidate();
  rag->invalidate();
  if (old && old != fresh) old->close();
  L.info("Modelo de embeddings: ", fresh->id);
  embed_worker->start();
  return fresh;
}

void Services::start() {
  monitor->start();
  if (pipeline) pipeline->start();
  embed_worker->start();
  std::vector<mem::ToolView> views;
  for (auto& t : tools->for_agent({"*"})) views.push_back(t->view());
  assist->prewarm(views, agents->list());
  if (hw::android() && config.flag("hardware.adaptive_tuning", true)) {
    android_tuner = std::make_unique<android_tune::LiveTuner>(*this);
    android_tuner->start();
    std::thread([this] {
      try {
        android_tune::after_start(*this, android_tuner.get());
      } catch (const std::exception& e) {
        L.warning("Optimizacion tras el arranque: ", e.what());
      }
    }).detach();
  }
  mcp = std::make_unique<mcp::Gestor>(*this);
  if (config.get("mcp.servidores", json::array()).size()) mcp->conectar();
  if (config.flag("coop.enabled", false)) {
    try {
      coop->start();
    } catch (const std::exception& e) {
      L.warning("No se pudo iniciar el modo cooperativo: ", e.what());
    }
  }
}

void Services::shutdown() {
  if (shut_) return;
  shut_ = true;
  L.info("Apagando Agente IA...");
  hilo.reset();   // espera al plegado en curso antes de parar el LLM
  if (pipeline) pipeline->stop();   // termina la difusion en curso antes de parar los motores
  if (coop) coop->stop();
  mcp.reset();   // espera a la conexion en curso y cierra los servidores MCP hijos
  if (llm) {
    auto ls = std::dynamic_pointer_cast<llm::LlamaServerBackend>(llm->backend("llamaserver"));
    if (ls) ls->stop();
  }
  if (embed_worker) embed_worker->stop();
  if (auto e = embedder()) e->close();
  if (android_tuner) android_tuner->stop();
  if (monitor) monitor->stop();
}

// codificador de vision activo (percepcion.modelo_vision), cargado al primer uso en el proveedor de la matriz del chip
std::shared_ptr<difusion::CodificadorVision> Services::codificador_vision() {
  std::lock_guard<std::mutex> lk(vision_mu_);
  std::string mid = config.str("percepcion.modelo_vision", "");
  if (mid.empty()) return nullptr;
  json m = models->catalog.get(mid);
  if (m.is_null()) return nullptr;
  std::string ruta = jstr(m, "path");
  if (vision_ && vision_clave_ == ruta) return vision_;
  if (vision_clave_ == ruta && now() - vision_fallo_ < 300) return nullptr;   // fallo hace poco: no reintentar en cada frame
  vision_.reset();
  vision_clave_ = ruta;
  for (auto& prov : hal::matriz(profile)["vision"]) {
    auto lib = difusion::biblioteca_ort(difusion::variante_de(prov.get<std::string>()));
    if (!lib || !difusion::proveedor_disponible(*lib, prov.get<std::string>())) continue;
    try {
      vision_ = difusion::CodificadorVision::abrir(*lib, P(ruta), prov.get<std::string>());
      log::get("percepcion").info("codificador de vision ", mid, " en ", prov.get<std::string>());
      return vision_;
    } catch (const std::exception& e) {
      log::get("percepcion").warning("codificador de vision en ", prov.get<std::string>(), ": ", e.what());
    }
  }
  vision_fallo_ = now();
  return nullptr;
}

}  // namespace nx
