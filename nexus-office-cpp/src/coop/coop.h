// Red COOP (coop/node.py y coop/crypto.py): LAN virtual en malla entre instalaciones de Nexus-Office por internet.
// Tramas AES-256-GCM con clave scrypt de la frase de red, WebSocket en un puerto propio, reenvio por inundacion con
// TTL y deduplicacion, IP virtual 10.77.x.y, presencia, chat, ping, LLM y agentes remotos y memoria/herramientas
// compartidas. El formato de las tramas es el de la version Python: los nodos de las dos ediciones se entienden.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/json.h"

namespace httplib {
class Server;
}

namespace nx {
class Services;
}

namespace nx::coop {

constexpr size_t MAX_FRAME = 16 * 1024 * 1024;

// ---------------------------------------------------------------- crypto.py
std::pair<std::string, std::string> derive(const std::string& network_id, const std::string& passphrase);   // (clave, tag)

class Cipher {
 public:
  Cipher(const std::string& network_id, const std::string& passphrase);
  std::string seal(const json& msg) const;
  json open(const std::string& frame) const;   // lanza std::runtime_error si no es de esta red o no autentica
  std::string tag;

 private:
  std::string key_;
};

std::string make_invite(const std::string& network_id, const std::string& passphrase, const std::vector<std::string>& peers);
json read_invite(const std::string& code);   // lanza std::invalid_argument

// ---------------------------------------------------------------- node.py
json node_identity();
std::string virtual_ip(const std::string& network_id, const std::string& node_id);
json overlay_addresses();   // [{iface, ip, kind}] (lan, tailscale, zerotier, publica)

class CoopNode {
 public:
  explicit CoopNode(Services& s);
  ~CoopNode();

  std::string node_id;
  std::string network_id() const;
  std::string name() const;
  std::string vip() const;

  json create_network(const std::string& name = "");
  std::string invite(const std::vector<std::string>& public_urls = {});
  json join(const std::string& code);

  void start();   // lanza std::runtime_error si no hay red configurada
  void stop();
  bool running() const { return running_; }
  size_t peer_count();

  // API sincrona (servidor y LLM). Lanzan std::runtime_error ("nodo remoto: ...") o std::out_of_range (nodo desconocido)
  double ping(const std::string& ref);
  std::string remote_agent(const std::string& ref, const std::string& agent, const std::string& text);
  void remote_llm(const std::string& ref, const json& messages, const json& opts,
                  const std::function<void(const std::string&)>& out);
  json send_chat(const std::string& text);
  void share_facts(const std::vector<std::string>& facts);
  void share_proposal(const json& prop);

  json status();
  std::vector<std::function<void(const json&)>> listeners;   // {"type": "coop", "event": ...}

 private:
  struct Link;
  using LinkPtr = std::shared_ptr<Link>;
  struct Pending {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<json> items;
  };

  Services& s_;
  std::unique_ptr<Cipher> cipher_;
  std::atomic<bool> running_{false}, stop_{false};
  std::unique_ptr<httplib::Server> srv_;
  std::vector<std::thread> threads_;
  std::mutex mu_, wake_mu_;
  std::condition_variable wake_;
  std::map<std::string, LinkPtr> links_;            // vecino -> enlace
  std::map<std::string, std::string> link_urls_;    // url saliente -> estado
  std::map<std::string, json> peers_;
  std::map<std::string, double> seen_;
  std::deque<std::string> seen_order_;
  std::map<std::string, std::shared_ptr<Pending>> pending_;
  std::deque<json> chat_;
  std::vector<LinkPtr> all_links_;

  json envelope(const std::string& type, const json& body, const std::string& dst = "*", int ttl = 0,
                const std::string& rid = "");
  void send(const json& msg, const Link* exclude = nullptr);
  bool mark_seen(const std::string& id);
  void serve_link(const LinkPtr& link, bool inbound, const std::function<int(std::string&)>& read);
  void receive(const json& msg, const LinkPtr& from);
  void handle(const json& msg);
  json presence();
  void update_peer(const json& msg, int hops, const std::string& via);
  void outbound(const std::string& url);
  void announce_loop();
  void respond_stream(const json& msg, const std::function<void(const std::function<void(const std::string&)>&)>& gen);
  void request_stream(const std::string& ref, const std::string& type, const json& body, double timeout,
                      const std::function<void(const std::string&)>& on_chunk);
  std::string resolve(const std::string& ref);
  void notify(const json& event);
  void require_running();
};

}  // namespace nx::coop
