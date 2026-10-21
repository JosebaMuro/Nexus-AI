// VPN cifrada (WireGuard) entre la app Android «Nexus-Office VPN» y este PC (remote/vpn.py). Nexus-Office genera las
// claves, el fichero del tunel (data/vpn/nexusvpn.conf, formato wg-quick) y un codigo NEXUSV1:... por dispositivo; el
// tunel lo levanta WireGuard oficial. Noise IK con Curve25519, ChaCha20-Poly1305 y BLAKE2s mas una clave
// precompartida de 256 bits por dispositivo.
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>
#include <thread>

#include "coop/coop.h"
#include "core/config.h"
#include "remote/remote.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/text.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace nx::remote {

namespace {
auto L = log::get("vpn");
const char* CODE_PREFIX = "NEXUSV1:";
constexpr int KEEPALIVE = 25;

std::string ip_text(uint32_t v) {
  return std::to_string(v >> 24) + "." + std::to_string((v >> 16) & 255) + "." + std::to_string((v >> 8) & 255) + "." +
         std::to_string(v & 255);
}

std::optional<uint32_t> ip_value(const std::string& ip) {
  in_addr a{};
  if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return std::nullopt;
  return ntohl(a.s_addr);
}

void make_private(const fs::path& p) {
  try {
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  } catch (...) {
  }
}

bool is_admin() {
#ifdef _WIN32
  return IsUserAnAdmin() != FALSE;
#else
  return geteuid() == 0;
#endif
}
}  // namespace

const char* IFACE = "nexusvpn";   // nombre del tunel (y del fichero .conf) en el PC

std::string encode_vpn_code(const json& data) {
  std::string s = b64url_encode(data.dump());
  while (!s.empty() && s.back() == '=') s.pop_back();
  return CODE_PREFIX + s;
}

json decode_vpn_code(const std::string& code) {
  std::string body = trim(code).substr(std::char_traits<char>::length(CODE_PREFIX));
  for (auto& c : body) c = c == '-' ? '+' : c == '_' ? '/' : c;
  return jparse(b64decode(body));
}

std::string platform_name() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  if (contains(lower(read_text("/proc/version")), "microsoft")) return "wsl";
  return "linux";
#endif
}

// ------------------------------------------------------------------ estado
VPN::VPN(Config& cfg) : cfg_(cfg) {
  folder = paths::sub({"vpn"});
  state_file = folder / "vpn.json";
  conf_file = folder / (std::string(IFACE) + ".conf");
  state = {{"server_private", ""}, {"server_public", ""}, {"peers", json::array()}};
  if (is_file(state_file)) {
    json j = jparse(read_text(state_file));
    if (j.is_object() && j.value("peers", json()).is_array())
      state = j;
    else
      L.warning("vpn.json ilegible: se regenera");
  }
}

void VPN::save() {
  write_file(state_file, state.dump(1));
  make_private(state_file);
}

VPN::Net VPN::network() {
  std::string text = cfg_.str("vpn.subnet", "10.78.0.0/24");
  auto parts = split(trim(text), "/");
  auto ip = ip_value(parts[0]);
  int prefix = parts.size() > 1 ? std::atoi(parts[1].c_str()) : 32;
  if (!ip || prefix < 0 || prefix > 29 || (parts.size() > 1 && parts[1].find_first_not_of("0123456789") != std::string::npos))
    throw std::invalid_argument("vpn.subnet debe ser una red IPv4 privada de /29 o mayor (por ejemplo 10.78.0.0/24)");
  uint32_t mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
  return {*ip & mask, prefix};
}

std::string VPN::net_text() {
  auto n = network();
  return ip_text(n.base) + "/" + std::to_string(n.prefix);
}

std::string VPN::server_ip() { return ip_text(network().base + 1); }
int VPN::port() { return (int)cfg_.num("vpn.port", 51820); }
int VPN::http_port() { return (int)cfg_.num("server.port", 8775); }

void VPN::ensure_server_keys() {
  if (jstr(state, "server_private").empty()) {
    auto [priv, pub] = x25519_keypair();
    state["server_private"] = b64encode(priv);
    state["server_public"] = b64encode(pub);
    save();
  }
}

// ------------------------------------------------------------------ ficheros
std::string VPN::server_conf() {
  ensure_server_keys();
  auto n = network();
  std::vector<std::string> lines = {
      "# Nexus-Office VPN (WireGuard) - tunel de este PC. Lo genera Nexus-Office: no lo edites a mano.",
      "# Importalo en WireGuard (Windows/macOS) o usalo con wg-quick (Linux/macOS). Red " + net_text() + ".",
      "[Interface]",
      "PrivateKey = " + jstr(state, "server_private"),
      "Address = " + server_ip() + "/" + std::to_string(n.prefix),
      "ListenPort = " + std::to_string(port()),
      ""};
  for (auto& p : state["peers"]) {
    lines.push_back("# " + jstr(p, "name") + " (" + strftime_local("%Y-%m-%d", p.value("created", 0.0)) + ")");
    lines.push_back("[Peer]");
    lines.push_back("PublicKey = " + jstr(p, "public"));
    lines.push_back("PresharedKey = " + jstr(p, "psk"));
    lines.push_back("AllowedIPs = " + jstr(p, "ip") + "/32");
    lines.push_back("");
  }
  return join(lines, "\n");
}

fs::path VPN::write_conf() {
  write_file(conf_file, server_conf());
  make_private(conf_file);
  return conf_file;
}

std::string VPN::client_conf(const json& peer, const std::string& priv, const std::string& endpoint) {
  return join({"# Nexus-Office VPN - dispositivo «" + jstr(peer, "name") + "» (para la app WireGuard oficial)", "[Interface]",
               "PrivateKey = " + priv, "Address = " + jstr(peer, "ip") + "/32", "", "[Peer]",
               "PublicKey = " + jstr(state, "server_public"), "PresharedKey = " + jstr(peer, "psk"), "Endpoint = " + endpoint,
               "AllowedIPs = " + server_ip() + "/32", "PersistentKeepalive = " + std::to_string(KEEPALIVE), ""},
              "\n");
}

// ------------------------------------------------------------------ dispositivos
std::string VPN::next_ip() {
  auto n = network();
  std::set<std::string> used = {server_ip()};
  for (auto& p : state["peers"]) used.insert(jstr(p, "ip"));
  uint32_t count = n.prefix ? (1u << (32 - n.prefix)) : 0;
  for (uint32_t h = 1; h + 1 < count; ++h) {
    std::string ip = ip_text(n.base + h);
    if (!used.count(ip)) return ip;
  }
  throw std::invalid_argument("no quedan direcciones libres en la red de la VPN (amplia vpn.subnet)");
}

json VPN::endpoints(const std::string& public_ip) {
  int p = port();
  std::vector<std::string> out;
  for (auto& e : cfg_.get("vpn.endpoints", json::array())) {
    if (!e.is_string()) continue;
    std::string s = e.get<std::string>();
    size_t br = s.rfind(']');
    std::string tail = br == std::string::npos ? s : s.substr(br + 1);
    out.push_back(contains(tail, ":") ? s : s + ":" + std::to_string(p));
  }
  if (!public_ip.empty()) out.push_back(public_ip + ":" + std::to_string(p));
  auto n = network();
  uint32_t mask = n.prefix ? 0xFFFFFFFFu << (32 - n.prefix) : 0;
  std::vector<json> addrs;
  for (auto& a : coop::overlay_addresses()) addrs.push_back(a);
  std::stable_sort(addrs.begin(), addrs.end(),
                   [](const json& a, const json& b) { return jstr(a, "kind") == "lan" && jstr(b, "kind") != "lan"; });
  for (auto& a : addrs) {
    std::string ip = jstr(a, "ip");
    auto v = ip_value(ip);
    if (starts_with(ip, "169.254.") || (v && (*v & mask) == n.base)) continue;   // la del tunel no sirve para llegar a el
    out.push_back(ip + ":" + std::to_string(p));
  }
  json res = json::array();
  std::set<std::string> seen;
  for (auto& e : out)
    if (seen.insert(e).second) res.push_back(e);
  return res;
}

json VPN::pub(const json& p) {
  json o = json::object();
  for (auto* k : {"id", "name", "ip", "public", "created"}) o[k] = p.value(k, json());
  return o;
}

json VPN::add_peer(const std::string& name_in, const std::string& lan_key, const std::string& public_ip) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_server_keys();
  std::string name = uleft(trim(name_in.empty() ? "Android" : name_in), 40);
  if (name.empty()) name = "Android";
  auto [priv_raw, pub_raw] = x25519_keypair();
  std::string priv = b64encode(priv_raw);
  json peer = {{"id", token_hex(4)}, {"name", name}, {"ip", next_ip()}, {"public", b64encode(pub_raw)},
               {"psk", b64encode(random_bytes(32))}, {"created", now()}};
  state["peers"].push_back(peer);
  save();
  write_conf();
  json eps = endpoints(public_ip);
  if (eps.empty()) throw std::invalid_argument("no se ha encontrado ninguna direccion de este PC; anade una en vpn.endpoints");
  std::string code = encode_vpn_code({{"v", 1},
                                      {"n", name},
                                      {"a", jstr(peer, "ip") + "/32"},
                                      {"k", priv},
                                      {"s", state["server_public"]},
                                      {"p", peer["psk"]},
                                      {"e", eps},
                                      {"r", server_ip() + "/32"},
                                      {"u", "http://" + server_ip() + ":" + std::to_string(http_port())},
                                      {"key", lan_key}});
  L.info("VPN: dispositivo '", name, "' anadido (", jstr(peer, "ip"), ")");
  return {{"peer", pub(peer)},
          {"code", code},
          {"endpoints", eps},
          {"client_conf", client_conf(peer, priv, eps[0].get<std::string>())},
          {"restart_tunnel", true}};
}

bool VPN::remove_peer(const std::string& peer_id) {
  std::lock_guard<std::mutex> lk(mu_);
  json keep = json::array();
  for (auto& p : state["peers"])
    if (jstr(p, "id") != peer_id) keep.push_back(p);
  if (keep.size() == state["peers"].size()) return false;
  state["peers"] = keep;
  save();
  write_conf();
  return true;
}

// ------------------------------------------------------------------ estado del tunel en el PC
bool VPN::tunnel_up() {
  try {
    std::string ip = server_ip();
    for (auto& a : net::interfaces())
      if (a.ip == ip) return true;
  } catch (...) {
  }
  return false;
}

std::string VPN::wireguard_tool() {
  if (platform_name() == "windows") {
    const char* pf = std::getenv("ProgramFiles");
    fs::path exe = P(pf ? pf : "C:\\Program Files") / "WireGuard" / "wireguard.exe";
    if (is_file(exe)) return S(exe);
    return proc::which("wireguard");
  }
  return proc::which("wg-quick");
}

json VPN::instructions() {
  std::string conf = S(conf_file), port_s = std::to_string(port()), http = std::to_string(http_port()), net = net_text();
  std::string ifc = IFACE;
  std::string win_exe = "& 'C:\\Program Files\\WireGuard\\wireguard.exe'";
  return {
      {"windows",
       {{"title", "Windows 10/11 (PowerShell como administrador)"},
        {"install", {"winget install WireGuard.WireGuard"}},
        {"up",
         {win_exe + " /installtunnelservice '" + conf + "'",
          "New-NetFirewallRule -DisplayName 'Nexus-Office VPN (WireGuard)' -Direction Inbound -Protocol UDP -LocalPort " + port_s +
              " -Action Allow",
          "New-NetFirewallRule -DisplayName 'Nexus-Office por la VPN' -Direction Inbound -Protocol TCP -LocalPort " + http +
              " -RemoteAddress " + net + " -Action Allow"}},
        {"reload", {win_exe + " /uninstalltunnelservice " + ifc, "Start-Sleep 2", win_exe + " /installtunnelservice '" + conf + "'"}},
        {"down", {win_exe + " /uninstalltunnelservice " + ifc}}}},
      {"linux",
       {{"title", "Linux (Ubuntu, Debian, Fedora, Arch, Raspberry Pi OS...)"},
        {"install",
         {"sudo apt install wireguard-tools    # Fedora: sudo dnf install wireguard-tools · Arch: sudo pacman -S wireguard-tools"}},
        {"up",
         {"sudo install -m 600 '" + conf + "' /etc/wireguard/" + ifc + ".conf", "sudo systemctl enable --now wg-quick@" + ifc,
          "sudo ufw allow " + port_s + "/udp    # solo si usas ufw"}},
        {"reload",
         {"sudo install -m 600 '" + conf + "' /etc/wireguard/" + ifc + ".conf",
          "sudo bash -c 'wg syncconf " + ifc + " <(wg-quick strip " + ifc + ")'"}},
        {"down", {"sudo systemctl disable --now wg-quick@" + ifc}}}},
      {"macos",
       {{"title", "macOS (Terminal) — o la app WireGuard de la App Store: «Importar túnel desde archivo»"},
        {"install", {"brew install wireguard-tools"}},
        {"up", {"sudo wg-quick up '" + conf + "'"}},
        {"reload", {"sudo wg-quick down '" + conf + "'", "sudo wg-quick up '" + conf + "'"}},
        {"down", {"sudo wg-quick down '" + conf + "'"}}}},
      {"wsl",
       {{"title", "WSL (Nexus-Office dentro de Ubuntu en Windows)"},
        {"install",
         {"En Windows, %UserProfile%\\.wslconfig →  [wsl2]  networkingMode=mirrored   (y luego: wsl --shutdown)",
          "sudo apt install wireguard-tools"}},
        {"up",
         {"sudo install -m 600 '" + conf + "' /etc/wireguard/" + ifc + ".conf", "sudo wg-quick up " + ifc,
          "(PowerShell admin) New-NetFirewallHyperVRule -Name NexusVPN -DisplayName 'Nexus-Office VPN' -Direction Inbound "
          "-VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts " + port_s}},
        {"reload",
         {"sudo install -m 600 '" + conf + "' /etc/wireguard/" + ifc + ".conf",
          "sudo bash -c 'wg syncconf " + ifc + " <(wg-quick strip " + ifc + ")'"}},
        {"down", {"sudo wg-quick down " + ifc}}}},
  };
}

json VPN::apply() {
  write_conf();
  std::string plat = platform_name();
  json steps = instructions()[plat];
  bool up = tunnel_up();
  json cmds = up ? steps["reload"] : steps["up"];
  std::string tool = wireguard_tool();
  if (tool.empty()) {
    json all = steps["install"];
    for (auto& c : steps["up"]) all.push_back(c);
    return {{"applied", false}, {"platform", plat}, {"reason", "WireGuard no está instalado en este PC"}, {"commands", all}};
  }
  proc::Options cap;
  cap.capture = true;
  proc::Result out;
  try {
    if ((plat == "linux" || plat == "wsl" || plat == "macos") && is_admin()) {
      if (plat == "macos") {
        if (up) proc::run({tool, "down", S(conf_file)}, 30, cap);
        out = proc::run({tool, "up", S(conf_file)}, 60, cap);
      } else {
        fs::path target = P("/etc/wireguard") / (std::string(IFACE) + ".conf");
        mkdirs(target.parent_path());
        fs::copy_file(conf_file, target, fs::copy_options::overwrite_existing);
        make_private(target);
        if (up) {
          std::string stripped = proc::run({tool, "strip", IFACE}, 30, cap).out;
          proc::Options in = cap;
          in.input = stripped;
          out = proc::run({"wg", "syncconf", IFACE, "/dev/stdin"}, 30, in);
        } else {
          out = proc::run({tool, "up", IFACE}, 60, cap);
        }
      }
    } else if (plat == "windows" && is_admin()) {
      if (up) {
        proc::run({tool, "/uninstalltunnelservice", IFACE}, 60, cap);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      out = proc::run({tool, "/installtunnelservice", S(conf_file)}, 60, cap);
    } else {
      return {{"applied", false}, {"platform", plat}, {"reason", "Nexus-Office no se ejecuta con permisos de administrador"},
              {"commands", cmds}};
    }
  } catch (const std::exception& e) {
    return {{"applied", false}, {"platform", plat}, {"reason", e.what()}, {"commands", cmds}};
  }
  bool ok = out.ok();
  std::string reason = trim(!out.err.empty() ? out.err : out.out);
  if (reason.size() > 300) reason = reason.substr(reason.size() - 300);
  if (!out.started) reason = "no se pudo ejecutar " + tool;
  return {{"applied", ok}, {"platform", plat}, {"reason", ok ? "" : reason}, {"commands", ok ? json::array() : cmds}};
}

json VPN::status() {
  std::string plat = platform_name();
  json peers = json::array();
  for (auto& p : state["peers"]) peers.push_back(pub(p));
  std::string host = cfg_.str("server.host", "127.0.0.1");
  return {{"enabled", cfg_.flag("vpn.enabled", false)},
          {"port", port()},
          {"subnet", net_text()},
          {"server_ip", server_ip()},
          {"public_key", jstr(state, "server_public")},
          {"peers", peers},
          {"conf_path", S(conf_file)},
          {"platform", plat},
          {"wireguard", !wireguard_tool().empty()},
          {"tunnel_up", tunnel_up()},
          {"listening_lan", host != "127.0.0.1" && host != "localhost"},
          {"instructions", instructions()}};
}

}  // namespace nx::remote
