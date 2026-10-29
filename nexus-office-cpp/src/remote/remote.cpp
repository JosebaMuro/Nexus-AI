// Acceso remoto: dispositivos emparejados, certificado propio, UPnP-IGD, asistente del router y direcciones.
#include "remote/remote.h"

#include <httplib.h>

#include <algorithm>
#include <cstring>

#include "coop/coop.h"
#include "core/config.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"
#include "util/xml.h"

namespace nx::rag {
std::string urljoin(const std::string& base, const std::string& href);
}

namespace nx::remote {

namespace {
auto L = log::get("remote");
const std::string ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";   // sin 0/O/1/I/L para dictarlo sin errores
constexpr double PAIR_TTL = 600, SEEN_WRITE_EVERY = 60;

std::string b64url_nopad(const std::string& raw) {
  std::string s = b64url_encode(raw);
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}
std::string b64url_decode(std::string body) {
  for (auto& c : body) c = c == '-' ? '+' : c == '_' ? '/' : c;
  return b64decode(body);
}
}  // namespace

const char* COOKIE = "nexus_dev";
const char* CRYPTO_TEXT = "TLS 1.3 · X25519 · AES-GCM / ChaCha20-Poly1305 · ECDSA P-256 anclado";
const std::vector<std::pair<std::string, std::string>> SECURITY_HEADERS = {
    {"x-content-type-options", "nosniff"}, {"referrer-policy", "no-referrer"}, {"x-frame-options", "DENY"},
    {"cache-control", "no-store"}};

bool open_path(const std::string& path) {
  return path == "/api/ping" || path == "/api/remote/pair" || path == "/sw.js" || path == "/favicon.ico" ||
         starts_with(path, "/static/");   // la interfaz es codigo publico; datos, salidas y API van protegidos
}

// ======================================================================== devices.py
std::string normalize_code(const std::string& code) {
  std::string out;
  for (char c : upper(code))
    if (std::isalnum((unsigned char)c)) out += c;
  return out;
}

std::vector<double>& RateLimiter::recent(const std::string& key) {
  double t = now();
  auto& lst = fails_[key];
  lst.erase(std::remove_if(lst.begin(), lst.end(), [&](double x) { return t - x >= window_; }), lst.end());
  return lst;
}
bool RateLimiter::blocked(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  return (int)recent(key).size() >= max_;
}
void RateLimiter::fail(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  recent(key).push_back(now());
}
void RateLimiter::reset(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  fails_.erase(key);
}

DeviceStore::DeviceStore(const fs::path& folder) : path_(folder / "devices.json"), data_({{"devices", json::object()}}) {
  if (is_file(path_)) {
    json d = jparse(read_text(path_));
    if (d.is_object() && d.value("devices", json()).is_object()) data_ = d;
  }
  for (auto& [id, d] : data_["devices"].items()) by_hash_[jstr(d, "token_hash")] = id;
}

void DeviceStore::save() {
  write_file(path_, data_.dump(1));
  last_write_ = now();
}

json DeviceStore::new_pairing(const std::string& name) {
  std::string raw, rnd = random_bytes(12);
  for (unsigned char c : rnd) raw += ALPHABET[c % ALPHABET.size()];
  std::string code = raw.substr(0, 4) + "-" + raw.substr(4, 4) + "-" + raw.substr(8);
  double t = now();
  std::lock_guard<std::mutex> lk(mu_);
  for (auto it = pending_.begin(); it != pending_.end();)
    it = it->second.value("expires", 0.0) > t ? std::next(it) : pending_.erase(it);
  pending_[normalize_code(code)] = {{"name", uleft(trim(name), 40)}, {"expires", t + PAIR_TTL}};
  return {{"code", code}, {"expires", (long long)(t + PAIR_TTL)}, {"ttl", (int)PAIR_TTL}};
}

std::optional<std::pair<std::string, json>> DeviceStore::pair(const std::string& code, const std::string& device_name,
                                                              const std::string& ip) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = pending_.find(normalize_code(code));
  if (it == pending_.end()) return std::nullopt;
  json p = it->second;
  pending_.erase(it);
  if (p.value("expires", 0.0) < now()) return std::nullopt;
  std::string token = token_urlsafe(32), dev_id = token_hex(6);
  std::string nm = !device_name.empty() ? device_name : !jstr(p, "name").empty() ? jstr(p, "name") : "Dispositivo";
  long long t = (long long)now();
  json dev = {{"name", uleft(trim(nm), 40)}, {"token_hash", sha256_hex(token)}, {"created", t}, {"last_seen", t},
              {"last_ip", ip}};
  data_["devices"][dev_id] = dev;
  by_hash_[jstr(dev, "token_hash")] = dev_id;
  save();
  json pub = {{"id", dev_id}};
  for (auto& [k, v] : dev.items())
    if (k != "token_hash") pub[k] = v;
  return std::make_pair(token, pub);
}

std::string DeviceStore::check(const std::string& token, const std::string& ip) {
  if (token.empty()) return "";
  std::lock_guard<std::mutex> lk(mu_);
  auto it = by_hash_.find(sha256_hex(token));   // busqueda por huella: no hay comparacion de secretos
  if (it == by_hash_.end()) return "";
  auto& devs = data_["devices"];
  if (!devs.contains(it->second)) return "";
  auto& dev = devs[it->second];
  double t = now();
  dev["last_seen"] = (long long)t;
  if (!ip.empty()) dev["last_ip"] = ip;
  if (t - last_write_ > SEEN_WRITE_EVERY) save();
  return it->second;
}

json DeviceStore::list() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::pair<std::string, json>> all;
  for (auto& [id, d] : data_["devices"].items()) all.emplace_back(id, d);
  std::stable_sort(all.begin(), all.end(), [](auto& a, auto& b) {
    return a.second.value("last_seen", 0.0) > b.second.value("last_seen", 0.0);
  });
  json out = json::array();
  for (auto& [id, d] : all) {
    json e = {{"id", id}};
    for (auto& [k, v] : d.items())
      if (k != "token_hash") e[k] = v;
    out.push_back(e);
  }
  return out;
}

bool DeviceStore::revoke(const std::string& dev_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto& devs = data_["devices"];
  if (!devs.contains(dev_id)) return false;
  by_hash_.erase(jstr(devs[dev_id], "token_hash"));
  devs.erase(dev_id);
  save();
  return true;
}

// ======================================================================== tlscert.py
std::string pretty(const std::string& fp) {
  std::string out;
  for (size_t i = 0; i < fp.size(); i += 2) out += (i ? ":" : "") + fp.substr(i, 2);
  return out;
}

std::tuple<fs::path, fs::path, std::string> ensure_cert(const fs::path& folder, const std::vector<std::string>& extra,
                                                        bool regenerate) {
  mkdirs(folder);
  fs::path cert_p = folder / "cert.pem", key_p = folder / "key.pem";
  if (regenerate || !(is_file(cert_p) && is_file(key_p))) {
    std::string host = sys::hostname();
    std::vector<std::string> names = {"localhost", host.empty() ? "nexus" : host};
    for (auto& n : std::vector<std::string>{"127.0.0.1"}) names.push_back(n);
    for (auto& n : extra)
      if (!n.empty() && std::find(names.begin() + 2, names.end(), n) == names.end()) names.push_back(n);
    auto [cert, key] = selfsigned_cert("Nexus-Office", "Nexus-Office (certificado propio)", names, 3650);
    write_file(key_p, key);
    try {
      fs::permissions(key_p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    } catch (...) {
    }
    write_file(cert_p, cert);
  }
  return {cert_p, key_p, cert_fingerprint(read_text(cert_p))};
}

// ======================================================================== upnp.py
namespace {
const std::pair<const char*, int> SSDP = {"239.255.255.250", 1900};
const std::vector<std::string> TARGETS = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                          "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
const std::vector<std::string> SERVICES = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                           "urn:schemas-upnp-org:service:WANIPConnection:1",
                                           "urn:schemas-upnp-org:service:WANPPPConnection:1"};
const char* DESC = "Nexus-Office acceso remoto";

std::string msearch(const std::string& st) {
  return "M-SEARCH * HTTP/1.1\r\nHOST: " + std::string(SSDP.first) + ":" + std::to_string(SSDP.second) +
         "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + st + "\r\n\r\n";
}

std::string findtext(const xml::Element& el, const std::string& tag) {
  for (auto& c : el.children)
    if (c->tag == tag) return c->text;
  return "";
}
}  // namespace

bool is_cgnat_or_private(const std::string& ip) { return net::is_private_ip(trim(ip)); }

std::string parse_ssdp(const std::string& reply) {
  for (auto& line : split(replace_all(reply, "\r", ""), "\n")) {
    auto kv = split(line, ":", 1);
    if (kv.size() == 2 && lower(trim(kv[0])) == "location") return trim(kv[1]);
  }
  return "";
}

std::optional<std::pair<std::string, std::string>> parse_description(const std::string& xml_text, const std::string& location) {
  auto root = xml::parse(xml_text);
  std::string ns;
  if (starts_with(root->tag, "{")) ns = root->tag.substr(0, root->tag.find('}') + 1);
  std::string base = trim(findtext(*root, ns + "URLBase"));
  if (base.empty()) base = location;
  std::map<std::string, std::string> found;
  for (auto* svc : root->iter()) {
    if (svc->tag != ns + "service") continue;
    std::string st = trim(findtext(*svc, ns + "serviceType")), ctl = trim(findtext(*svc, ns + "controlURL"));
    if (std::find(SERVICES.begin(), SERVICES.end(), st) != SERVICES.end() && !ctl.empty()) found[st] = rag::urljoin(base, ctl);
  }
  for (auto& st : SERVICES)
    if (found.count(st)) return std::make_pair(found[st], st);
  return std::nullopt;
}

std::string soap_body(const std::string& action, const std::string& service,
                      const std::vector<std::pair<std::string, std::string>>& args) {
  std::string inner;
  for (auto& [k, v] : args) inner += "<" + k + ">" + v + "</" + k + ">";
  return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
         "<u:" + action + " xmlns:u=\"" + service + "\">" + inner + "</u:" + action + "></s:Body></s:Envelope>";
}

std::string parse_soap_value(const std::string& xml_text, const std::string& field) {
  auto root = xml::parse(xml_text);
  for (auto* el : root->iter())
    if (xml::strip_ns(el->tag) == field) return trim(el->text);
  return "";
}

Gateway Gateway::discover(double timeout) {
  std::vector<std::string> msgs, locations;
  for (auto& st : TARGETS) msgs.push_back(msearch(st));
  for (auto& [data, ip] : net::udp_query(SSDP.first, SSDP.second, msgs, timeout)) {
    std::string loc = parse_ssdp(data);
    if (!loc.empty() && std::find(locations.begin(), locations.end(), loc) == locations.end()) locations.push_back(loc);
  }
  for (auto& loc : locations) {
    try {
      auto r = http::get(loc, {}, 4);
      if (!r.status) continue;
      auto svc = parse_description(r.body, loc);
      if (svc) return Gateway(svc->first, svc->second);
    } catch (...) {
    }
  }
  throw std::runtime_error("No se encontro un router con UPnP activo (activalo en el router o abre el puerto a mano)");
}

std::string Gateway::call(const std::string& action, const std::vector<std::pair<std::string, std::string>>& args) {
  http::Request req;
  req.method = "POST";
  req.url = control_url;
  req.body = soap_body(action, service, args);
  req.content_type = "text/xml; charset=\"utf-8\"";
  req.headers = {{"SOAPAction", "\"" + service + "#" + action + "\""}};
  req.timeout = 6;
  auto r = http::send(req);
  if (r.status == 0) throw std::runtime_error(r.error.empty() ? "el router no responde" : r.error);
  if (r.status != 200) {
    std::string err;
    try {
      if (!r.body.empty()) err = parse_soap_value(r.body, "errorDescription");
    } catch (...) {
    }
    throw std::runtime_error("El router rechazo " + action + ": " + (err.empty() ? std::to_string(r.status) : err));
  }
  return r.body;
}

std::string Gateway::external_ip() { return parse_soap_value(call("GetExternalIPAddress", {}), "NewExternalIPAddress"); }

std::string Gateway::add_mapping(int port, int lease_s, const std::string& protocol) {
  std::string host = http::parse_url(control_url).host;
  std::string internal = net::local_ip_towards(host.empty() ? "8.8.8.8" : host);
  std::string p = std::to_string(port);
  call("AddPortMapping", {{"NewRemoteHost", ""}, {"NewExternalPort", p}, {"NewProtocol", protocol}, {"NewInternalPort", p},
                          {"NewInternalClient", internal}, {"NewEnabled", "1"}, {"NewPortMappingDescription", DESC},
                          {"NewLeaseDuration", std::to_string(lease_s)}});
  return internal;
}

void Gateway::delete_mapping(int port, const std::string& protocol) {
  call("DeletePortMapping", {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(port)}, {"NewProtocol", protocol}});
}

// ======================================================================== router.py
namespace {
struct Guide {
  const char *name, *path, *tip;
};
// marca -> (nombre, ruta orientativa del menu, consejo). El nombre exacto varia segun modelo y firmware.
const std::map<std::string, Guide> GUIDES = {
    {"movistar", {"Movistar (HGU / Askey / Mitrastar)", "Configuración avanzada → NAT / Puertos (Port Mapping)",
                  "También desde la app Smart WiFi de Movistar → Puertos."}},
    {"vodafone", {"Vodafone (Station / Sercomm / Hitron)", "Ajustes → Internet / Firewall → Mapeo de puertos (Port Forwarding)",
                  "En routers de cable antiguos (ONO) la opción está en Básico/Avanzado → Reenvío de puertos."}},
    {"orange", {"Orange / Jazztel (Livebox)", "Configuración avanzada → NAT/PAT", "Elige este equipo en la lista de dispositivos."}},
    {"digi", {"Digi", "Avanzado / Red → NAT → Reenvío de puertos", "Los routers de Digi suelen ser ZTE: mira también la guía ZTE."}},
    {"fritz", {"FRITZ!Box (AVM)", "Internet → Permitir acceso → Uso compartido de puertos → Añadir dispositivo",
               "Puedes elegir este equipo por su nombre y añadir el puerto."}},
    {"tplink", {"TP-Link", "Avanzado → NAT Forwarding → Servidores virtuales (Virtual Servers)", ""}},
    {"asus", {"ASUS", "WAN → Servidor virtual / Reenvío de puertos", ""}},
    {"netgear", {"Netgear", "Avanzado → Configuración avanzada → Reenvío de puertos / activación de puertos", ""}},
    {"huawei", {"Huawei", "Avanzado → NAT → Mapeo de puertos (Port Mapping)", ""}},
    {"zte", {"ZTE", "Internet / Red → Seguridad → Reenvío de puertos (Port Forwarding)", ""}},
    {"xiaomi", {"Xiaomi (MiWiFi)", "Ajustes avanzados → Reenvío de puertos", ""}},
    {"mikrotik", {"MikroTik (RouterOS)", "IP → Firewall → NAT → regla dst-nat (TCP, puerto, to-addresses)", ""}},
    {"dlink", {"D-Link", "Avanzado → Reenvío de puertos / Servidor virtual", ""}},
    {"linksys", {"Linksys", "Seguridad → Aplicaciones y juegos → Reenvío de un solo puerto", ""}},
    {"sagemcom", {"Sagemcom", "Acceso avanzado → Red → NAT / Reenvío de puertos", ""}},
    {"generic", {"Router", "Busca un menú llamado «NAT», «Reenvío de puertos», «Port forwarding», «Port mapping» o «Servidores virtuales»",
                 "Suele estar en la configuración avanzada o en Seguridad / Firewall."}},
};
const std::vector<std::pair<std::string, std::string>> BRAND_RE = {
    {"movistar", R"(movistar|askey|mitrastar|\bhgu\b|telef[oó]nica)"}, {"vodafone", R"(vodafone|sercomm|hitron|compal|\bono\b)"},
    {"orange", R"(livebox|orange|jazztel|arcadyan)"}, {"digi", R"(\bdigi\b)"}, {"fritz", R"(fritz|\bavm\b)"},
    {"tplink", R"(tp-?link|archer|\bdeco\b)"}, {"asus", R"(\basus\b|\brt-[a-z]{1,2}\d)"}, {"netgear", R"(netgear|nighthawk|orbi)"},
    {"huawei", R"(huawei|\bhg8\d)"}, {"zte", R"(\bzte\b|zxhn|\bf6\d\d)"}, {"xiaomi", R"(xiaomi|miwifi|redmi)"},
    {"mikrotik", R"(mikrotik|routeros)"}, {"dlink", R"(d-link|dlink)"}, {"linksys", R"(linksys)"}, {"sagemcom", R"(sagemcom)"},
};

std::string page_url(const std::string& host, int port, bool tls) {
  return std::string(tls ? "https" : "http") + "://" + host + (port == 80 || port == 443 ? "" : ":" + std::to_string(port)) + "/";
}

// GET tolerante a la pagina del router (solo lee). nullopt si no acepta conexiones
std::optional<json> probe_page(const std::string& host, int port, bool tls, double timeout = 3) {
  std::string url = page_url(host, port, tls), raw;
  if (!net::tcp_exchange(host, port, "", timeout, 0)) return std::nullopt;
  json blank = {{"url", url}, {"responds", true}, {"title", ""}, {"text", ""}};
  if (!tls) {
    auto got = net::tcp_exchange(host, port,
                                 "GET / HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: Mozilla/5.0 Nexus-Office-router-assistant\r\n"
                                 "Accept: text/html\r\nConnection: close\r\n\r\n", timeout, 65536);
    if (!got) return blank;
    raw = *got;
  } else {
#ifdef CPPHTTPLIB_SSL_ENABLED
    httplib::SSLClient cli(host, port);
    cli.enable_server_certificate_verification(false);
    cli.set_connection_timeout((time_t)timeout);
    cli.set_read_timeout((time_t)timeout);
    httplib::Headers hdr = {{"User-Agent", "Mozilla/5.0 Nexus-Office-router-assistant"}, {"Accept", "text/html"}};
    auto r = cli.Get("/", hdr);
    if (!r) return blank;
    raw = "HTTP/1.1 " + std::to_string(r->status) + "\r\n";
    for (auto& [k, v] : r->headers) raw += k + ": " + v + "\r\n";
    raw += "\r\n" + r->body.substr(0, 65536);
#else
    return blank;
#endif
  }
  std::string text = latin1_to_utf8(raw);
  size_t cut = text.find("\r\n\r\n");
  std::string head = cut == std::string::npos ? text : text.substr(0, cut);
  std::string body = cut == std::string::npos ? "" : text.substr(cut + 4);
  static const Rx TITLE(R"(<title[^>]*>(.*?)</title>)", Rx::I | Rx::S), CODE(R"(\d{3}\b.*)", Rx::S),
      REALM(R"x(realm="([^"]+)")x", Rx::I), SERVER(R"(^server:\s*(.+)$)", Rx::I | Rx::M), WS(R"(\s+)");
  std::string t;
  if (auto m = TITLE.search(body)) t = uleft(trim(WS.sub(m->str(1), " ")), 80);
  if (CODE.fullmatch(t)) t.clear();   // "400 Bad Request", "401 Unauthorized"... no identifica nada
  std::string realm, server;
  if (auto m = REALM.search(head)) realm = m->str(1);
  if (auto m = SERVER.search(head)) server = m->str(1);
  std::vector<std::string> parts;
  for (auto& p : {t, realm, server, uleft(body, 4000)})
    if (!p.empty()) parts.push_back(p);
  return json{{"url", url}, {"responds", true}, {"title", t.empty() ? realm : t}, {"text", join(parts, " ")}};
}

json upnp_devices(double timeout = 2.5) {
  std::vector<std::string> msgs;
  for (auto& st : TARGETS) msgs.push_back(msearch(st));
  msgs.push_back(msearch("upnp:rootdevice"));
  std::vector<std::pair<std::string, std::string>> found;   // (location, ip) por orden de llegada
  for (auto& [data, ip] : net::udp_query(SSDP.first, SSDP.second, msgs, timeout)) {
    std::string loc = parse_ssdp(data);
    if (!loc.empty() && std::none_of(found.begin(), found.end(), [&](auto& f) { return f.first == loc; }))
      found.emplace_back(loc, ip);
  }
  json out = json::array();
  for (auto& [loc, ip] : found) {
    auto r = http::get(loc, {}, 3);
    if (!r.status) continue;
    const std::string& x = r.body;
    auto tag = [&](const std::string& n) {
      Rx rx("<" + n + ">(.*?)</" + n + ">", Rx::S);
      auto m = rx.search(x);
      return m ? trim(m->str(1)) : std::string();
    };
    bool igd = false;
    try {
      igd = parse_description(x, loc).has_value();
    } catch (...) {
    }
    std::vector<std::string> model;
    for (auto& p : {tag("modelName"), tag("modelNumber")})
      if (!p.empty()) model.push_back(p);
    out.push_back({{"ip", ip}, {"location", loc}, {"friendly", tag("friendlyName")}, {"manufacturer", tag("manufacturer")},
                   {"model", join(model, " ")}, {"presentation", tag("presentationURL")}, {"igd", igd}});
  }
  return out;
}
}  // namespace

std::string identify(const std::vector<std::string>& texts) {
  std::vector<std::string> nonempty;
  for (auto& t : texts)
    if (!t.empty()) nonempty.push_back(t);
  std::string blob = lower(join(nonempty, " "));
  for (auto& [key, rx] : BRAND_RE)
    if (re_search(rx, blob)) return key;
  return "generic";
}

std::string parse_route_print(const std::string& text) {
  static const Rx R(R"(^\s*0\.0\.0\.0\s+0\.0\.0\.0\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+))", Rx::M);
  std::string best;
  long best_metric = 0;
  for (auto& m : R.finditer(text)) {
    long metric = std::atol(m.str(3).c_str());
    if (best.empty() || metric < best_metric) {
      best = m.str(1);
      best_metric = metric;
    }
  }
  return best;
}

std::string parse_proc_route(const std::string& text) {
  auto lines = split(text, "\n");
  for (size_t i = 1; i < lines.size(); ++i) {
    auto f = split_ws(lines[i]);
    if (f.size() > 2 && f[1] == "00000000" && f[2] != "00000000" && f[2].size() == 8) {
      std::string b = from_hex(f[2]);
      return std::to_string((unsigned char)b[3]) + "." + std::to_string((unsigned char)b[2]) + "." +
             std::to_string((unsigned char)b[1]) + "." + std::to_string((unsigned char)b[0]);
    }
  }
  return "";
}

std::string default_gateway() {
#ifdef _WIN32
  auto r = proc::run({"route", "print", "-4", "0.0.0.0"}, 8, {.capture = true});
  return parse_route_print(r.out);
#else
  if (is_file("/proc/net/route")) return parse_proc_route(read_text("/proc/net/route"));
  proc::Options o;
  o.capture = true;
  auto r = proc::run({"route", "-n", "get", "default"}, 8, o);
  static const Rx G(R"(gateway:\s*(\S+))");
  auto m = G.search(r.out);
  return m ? m->str(1) : "";
#endif
}

json interface_for(const std::string& ip) {
  for (auto& a : net::interfaces())
    if (a.ip == ip) return {{"name", a.name}, {"mac", a.mac}};
  return {{"name", ""}, {"mac", ""}};
}

json detect(int port) {
  std::string gw = default_gateway();
  std::string local_ip = net::local_ip_towards(gw.empty() ? "8.8.8.8" : gw);
  if (gw.empty() && !local_ip.empty()) {   // ultimo recurso: la .1 de la subred
    auto p = split(local_ip, ".");
    if (p.size() == 4) gw = p[0] + "." + p[1] + "." + p[2] + ".1";
  }
  json devs = json::array();
  for (auto& d : upnp_devices())
    if (jstr(d, "ip") == gw) devs.push_back(d);
  json pages = json::array();
  if (!gw.empty())
    for (auto& p : {probe_page(gw, 80, false), probe_page(gw, 443, true)})
      if (p) pages.push_back(*p);
  for (auto& d : devs)
    if (starts_with(jstr(d, "presentation"), "http"))
      pages.insert(pages.begin(), json{{"url", d["presentation"]}, {"responds", true}, {"title", d["friendly"]}, {"text", ""}});
  std::vector<std::string> texts;
  for (auto& p : pages) texts.push_back(jstr(p, "text"));
  for (auto& d : devs) texts.push_back(jstr(d, "manufacturer") + " " + jstr(d, "model") + " " + jstr(d, "friendly"));
  std::string brand = identify(texts);
  const Guide& g = GUIDES.at(brand);
  std::string model, title;
  for (auto& d : devs)
    if (!jstr(d, "manufacturer").empty() && !contains(lower(jstr(d, "manufacturer")), "broadcom")) {
      model = trim(jstr(d, "manufacturer") + " " + jstr(d, "model"));
      break;
    }
  for (auto& p : pages)
    if (!jstr(p, "title").empty()) {
      title = jstr(p, "title");
      break;
    }
  json admin = json::array();
  for (auto& p : pages) admin.push_back(p["url"]);
  if (admin.empty() && !gw.empty()) admin.push_back("http://" + gw + "/");
  bool igd = std::any_of(devs.begin(), devs.end(), [](const json& d) { return d.value("igd", false); });
  return {{"gateway", gw},
          {"gateway_private", !gw.empty() && net::is_private_ip(gw)},
          {"local_ip", local_ip},
          {"interface", interface_for(local_ip)},
          {"admin_urls", admin},
          {"page_title", title},
          {"brand", brand},
          {"brand_name", brand != "generic" ? std::string(g.name)
                         : !model.empty() ? model : !title.empty() ? title : std::string("Router no identificado")},
          {"menu_path", g.path},
          {"tip", g.tip},
          {"upnp_igd", igd},
          {"upnp_devices", devs},
          {"rule",
           {{"name", "Nexus-Office"}, {"protocol", "TCP"}, {"external_port", port}, {"internal_ip", local_ip},
            {"internal_port", port}}}};
}

bool reachable_from_outside(const std::string& public_ip, int port, double timeout) {
#ifdef CPPHTTPLIB_SSL_ENABLED
  // si falla puede ser que el router no admita el «bucle» (hairpin NAT): la prueba definitiva es el movil con datos
  httplib::SSLClient cli(public_ip, port);
  cli.enable_server_certificate_verification(false);
  cli.set_connection_timeout((time_t)timeout);
  cli.set_read_timeout((time_t)timeout);
  return (bool)cli.Get("/api/ping");
#else
  (void)public_ip, (void)port, (void)timeout;
  return false;
#endif
}

std::string public_ip_lookup() {
  auto r = http::get("https://api.ipify.org", {}, 6);
  if (!r.ok()) throw std::runtime_error(r.status ? "HTTP " + std::to_string(r.status) : r.error);
  return trim(r.body);
}

// ======================================================================== __init__.py
RemoteAccess::RemoteAccess(Config& cfg) : folder(paths::sub({"remote"})), devices(folder), cfg_(cfg) {}

std::tuple<fs::path, fs::path, std::string> RemoteAccess::cert(bool regenerate) {
  std::vector<std::string> names;
  for (auto& a : coop::overlay_addresses()) names.push_back(jstr(a, "ip"));
  if (!public_ip.empty()) names.push_back(public_ip);
  auto res = ensure_cert(folder, names, regenerate);
  std::lock_guard<std::mutex> lk(mu_);
  fp_ = std::get<2>(res);
  return res;
}

std::string RemoteAccess::fingerprint() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!fp_.empty()) return fp_;
  }
  return std::get<2>(cert());
}

int RemoteAccess::port() { return (int)cfg_.num("remote.port", 8443); }

json RemoteAccess::urls() {
  int p = port();
  json out = json::array();
  std::string ext = jstr(upnp, "external_ip");
  if (ext.empty()) ext = public_ip;
  if (!ext.empty()) out.push_back({{"kind", "internet"}, {"url", "https://" + ext + ":" + std::to_string(p)}});
  std::vector<json> addrs;
  for (auto& a : coop::overlay_addresses()) addrs.push_back(a);
  std::stable_sort(addrs.begin(), addrs.end(),
                   [](const json& a, const json& b) { return jstr(a, "kind") == "tailscale" && jstr(b, "kind") != "tailscale"; });
  for (auto& a : addrs)
    if (!starts_with(jstr(a, "ip"), "169.254."))
      out.push_back({{"kind", a["kind"]}, {"url", "https://" + jstr(a, "ip") + ":" + std::to_string(p)}});
  return out;
}

std::string RemoteAccess::invite(const json& pairing, const std::string& name) {
  json u = json::array();
  for (auto& x : urls()) u.push_back(x["url"]);
  json body = {{"u", u}, {"f", fingerprint()}, {"c", pairing["code"]}, {"n", name}};
  return "NEXUSR1:" + b64url_nopad(body.dump());
}

json RemoteAccess::status() {
  return {{"enabled", cfg_.flag("remote.enabled", false)},
          {"port", port()},
          {"listening", listening_port.load() != 0},
          {"fingerprint", pretty(fingerprint())},
          {"urls", urls()},
          {"upnp", upnp},
          {"public_ip", public_ip},
          {"behind_cgnat", behind_cgnat},
          {"devices", devices.list()},
          {"crypto", CRYPTO_TEXT}};
}

std::string RemoteAccess::pair_page() {
  std::string html = read_text(paths::package() / "remote" / "pair.html");
  if (html.empty())
    html = "<!doctype html><meta charset=utf-8><title>Nexus-Office</title><h2>Nexus-Office</h2><p>Empareja este "
           "dispositivo desde la app. Huella: {{FP}}</p>";
  return replace_all(html, "{{FP}}", pretty(fingerprint()));
}

json read_invite(const std::string& code_in) {
  std::string code = trim(code_in);
  if (!starts_with(code, "NEXUSR1:")) throw std::invalid_argument("codigo de acceso remoto no valido");
  return jparse(b64url_decode(code.substr(8)));
}

}  // namespace nx::remote
