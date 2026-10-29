// Acceso remoto por internet (remote/__init__.py, devices.py, tlscert.py, upnp.py, router.py) y VPN WireGuard
// (remote/vpn.py). El puerto local sigue igual; el remoto (HTTPS, 8443) sirve la misma aplicacion detras de una
// puerta que exige la llave de un dispositivo emparejado, venga de donde venga.
#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "util/fs.h"
#include "util/json.h"

namespace nx {
class Config;
}

namespace nx::remote {

extern const char* COOKIE;            // "nexus_dev"
extern const char* CRYPTO_TEXT;
bool open_path(const std::string& path);   // lo que el puerto remoto deja ver sin emparejar
extern const std::vector<std::pair<std::string, std::string>> SECURITY_HEADERS;

// ---------------------------------------------------------------- devices.py
std::string normalize_code(const std::string& code);

class RateLimiter {   // fallos por clave (IP) en una ventana; al superar el maximo, bloqueo hasta que caduquen
 public:
  explicit RateLimiter(int max_fails = 8, double window_s = 900) : max_(max_fails), window_(window_s) {}
  bool blocked(const std::string& key);
  void fail(const std::string& key);
  void reset(const std::string& key);

 private:
  int max_;
  double window_;
  std::mutex mu_;
  std::map<std::string, std::vector<double>> fails_;
  std::vector<double>& recent(const std::string& key);
};

class DeviceStore {
 public:
  explicit DeviceStore(const fs::path& folder);
  json new_pairing(const std::string& name = "");
  // canjea el codigo (una sola vez) por (llave, dispositivo)
  std::optional<std::pair<std::string, json>> pair(const std::string& code, const std::string& device_name,
                                                   const std::string& ip);
  std::string check(const std::string& token, const std::string& ip = "");   // id del dispositivo o ""
  json list();
  bool revoke(const std::string& dev_id);

 private:
  fs::path path_;
  std::mutex mu_;
  std::map<std::string, json> pending_;
  json data_;
  std::map<std::string, std::string> by_hash_;
  double last_write_ = 0;
  void save();
};

// ---------------------------------------------------------------- tlscert.py
std::string pretty(const std::string& fp);
// (cert.pem, key.pem, huella). Lo crea la primera vez y lo conserva (la huella anclada no cambia)
std::tuple<fs::path, fs::path, std::string> ensure_cert(const fs::path& folder, const std::vector<std::string>& extra,
                                                        bool regenerate = false);

// ---------------------------------------------------------------- upnp.py
bool is_cgnat_or_private(const std::string& ip);
std::string parse_ssdp(const std::string& reply);
std::optional<std::pair<std::string, std::string>> parse_description(const std::string& xml, const std::string& location);
std::string soap_body(const std::string& action, const std::string& service,
                      const std::vector<std::pair<std::string, std::string>>& args);
std::string parse_soap_value(const std::string& xml, const std::string& field);

class Gateway {   // cliente UPnP-IGD minimo (lanza std::runtime_error)
 public:
  Gateway(std::string control_url, std::string service) : control_url(std::move(control_url)), service(std::move(service)) {}
  static Gateway discover(double timeout = 2.5);
  std::string external_ip();
  std::string add_mapping(int port, int lease_s = 0, const std::string& protocol = "TCP");
  void delete_mapping(int port, const std::string& protocol = "TCP");
  std::string control_url, service;

 private:
  std::string call(const std::string& action, const std::vector<std::pair<std::string, std::string>>& args);
};

// ---------------------------------------------------------------- router.py
std::string identify(const std::vector<std::string>& texts);
std::string parse_route_print(const std::string& text);
std::string parse_proc_route(const std::string& text);
std::string default_gateway();
json interface_for(const std::string& ip);
json detect(int port);
bool reachable_from_outside(const std::string& public_ip, int port, double timeout = 4);
std::string public_ip_lookup();   // api.ipify.org (lanza std::runtime_error)

// ---------------------------------------------------------------- __init__.py
class RemoteAccess {
 public:
  explicit RemoteAccess(Config& cfg);
  fs::path folder;
  DeviceStore devices;
  RateLimiter limiter;
  std::string public_ip;
  bool behind_cgnat = false;
  json upnp = {{"mapped", false}, {"external_ip", ""}, {"error", ""}};
  std::atomic<int> listening_port{0};   // 0: el puerto remoto no esta abierto

  std::tuple<fs::path, fs::path, std::string> cert(bool regenerate = false);
  std::string fingerprint();
  int port();
  json urls();
  std::string invite(const json& pairing, const std::string& name = "");
  json status();
  std::string pair_page();   // pagina de emparejamiento con la huella

 private:
  Config& cfg_;
  std::mutex mu_;
  std::string fp_;
};
json read_invite(const std::string& code);

// ---------------------------------------------------------------- vpn.py
extern const char* IFACE;   // "nexusvpn"
std::string encode_vpn_code(const json& data);
json decode_vpn_code(const std::string& code);
std::string platform_name();   // windows | macos | wsl | linux

class VPN {
 public:
  explicit VPN(Config& cfg);
  json state;
  fs::path folder, state_file, conf_file;
  struct Net {
    uint32_t base;
    int prefix;
  };
  Net network();
  std::string net_text();
  std::string server_ip();
  int port();
  int http_port();
  void ensure_server_keys();
  std::string server_conf();
  fs::path write_conf();
  std::string client_conf(const json& peer, const std::string& priv, const std::string& endpoint);
  json endpoints(const std::string& public_ip = "");
  json add_peer(const std::string& name, const std::string& lan_key, const std::string& public_ip = "");
  bool remove_peer(const std::string& peer_id);
  bool tunnel_up();
  std::string wireguard_tool();
  json instructions();
  json apply();
  json status();

 private:
  Config& cfg_;
  std::mutex mu_;
  void save();
  std::string next_ip();
  static json pub(const json& p);
};

}  // namespace nx::remote
