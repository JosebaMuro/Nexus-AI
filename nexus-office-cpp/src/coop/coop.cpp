// Red COOP (coop/node.py, coop/crypto.py). Cada enlace (entrante o saliente) lee en su propio hilo; las peticiones
// que tardan (LLM, agente) se atienden en hilos aparte para no frenar el reenvio de la malla.
#include "coop/coop.h"

#include <httplib.h>

#include <algorithm>
#include <cmath>

#include "agents/agents.h"
#include "core/config.h"
#include "core/services.h"
#include "llm/llm.h"
#include "memory/memory.h"
#include "tools/tools.h"
#include "util/crypto.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/net.h"
#include "util/text.h"

namespace nx::coop {

namespace {
auto L = log::get("coop");

std::string b64url_nopad(const std::string& raw) {
  std::string s = b64url_encode(raw);
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

std::string b64url_decode(std::string body) {
  for (auto& c : body) {
    if (c == '-') c = '+';
    if (c == '_') c = '/';
  }
  return b64decode(body);
}
}  // namespace

// ======================================================================== crypto.py
std::pair<std::string, std::string> derive(const std::string& network_id, const std::string& passphrase) {
  std::string key = scrypt(passphrase, "nexus-lan:" + network_id, 1 << 14, 8, 1, 32);
  return {key, sha256_hex("tag" + key).substr(0, 16)};   // identificador publico de la red (no revela la clave)
}

Cipher::Cipher(const std::string& network_id, const std::string& passphrase) {
  std::tie(key_, tag) = derive(network_id, passphrase);
}

std::string Cipher::seal(const json& msg) const {
  std::string nonce = random_bytes(12);
  std::string ct = aes_gcm_encrypt(key_, nonce, jdump(msg), tag);
  return jdump(json{{"v", 1}, {"net", tag}, {"n", b64encode(nonce)}, {"c", b64encode(ct)}});
}

json Cipher::open(const std::string& frame) const {
  json env = jparse(frame);
  if (!env.is_object() || env.value("v", json()) != json(1) || jstr(env, "net") != tag)
    throw std::runtime_error("trama de otra red");
  json msg = jparse(aes_gcm_decrypt(key_, b64decode(jstr(env, "n")), b64decode(jstr(env, "c")), tag));
  if (!msg.is_object()) throw std::runtime_error("trama no valida");
  return msg;
}

std::string make_invite(const std::string& network_id, const std::string& passphrase, const std::vector<std::string>& peers) {
  return "NEXUS1:" + b64url_nopad(json{{"net", network_id}, {"key", passphrase}, {"peers", peers}}.dump());
}

json read_invite(const std::string& code_in) {
  std::string code = trim(code_in);
  if (!starts_with(code, "NEXUS1:")) throw std::invalid_argument("codigo de invitacion no valido");
  json data = jparse(b64url_decode(code.substr(7)));
  if (!data.is_object() || !data.contains("net") || !data.contains("key"))
    throw std::invalid_argument("codigo de invitacion no valido");
  return data;
}

// ======================================================================== node.py
json node_identity() {
  fs::path f = paths::data_dir() / "coop_node.json";
  if (is_file(f)) {
    json j = jparse(read_text(f));
    if (j.is_object() && j.contains("node_id")) return j;
  }
  json ident = {{"node_id", uuid_hex().substr(0, 16)}};
  write_file(f, ident.dump());
  return ident;
}

std::string virtual_ip(const std::string& network_id, const std::string& node_id) {
  std::string h = sha256(network_id + ":" + node_id);
  return "10.77." + std::to_string((unsigned char)h[0]) + "." + std::to_string(1 + (unsigned char)h[1] % 254);
}

json overlay_addresses() {
  json out = json::array();
  for (auto& a : net::interfaces()) {
    if (starts_with(a.ip, "127.")) continue;
    std::string kind = "lan", iface = lower(a.name);
    auto parts = split(a.ip, ".");
    if (starts_with(a.ip, "100.") && parts.size() > 1 && std::atoi(parts[1].c_str()) >= 64 && std::atoi(parts[1].c_str()) <= 127)
      kind = "tailscale";
    else if (contains(iface, "zerotier") || starts_with(iface, "zt"))
      kind = "zerotier";
    else if (!starts_with(a.ip, "10.") && !starts_with(a.ip, "192.168.") && !starts_with(a.ip, "172.") &&
             !starts_with(a.ip, "169.254."))
      kind = "publica";
    out.push_back({{"iface", a.name}, {"ip", a.ip}, {"kind", kind}});
  }
  return out;
}

// ------------------------------------------------------------------ enlace (WebSocket entrante o saliente)
struct CoopNode::Link {
  std::mutex mu;
  std::function<bool(const std::string&)> send_fn;
  std::function<void()> close_fn;
  bool open = true;
  bool send(const std::string& frame) {
    std::lock_guard<std::mutex> lk(mu);
    if (!open) return false;
    try {
      return send_fn(frame);
    } catch (...) {
      return false;
    }
  }
  void close() {
    std::lock_guard<std::mutex> lk(mu);
    if (!open) return;
    open = false;
    try {
      close_fn();
    } catch (...) {
    }
  }
};

CoopNode::CoopNode(Services& s) : s_(s) { node_id = jstr(node_identity(), "node_id"); }

CoopNode::~CoopNode() { stop(); }

// ------------------------------------------------------------ configuracion
std::string CoopNode::network_id() const { return s_.config.str("coop.network_id"); }

std::string CoopNode::name() const {
  std::string n = s_.config.str("coop.node_name");
  if (!n.empty()) return n;
  return s_.config.str("user.assistant_name", "Nexus-Office") + "-" + node_id.substr(0, 4);
}

std::string CoopNode::vip() const {
  std::string net = network_id();
  return net.empty() ? "" : virtual_ip(net, node_id);
}

json CoopNode::create_network(const std::string& nm) {
  std::string net = nm.empty() ? "red-" + uuid_hex().substr(0, 6) : nm;
  s_.config.set("coop.network_id", net, false);
  s_.config.set("coop.passphrase", uuid_hex() + uuid_hex());
  return {{"network_id", net}};
}

std::string CoopNode::invite(const std::vector<std::string>& public_urls) {
  std::vector<std::string> urls = public_urls;
  if (urls.empty())
    for (auto& u : s_.config.get("coop.public_urls", json::array()))
      if (u.is_string()) urls.push_back(u.get<std::string>());
  return make_invite(network_id(), s_.config.str("coop.passphrase"), urls);
}

json CoopNode::join(const std::string& code) {
  json data = read_invite(code);
  s_.config.set("coop.network_id", data["net"], false);
  s_.config.set("coop.passphrase", data["key"], false);
  std::vector<std::string> peers;
  auto add = [&](const json& list) {
    if (!list.is_array()) return;
    for (auto& p : list)
      if (p.is_string() && std::find(peers.begin(), peers.end(), p.get<std::string>()) == peers.end())
        peers.push_back(p.get<std::string>());
  };
  add(s_.config.get("coop.peers", json::array()));
  add(data.value("peers", json::array()));
  s_.config.set("coop.peers", peers);
  return {{"network_id", data["net"]}, {"peers", peers}};
}

// ------------------------------------------------------------ ciclo de vida
void CoopNode::start() {
  if (running_) return;
  if (network_id().empty() || s_.config.str("coop.passphrase").empty())
    throw std::runtime_error("Crea una red o unete con un codigo de invitacion primero");
  cipher_ = std::make_unique<Cipher>(network_id(), s_.config.str("coop.passphrase"));
  stop_ = false;
  running_ = true;
  if (s_.config.flag("coop.listen", false)) {
    std::string host = s_.config.str("coop.listen_host", "0.0.0.0");
    int port = (int)s_.config.num("coop.listen_port", 8766);
    srv_ = std::make_unique<httplib::Server>();
    srv_->WebSocket("/.*", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
      auto link = std::make_shared<Link>();
      link->send_fn = [&ws](const std::string& f) { return ws.send(f); };
      link->close_fn = [&ws] { ws.close(httplib::ws::CloseStatus::PolicyViolation, "auth"); };
      ws.set_read_timeout(1);
      serve_link(link, true, [&ws](std::string& m) {
        auto r = ws.read(m);
        return r == httplib::ws::Timeout ? 0 : r == httplib::ws::Fail ? -1 : 1;
      });
    });
    if (!srv_->bind_to_port(host, port)) {
      L.error("Nodo cooperativo detenido: no se pudo abrir ", host, ":", port);
      srv_.reset();
      running_ = false;
      return;
    }
    threads_.emplace_back([this] { srv_->listen_after_bind(); });
    L.info("LAN virtual escuchando en ", host, ":", port, " (IP virtual ", vip(), ")");
  }
  for (auto& u : s_.config.get("coop.peers", json::array()))
    if (u.is_string()) threads_.emplace_back([this, url = u.get<std::string>()] { outbound(url); });
  threads_.emplace_back([this] { announce_loop(); });
}

void CoopNode::stop() {
  if (running_ || !threads_.empty()) {
    stop_ = true;
    wake_.notify_all();
    std::vector<LinkPtr> links;
    {
      std::lock_guard<std::mutex> lk(mu_);
      links = all_links_;
    }
    for (auto& l : links) l->close();
    if (srv_) srv_->stop();
    for (auto& t : threads_)
      if (t.joinable()) t.join();
    threads_.clear();
    srv_.reset();
  }
  running_ = false;
  std::lock_guard<std::mutex> lk(mu_);
  links_.clear();
  peers_.clear();
  all_links_.clear();
}

size_t CoopNode::peer_count() {
  std::lock_guard<std::mutex> lk(mu_);
  return peers_.size();
}

void CoopNode::require_running() {
  if (!running_) throw std::runtime_error("El modo cooperativo no esta activo");
}

// ------------------------------------------------------------ enlaces
void CoopNode::outbound(const std::string& url_in) {
  std::string url = url_in;
  size_t scheme = url.find("://");
  if (scheme != std::string::npos && url.find('/', scheme + 3) == std::string::npos) url += "/";   // el cliente pide ruta
  int delay = 2;
  while (!stop_) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      link_urls_[url_in] = "conectando";
    }
    try {
      httplib::ws::WebSocketClient cli(url);
      if (!cli.is_valid()) throw std::runtime_error("direccion no valida (usa ws:// o wss://)");
      cli.set_connection_timeout(10);
      cli.set_read_timeout(1);
      auto res = cli.connect();
      if (!res) throw std::runtime_error(httplib::to_string(res.error()));
      {
        std::lock_guard<std::mutex> lk(mu_);
        link_urls_[url_in] = "conectado";
      }
      delay = 2;
      auto link = std::make_shared<Link>();
      link->send_fn = [&cli](const std::string& f) { return cli.send(f); };
      link->close_fn = [&cli] { cli.close(httplib::ws::CloseStatus::PolicyViolation, "auth"); };
      serve_link(link, false, [&cli](std::string& m) {
        auto r = cli.read(m);
        return r == httplib::ws::Timeout ? 0 : r == httplib::ws::Fail ? -1 : 1;
      });
      link->close();
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lk(mu_);
      link_urls_[url_in] = "error: " + uleft(e.what(), 80);
    }
    std::unique_lock<std::mutex> lk(wake_mu_);
    wake_.wait_for(lk, std::chrono::seconds(delay), [this] { return stop_.load(); });
    delay = std::min(60, delay * 2);
  }
}

void CoopNode::serve_link(const LinkPtr& link, bool inbound, const std::function<int(std::string&)>& read) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    all_links_.push_back(link);
  }
  std::string neighbor;
  link->send(cipher_->seal(envelope("hello", presence(), "*", 1)));
  while (!stop_) {
    std::string frame;
    int r = read(frame);
    if (r == 0) continue;
    if (r < 0) break;
    json msg;
    try {
      msg = cipher_->open(frame);
    } catch (const std::exception&) {
      L.warning("Trama no autenticada descartada (", inbound ? "entrante" : "saliente", ")");
      link->close();
      break;
    }
    if (jstr(msg, "type") == "hello" && neighbor.empty()) {
      neighbor = jstr(msg, "src");
      json peer;
      {
        std::lock_guard<std::mutex> lk(mu_);
        links_[neighbor] = link;
        update_peer(msg, 1, neighbor);
        peer = peers_[neighbor];
      }
      notify({{"type", "peer_join"}, {"peer", peer}});
      continue;
    }
    try {
      receive(msg, link);
    } catch (const std::exception& e) {
      L.warning("Trama mal formada: ", e.what());
    }
  }
  bool left = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    all_links_.erase(std::remove(all_links_.begin(), all_links_.end(), link), all_links_.end());
    if (!neighbor.empty() && links_.count(neighbor) && links_[neighbor] == link) {
      links_.erase(neighbor);
      if (peers_.count(neighbor)) peers_[neighbor]["direct"] = false;
      left = true;
    }
  }
  if (left) notify({{"type", "peer_leave"}, {"node", neighbor}});
}

// ------------------------------------------------------------ mensajeria
json CoopNode::envelope(const std::string& type, const json& body, const std::string& dst, int ttl, const std::string& rid) {
  return {{"id", uuid_hex()},
          {"src", node_id},
          {"dst", dst},
          {"ttl", ttl ? ttl : (int)s_.config.num("coop.max_hops", 6)},
          {"ts", now()},
          {"type", type},
          {"body", body},
          {"rid", rid}};
}

void CoopNode::send(const json& msg, const Link* exclude) {
  mark_seen(jstr(msg, "id"));
  std::string frame = cipher_->seal(msg);
  std::vector<LinkPtr> targets;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = links_.find(jstr(msg, "dst"));
    if (it != links_.end())
      targets.push_back(it->second);
    else
      for (auto& [k, l] : links_) targets.push_back(l);
  }
  for (auto& l : targets)
    if (l.get() != exclude) l->send(frame);
}

bool CoopNode::mark_seen(const std::string& id) {
  std::lock_guard<std::mutex> lk(mu_);
  if (seen_.count(id)) return false;
  seen_[id] = now();
  seen_order_.push_back(id);
  while (seen_order_.size() > 5000) {
    seen_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  return true;
}

void CoopNode::receive(const json& msg, const LinkPtr& from) {
  double ts = msg.value("ts", json(0)).is_number() ? msg["ts"].get<double>() : 0;
  if (std::fabs(now() - ts) > 300 || !mark_seen(jstr(msg, "id"))) return;   // repetido o fuera de ventana (anti-replay)
  std::string dst = jstr(msg, "dst");
  int ttl = msg.value("ttl", json(0)).is_number() ? msg["ttl"].get<int>() : 0;
  if (dst != node_id && ttl > 1) {   // reenvio en la malla
    json fwd = msg;
    fwd["ttl"] = ttl - 1;
    std::string frame = cipher_->seal(fwd);
    std::vector<LinkPtr> targets;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = links_.find(dst);
      if (it != links_.end())
        targets.push_back(it->second);
      else
        for (auto& [k, l] : links_)
          if (l != from) targets.push_back(l);
    }
    for (auto& l : targets) l->send(frame);
  }
  if (dst != "*" && dst != node_id) return;
  std::string type = jstr(msg, "type");
  if (starts_with(type, "req.") || starts_with(type, "share.")) {
    std::thread([this, msg] { handle(msg); }).detach();   // LLM y agentes tardan: fuera del hilo del enlace
  } else {
    handle(msg);
  }
}

void CoopNode::handle(const json& msg) {
  std::string type = jstr(msg, "type"), src = jstr(msg, "src");
  json body = msg.value("body", json::object());
  auto who = [&] {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(src);
    return it != peers_.end() && it->second.contains("name") ? jstr(it->second, "name") : src;
  };
  auto reply = [&](const json& b) { send(envelope("res", b, src, 0, jstr(msg, "rid"))); };
  try {
    if (type == "announce") {
      int hops = (int)s_.config.num("coop.max_hops", 6) - msg.value("ttl", 0) + 1;
      bool fresh;
      json peer;
      {
        std::lock_guard<std::mutex> lk(mu_);
        fresh = !peers_.count(src);
        update_peer(msg, hops, "");
        peer = peers_[src];
      }
      if (fresh) notify({{"type", "peer_join"}, {"peer", peer}});
    } else if (type == "chat") {
      json item = {{"from", body.value("name", json())}, {"node", src}, {"text", uleft(jstr(body, "text"), 2000)},
                   {"ts", msg["ts"]}};
      {
        std::lock_guard<std::mutex> lk(mu_);
        chat_.push_back(item);
        while (chat_.size() > 200) chat_.pop_front();
      }
      notify({{"type", "lan_chat"}, {"message", item}});
    } else if (type == "req.ping") {
      reply({{"pong", true}, {"final", true}, {"name", name()}});
    } else if (type == "req.llm") {
      if (!s_.config.flag("coop.allow_remote_inference", true)) throw PermissionError("este nodo no ofrece inferencia remota");
      notify({{"type", "remote_use"}, {"from", who()}, {"what", "LLM"}});
      json opts = json::object();
      const json req_opts = body.value("opts", json::object());   // items() de un temporal queda colgando
      for (auto& [k, v] : req_opts.items())
        if (k == "temperature" || k == "max_tokens") opts[k] = v;
      json messages = body.value("messages", json::array());
      respond_stream(msg, [this, opts, messages](const std::function<void(const std::string&)>& out) {
        llm::Opts o;
        o.o = opts;
        s_.llm->stream(messages, "default", o, out);
      });
    } else if (type == "req.agent") {
      if (!s_.config.flag("coop.allow_remote_agents", false)) throw PermissionError("este nodo no ofrece agentes remotos");
      std::string w = who(), agent = jstr(body, "agent", "auto"), text = jstr(body, "text");
      notify({{"type", "remote_use"}, {"from", w}, {"what", "agente " + agent}});
      respond_stream(msg, [this, w, agent, text](const std::function<void(const std::string&)>& out) {
        std::string sid = s_.memory->ensure_session("", "[LAN] " + w);
        tools::Emit emit = [this, w](const json& e) {
          if (jstr(e, "type") == "approval_required") {
            json ev = e;
            ev["remote_from"] = w;
            notify(ev);
          }
        };
        json res = s_.agents->run(agent, text, sid, {}, emit, true, 0, false);
        out(jstr(res, "answer"));
      });
    } else if (type == "share.memory") {
      if (!s_.config.flag("coop.share_memory", false)) return;
      std::string w = who();
      json facts = body.value("facts", json::array());
      for (size_t i = 0; i < facts.size() && i < 50; ++i)
        s_.memory->remember(uleft(facts[i].is_string() ? facts[i].get<std::string>() : facts[i].dump(), 500), "note", 0.4,
                            "peer:" + w);
      notify({{"type", "memory_shared"}, {"from", w}, {"count", facts.size()}});
    } else if (type == "share.tool") {
      if (!s_.config.flag("coop.share_tools", true)) return;
      std::string w = who();
      json prop = s_.proposals->create(jstr(body, "name"), jstr(body, "description"), jstr(body, "code"), json::object(),
                                       jstr(body, "risk", "medium"),
                                       "Compartida por el nodo " + w + ". " + jstr(body, "rationale"), "peer:" + w);
      notify({{"type", "tool_shared"}, {"from", w}, {"proposal", prop["id"]}});
    } else if (type == "res") {
      std::shared_ptr<Pending> p;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_.find(jstr(msg, "rid"));
        if (it != pending_.end()) p = it->second;
      }
      if (p) {
        std::lock_guard<std::mutex> lk(p->mu);
        p->items.push_back(body);
        p->cv.notify_all();
      }
    }
  } catch (const std::exception& e) {
    L.warning("Error procesando ", type, ": ", e.what());
    if (!jstr(msg, "rid").empty() && starts_with(type, "req")) reply({{"error", e.what()}, {"final", true}});
  }
}

// ------------------------------------------------------------ presencia
json CoopNode::presence() {
  const json& prof = s_.profile;
  json llm = s_.llm->status();
  return {{"name", name()},
          {"vip", vip()},
          {"version", NEXUS_VERSION},
          {"hardware",
           {{"tier", prof.value("tier", json())},
            {"accelerator", prof.value("accelerator", json())},
            {"device", prof.value("device_name", json())},
            {"ram_gb", prof.value("ram_gb", json())},
            {"vram_gb", prof.value("vram_gb", json())}}},
          {"llm", {{"backend", llm.value("backend", json())}, {"model", llm.value("model", json())}}},
          {"agents", s_.agents->ids()},
          {"offers",
           {{"inference", s_.config.flag("coop.allow_remote_inference", true)},
            {"agents", s_.config.flag("coop.allow_remote_agents", false)},
            {"memory", s_.config.flag("coop.share_memory", false)},
            {"tools", s_.config.flag("coop.share_tools", true)}}}};
}

// con mu_ tomado
void CoopNode::update_peer(const json& msg, int hops, const std::string& via) {
  std::string src = jstr(msg, "src");
  json info = msg.value("body", json::object());
  if (!info.is_object()) info = json::object();
  info["node_id"] = src;
  info["last_seen"] = now();
  info["hops"] = hops;
  info["via"] = via;
  info["direct"] = links_.count(src) > 0;
  peers_[src] = info;
}

void CoopNode::announce_loop() {
  while (!stop_) {
    try {
      send(envelope("announce", presence()));
    } catch (const std::exception& e) {
      L.warning("Anuncio fallido: ", e.what());
    }
    std::vector<std::string> gone;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto it = peers_.begin(); it != peers_.end();) {
        if (now() - it->second.value("last_seen", 0.0) > 90) {
          gone.push_back(it->first);
          it = peers_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& g : gone) notify({{"type", "peer_leave"}, {"node", g}});
    std::unique_lock<std::mutex> lk(wake_mu_);
    wake_.wait_for(lk, std::chrono::seconds(15), [this] { return stop_.load(); });
  }
}

// ------------------------------------------------------------ servicios remotos
void CoopNode::respond_stream(const json& msg,
                              const std::function<void(const std::function<void(const std::string&)>&)>& gen) {
  std::string src = jstr(msg, "src"), rid = jstr(msg, "rid"), buf;
  double last = now();
  auto flush = [&] {
    if (buf.empty()) return;
    send(envelope("res", {{"chunk", buf}}, src, 0, rid));
    buf.clear();
    last = now();
  };
  json fin = {{"final", true}};
  try {
    gen([&](const std::string& piece) {
      buf += piece;
      if (now() - last > 0.15) flush();
    });
  } catch (const std::exception& e) {
    fin["error"] = e.what();
  }
  flush();
  send(envelope("res", fin, src, 0, rid));
}

std::string CoopNode::resolve(const std::string& ref) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [nid, p] : peers_)
    if (ref == nid || ref == jstr(p, "name") || ref == jstr(p, "vip")) return nid;
  throw std::out_of_range("nodo no encontrado en la LAN virtual: " + ref);
}

void CoopNode::request_stream(const std::string& ref, const std::string& type, const json& body, double timeout,
                              const std::function<void(const std::string&)>& on_chunk) {
  std::string nid = resolve(ref);
  require_running();
  std::string rid = uuid_hex();
  auto p = std::make_shared<Pending>();
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_[rid] = p;
  }
  struct Drop {
    CoopNode* n;
    std::string rid;
    ~Drop() {
      std::lock_guard<std::mutex> lk(n->mu_);
      n->pending_.erase(rid);
    }
  } drop{this, rid};
  send(envelope(type, body, nid, 0, rid));
  while (true) {
    json item;
    {
      std::unique_lock<std::mutex> lk(p->mu);
      if (!p->cv.wait_for(lk, std::chrono::duration<double>(timeout), [&] { return !p->items.empty(); }))
        throw std::runtime_error("nodo remoto: tiempo agotado");
      item = p->items.front();
      p->items.pop_front();
    }
    if (truthy(item.value("error", json()))) throw std::runtime_error("nodo remoto: " + jstr(item, "error"));
    if (truthy(item.value("chunk", json())) && on_chunk) on_chunk(jstr(item, "chunk"));
    if (truthy(item.value("final", json()))) return;
  }
}

void CoopNode::remote_llm(const std::string& ref, const json& messages, const json& opts,
                          const std::function<void(const std::string&)>& out) {
  request_stream(ref, "req.llm", {{"messages", messages}, {"opts", opts}}, 300, out);
}

std::string CoopNode::remote_agent(const std::string& ref, const std::string& agent, const std::string& text) {
  std::string out;
  request_stream(ref, "req.agent", {{"agent", agent}, {"text", text}}, 600, [&](const std::string& c) { out += c; });
  return out;
}

double CoopNode::ping(const std::string& ref) {
  double t0 = now();
  request_stream(ref, "req.ping", json::object(), 10, nullptr);
  return std::round((now() - t0) * 1000 * 10) / 10;
}

json CoopNode::send_chat(const std::string& text) {
  json item = {{"from", name()}, {"node", node_id}, {"text", uleft(text, 2000)}, {"ts", now()}, {"own", true}};
  {
    std::lock_guard<std::mutex> lk(mu_);
    chat_.push_back(item);
    while (chat_.size() > 200) chat_.pop_front();
  }
  require_running();
  send(envelope("chat", {{"name", name()}, {"text", uleft(text, 2000)}}));
  return item;
}

void CoopNode::share_facts(const std::vector<std::string>& facts) {
  require_running();
  std::vector<std::string> f(facts.begin(), facts.begin() + std::min<size_t>(50, facts.size()));
  send(envelope("share.memory", {{"facts", f}}));
}

void CoopNode::share_proposal(const json& prop) {
  require_running();
  json b = json::object();
  for (auto* k : {"name", "description", "code", "rationale"}) b[k] = prop.value(k, json());
  b["risk"] = prop.value("declared_risk", json("medium"));
  send(envelope("share.tool", b));
}

void CoopNode::notify(const json& event) {
  for (auto& cb : listeners) {
    try {
      cb({{"type", "coop"}, {"event", event}});
    } catch (...) {
    }
  }
}

json CoopNode::status() {
  int port = (int)s_.config.num("coop.listen_port", 8766);
  json addresses = json::array();
  for (auto a : overlay_addresses()) {
    a["url"] = "ws://" + jstr(a, "ip") + ":" + std::to_string(port);
    addresses.push_back(a);
  }
  std::lock_guard<std::mutex> lk(mu_);
  json links = json::array(), peers = json::array(), chat = json::array();
  for (auto& [k, l] : links_) links.push_back(k);
  std::vector<json> pl;
  for (auto& [k, p] : peers_) pl.push_back(p);
  std::stable_sort(pl.begin(), pl.end(), [](const json& a, const json& b) { return jstr(a, "name") < jstr(b, "name"); });
  for (auto& p : pl) peers.push_back(p);
  size_t from = chat_.size() > 50 ? chat_.size() - 50 : 0;
  for (size_t i = from; i < chat_.size(); ++i) chat.push_back(chat_[i]);
  json outbound = json::object();
  for (auto& [k, v] : link_urls_) outbound[k] = v;
  return {{"running", running_.load()},
          {"node_id", node_id},
          {"name", name()},
          {"vip", vip()},
          {"network_id", network_id()},
          {"configured", !network_id().empty() && !s_.config.str("coop.passphrase").empty()},
          {"listen", s_.config.get("coop.listen", false)},
          {"port", port},
          {"links", links},
          {"outbound", outbound},
          {"peers", peers},
          {"chat", chat},
          {"addresses", addresses}};
}

}  // namespace nx::coop
