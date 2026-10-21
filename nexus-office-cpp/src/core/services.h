// Ensamblado de todos los servicios de Nexus-Office (core.py).
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/db.h"
#include "hardware/hardware.h"
#include "memory/memory.h"
#include "util/json.h"

namespace nx::runtimes {
class RuntimeManager;
}
namespace nx::models {
class ModelManager;
}
namespace nx::llm {
class LLMManager;
}
namespace nx::rag {
class RAGStore;
}
namespace nx::tools {
class ToolRegistry;
class Supervisor;
class ProposalManager;
}  // namespace nx::tools
namespace nx::agents {
class AgentManager;
class Lounge;
}  // namespace nx::agents
namespace nx::media {
class ImageEngine;
class MusicEngine;
class VoiceEngine;
}  // namespace nx::media

namespace nx::coop {
class CoopNode;
}
namespace nx::remote {
class RemoteAccess;
class VPN;
}  // namespace nx::remote

namespace nx {

class Services {
 public:
  explicit Services(bool detect_hardware = true);
  ~Services();
  Services(const Services&) = delete;
  Services& operator=(const Services&) = delete;

  void start();
  void shutdown();

  // embeddings (se cambian en caliente)
  mem::EmbedderPtr embedder();
  mem::EmbedderPtr set_embedder(mem::EmbedderPtr e = nullptr);
  mem::EmbedderGetter embedder_getter();

  Config& config;
  std::unique_ptr<hw::Monitor> monitor;
  hw::HardwareInfo hw;
  json profile;
  std::vector<std::string> tuning_reasons;
  std::unique_ptr<runtimes::RuntimeManager> runtimes;
  std::unique_ptr<DB> db;
  std::unique_ptr<models::ModelManager> models;
  std::unique_ptr<mem::MemoryStore> memory;
  std::unique_ptr<mem::KnowledgeGraph> graph;
  std::unique_ptr<mem::Organizer> organizer;
  std::unique_ptr<mem::Learner> learner;
  std::unique_ptr<rag::RAGStore> rag;
  std::unique_ptr<mem::EmbedWorker> embed_worker;
  std::unique_ptr<llm::LLMManager> llm;
  std::unique_ptr<media::ImageEngine> images;
  std::unique_ptr<media::MusicEngine> music;
  std::unique_ptr<media::VoiceEngine> voice;
  std::unique_ptr<tools::ToolRegistry> tools;
  std::unique_ptr<tools::Supervisor> supervisor;
  std::unique_ptr<mem::SemanticAssist> assist;
  std::unique_ptr<tools::ProposalManager> proposals;
  std::unique_ptr<agents::AgentManager> agents;
  std::unique_ptr<agents::Lounge> lounge;
  std::unique_ptr<coop::CoopNode> coop;
  std::unique_ptr<remote::RemoteAccess> remote;
  std::unique_ptr<remote::VPN> vpn;

 private:
  std::mutex emb_mu_;
  mem::EmbedderPtr embedder_;
  bool shut_ = false;
  mem::EmbedderPtr make_embedder();
};

}  // namespace nx
