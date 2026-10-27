// Dispositivos (nexus/devices/leds.py y hub.py): mismo protocolo NexusLED, mismos modos y mismos JSON.
#include "devices/devices.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <set>

#include "agents/activity.h"
#include "agents/agents.h"
#include "core/config.h"
#include "core/services.h"
#include "media/raster.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/log.h"
#include "util/net.h"
#include "util/text.h"

#include "stb_image_write.h"

namespace nx::devices {

const int UDP_PORT = 4210;
const std::vector<std::string> ESTADOS = {"none", "idle", "listening", "thinking", "tool",
                                          "speaking", "success", "error", "alert"};

namespace {

auto L = log::get("dispositivos");
auto LL = log::get("leds");

const std::map<std::string, std::string> NOMBRES = {
    {"none", "Sin estado"}, {"idle", "En línea"},         {"listening", "Escuchando"}, {"thinking", "Pensando"},
    {"tool", "Usando herramienta"}, {"speaking", "Hablando"}, {"success", "Hecho"}, {"error", "Error"},
    {"alert", "Aviso"}};
const std::map<std::string, std::string> COLORES = {
    {"none", "#000000"}, {"idle", "#00f0ff"},    {"listening", "#7cff6b"}, {"thinking", "#a66bff"}, {"tool", "#ffb020"},
    {"speaking", "#ff3df2"}, {"success", "#28ff5a"}, {"error", "#ff1414"}, {"alert", "#ffb020"}};
// caras de pwnagotchi para la pantalla de tinta (ademas del avatar)
const std::map<std::string, std::string> CARAS = {
    {"idle", "(◕‿‿◕)"}, {"listening", "(⊙‿‿⊙)"}, {"thinking", "(◔_◔ )"}, {"tool", "(•̀ᴗ•́ )و"}, {"speaking", "(ᵔ◡◡ᵔ)"},
    {"success", "(★‿‿★)"}, {"error", "(╥☁╥ )"}, {"alert", "(⚆_⚆)"}, {"none", "(-__-)"}};

// modos preconfigurados: cada uno cambia unas claves de configuracion (y, si se indica, el perfil de hardware)
const json& modos_defecto() {
  static const json M = json::parse(R"JSON([
    {"id": "normal", "nombre": "Normal", "icono": "◆", "descripcion": "Voz y luces como siempre",
     "config": {"user.voice_enabled": true, "user.auto_speak": false, "lounge.auto_meet": true}, "leds": {"bri": 160}},
    {"id": "voz", "nombre": "Manos libres", "icono": "🎙", "descripcion": "Responde siempre en voz alta",
     "config": {"user.voice_enabled": true, "user.auto_speak": true}, "leds": {"bri": 200}},
    {"id": "silencio", "nombre": "Silencio", "icono": "🔇", "descripcion": "Sin voz y luces tenues",
     "config": {"user.voice_enabled": false, "user.auto_speak": false}, "leds": {"bri": 25}},
    {"id": "concentracion", "nombre": "Concentración", "icono": "🎯",
     "descripcion": "Sin reuniones automáticas ni voz; respuestas breves",
     "config": {"user.auto_speak": false, "lounge.auto_meet": false, "user.response_style": "breve"}, "leds": {"bri": 60}},
    {"id": "rendimiento", "nombre": "Rendimiento", "icono": "⚡", "descripcion": "Perfil de hardware más potente",
     "config": {}, "perfil_hw": "rendimiento", "leds": {"bri": 200}},
    {"id": "ahorro", "nombre": "Ahorro", "icono": "🔋", "descripcion": "Perfil ligero: menos RAM y batería",
     "config": {}, "perfil_hw": "ahorro", "leds": {"bri": 40}},
    {"id": "noche", "nombre": "Noche", "icono": "🌙", "descripcion": "Luces apagadas y sin voz",
     "config": {"user.voice_enabled": false, "user.auto_speak": false}, "leds": {"on": false}}
  ])JSON");
  return M;
}

json sin_clave(const json& d) {
  json o = json::object();
  for (auto it = d.begin(); it != d.end(); ++it)
    if (it.key() != "token") o[it.key()] = it.value();
  return o;
}

}  // namespace

std::string nombre_estado(const std::string& e) {
  auto it = NOMBRES.find(e);
  return it == NOMBRES.end() ? e : it->second;
}

// ---------------------------------------------------------------- Leds
Leds::Leds(const fs::path& path) : path_(path.empty() ? paths::sub({"dispositivos"}) / "leds.json" : path) { load(); }

void Leds::load() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json j = jparse(read_text(path_));
  devices_ = j.is_object() && j.value("devices", json()).is_array() ? j["devices"] : json::array();
}

void Leds::save() { write_file(path_, jdump(json{{"devices", devices_}}, 1)); }

json Leds::list() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json out = json::array();
  for (auto& d : devices_) {
    json o = sin_clave(d);
    o["auth"] = truthy(d.value("token", json("")));
    out.push_back(o);
  }
  return out;
}

json Leds::devices() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  return devices_;
}

void Leds::set_udp_port(const std::string& did, int port) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  for (auto& d : devices_)
    if (jstr(d, "id") == did) d["udp_port"] = port;
}

json Leds::get(const std::string& did) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  for (auto& d : devices_)
    if (jstr(d, "id") == did) return d;
  throw std::out_of_range("dispositivo LED desconocido: " + did);
}

json Leds::add(std::string host, int port, const std::string& name, const std::string& token, std::string did) {
  host = trim(host);
  if (std::count(host.begin(), host.end(), ':') == 1) {
    port = std::stoi(host.substr(host.find(':') + 1));
    host = host.substr(0, host.find(':'));
  }
  json info = json::object();
  try {
    info = http({{"host", host}, {"port", port}, {"token", token}}, "GET", "/api/info", nullptr, 4);
  } catch (const std::exception& e) {   // se puede anadir apagado y probar luego
    LL.info("NexusLED ", host, ":", port, " no responde al anadirlo: ", e.what());
  }
  if (!info.is_object()) info = json::object();
  if (did.empty()) did = truthy(info.value("id", json())) ? jstr(info, "id") : host + ":" + std::to_string(port);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json keep = json::array();
  for (auto& d : devices_)
    if (jstr(d, "id") != did) keep.push_back(d);
  json dev = {{"id", did},
              {"name", !name.empty() ? name : truthy(info.value("name", json())) ? jstr(info, "name") : "NexusLED " + host},
              {"host", host},
              {"port", port},
              {"token", token},
              {"enabled", true},
              {"leds", info.value("leds", json(0))},
              {"version", info.value("version", json(""))},
              {"follow_state", true}};
  keep.push_back(dev);
  devices_ = keep;
  save();
  return sin_clave(dev);
}

json Leds::update(const std::string& did, const json& kw) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  for (auto& d : devices_) {
    if (jstr(d, "id") != did) continue;
    for (auto k : {"name", "enabled", "follow_state", "token", "host", "port"})
      if (kw.contains(k) && !kw[k].is_null()) d[k] = kw[k];
    save();
    return sin_clave(d);
  }
  throw std::out_of_range("dispositivo LED desconocido: " + did);
}

void Leds::remove(const std::string& did) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  json keep = json::array();
  for (auto& d : devices_)
    if (jstr(d, "id") != did) keep.push_back(d);
  devices_ = keep;
  save();
}

json Leds::discover(double timeout, const std::vector<std::string>& hosts) {
  std::vector<std::pair<std::string, int>> targets = {{"255.255.255.255", UDP_PORT}};
  for (auto& t : hosts) {
    auto c = t.find(':');
    targets.push_back(c == std::string::npos ? std::pair{t, UDP_PORT} : std::pair{t.substr(0, c), std::stoi(t.substr(c + 1))});
  }
  json found = json::object();
  for (auto& r : net::udp_broadcast(targets, "NEXUSLED?", timeout, 2)) {
    json info = jparse(r.data);
    if (!info.is_object() || !info.contains("nexusled")) continue;
    info["host"] = r.ip;
    info["udp_port"] = r.port;
    bool added = false;
    {
      std::lock_guard<std::recursive_mutex> lk(mu_);
      for (auto& d : devices_) added |= d.value("id", json()) == info.value("id", json());
    }
    info["added"] = added;
    found[truthy(info.value("id", json())) ? jstr(info, "id") : r.ip] = info;
  }
  json out = json::array();
  for (auto& [k, v] : found.items()) out.push_back(v);
  return out;
}

json Leds::http(const json& d, const std::string& method, const std::string& path, const json& body, double timeout) {
  http::Request req;
  req.method = method;
  req.url = "http://" + jstr(d, "host") + ":" + std::to_string(d.value("port", 80)) + path;
  req.timeout = timeout;
  req.follow = false;
  if (truthy(d.value("token", json(""))) ) req.headers.push_back({"X-Nexus-Token", jstr(d, "token")});
  if (!body.is_null()) {
    req.body = jdump(body);
    req.content_type = "application/json";
  }
  auto r = http::send(req);
  if (!r.status) throw std::runtime_error(r.error.empty() ? "sin respuesta" : r.error);
  if (r.status >= 400) {
    json j = r.json_body();
    std::string msg = j.is_object() && truthy(j.value("error", json())) ? jstr(j, "error") : r.body;
    throw std::invalid_argument("NexusLED " + jstr(d, "host") + ": " + msg);
  }
  if (r.body.empty()) return json::object();
  json j = r.json_body();
  if (j.is_discarded()) throw std::runtime_error("respuesta no valida del dispositivo");
  return j;
}

json Leds::call(const std::string& did, const std::string& method, const std::string& path, const json& body) {
  if (!starts_with(path, "/api/")) throw std::invalid_argument("ruta no permitida");
  return http(get(did), method, path, body);
}

int Leds::send_state(const std::string& state, double level, int r, int g, int b, int ttl) {
  auto it = std::find(ESTADOS.begin(), ESTADOS.end(), state);
  size_t idx = it == ESTADOS.end() ? 1 : (size_t)(it - ESTADOS.begin());
  int lv = level < 0 ? 255 : std::max(0, std::min(254, (int)(level * 254)));
  std::string pkt = {'N', 'X', 'L', (char)2, (char)idx, (char)lv, (char)r, (char)g, (char)b, (char)std::max(0, std::min(255, ttl))};
  int n = 0;
  std::lock_guard<std::recursive_mutex> lk(mu_);
  for (auto& d : devices_) {
    if (!d.value("enabled", true) || !d.value("follow_state", true)) continue;
    int port = truthy(d.value("udp_port", json())) ? d["udp_port"].get<int>() : UDP_PORT;
    if (net::udp_send(jstr(d, "host"), port, pkt))
      ++n;
    else
      LL.debug("NexusLED ", jstr(d, "host"), ": no se pudo enviar");
  }
  return n;
}

// ---------------------------------------------------------------- Hub
Hub::Hub(Services* s_) : s(s_), since_(now()) {}

json Hub::set_state(std::string state, const json& level, const json& texto, const std::string& origen) {
  if (!COLORES.count(state)) state = "idle";
  double lv = -1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state != state_) {
      since_ = now();
      ++seq_;
    }
    state_ = state;
    if (!level.is_null()) {
      level_ = std::max(0.0, std::min(1.0, jnum(level)));
      lv = jnum(level);
    }
    if (!texto.is_null()) texto_ = uleft(texto.is_string() ? texto.get<std::string>() : jdump(texto), 120);
  }
  leds.send_state(state, level.is_null() ? -1 : std::max(0.0, std::min(1.0, lv)));
  return snapshot();
}

json Hub::snapshot() {
  std::string modo = modo_actual(), pv = pico_version();
  std::lock_guard<std::mutex> lk(mu_);
  return {{"estado", state_},
          {"nombre", nombre_estado(state_)},
          {"color", COLORES.at(state_)},
          {"nivel", std::round(level_ * 100) / 100},
          {"desde", since_},
          {"seq", seq_},
          {"texto", texto_},
          {"modo", modo},
          {"pico_version", pv}};
}

void Hub::visto(const std::string& cid, const std::string& tipo, const json& extra) {
  if (cid.empty()) return;
  json c = {{"id", cid}, {"tipo", tipo}, {"visto", now()}};
  if (extra.is_object())
    for (auto it = extra.begin(); it != extra.end(); ++it) c[it.key()] = it.value();
  std::lock_guard<std::mutex> lk(mu_);
  clientes_[cid] = c;
}

json Hub::clientes_recientes(double max_age) {
  double t = now();
  std::lock_guard<std::mutex> lk(mu_);
  json out = json::array();
  for (auto& [k, c] : clientes_)
    if (t - jnum(c["visto"]) < max_age) out.push_back(c);
  return out;
}

json Hub::modos() {
  json extra = get_config().get("dispositivos.modos", json::array());
  std::set<std::string> ids;
  json out = json::array();
  if (extra.is_array())
    for (auto& m : extra)
      if (m.is_object() && truthy(m.value("id", json()))) ids.insert(jstr(m, "id"));
  for (auto& m : modos_defecto())
    if (!ids.count(jstr(m, "id"))) out.push_back(m);
  if (extra.is_array())
    for (auto& m : extra)
      if (m.is_object()) out.push_back(m);
  return out;
}

std::string Hub::modo_actual() {
  std::string m = get_config().str("dispositivos.modo", "normal");
  return m.empty() ? "normal" : m;
}

json Hub::aplicar_modo(const std::string& mid, const std::string& origen) {
  json modo;
  for (auto& m : modos())
    if (jstr(m, "id") == mid) {
      modo = m;
      break;
    }
  if (!modo.is_object()) throw std::invalid_argument("modo desconocido: " + mid);
  auto& cfg = get_config();
  json conf = modo.value("config", json::object());
  if (conf.is_object())
    for (auto it = conf.begin(); it != conf.end(); ++it) {
      const std::string& k = it.key();
      bool sensible = false;
      for (auto p : {"admin.", "nube.", "server.", "remote.", "vpn."}) sensible |= starts_with(k, p);
      if (sensible) continue;   // un modo nunca toca seguridad ni red
      cfg.set(k, it.value(), false);
    }
  if (truthy(modo.value("perfil_hw", json())))
    L.warning("Modo ", mid, ": no se pudo aplicar el perfil de hardware ", jstr(modo, "perfil_hw"),
              ": los perfiles de hardware con nombre aun no existen");
  cfg.set("dispositivos.modo", mid);
  json ledcfg = modo.value("leds", json());
  for (auto& d : leds.devices())
    if (d.value("enabled", true) && truthy(ledcfg)) {
      try {
        leds.call(jstr(d, "id"), "POST", "/api/state", ledcfg);
      } catch (const std::exception& e) {
        L.debug("modo en ", jstr(d, "host"), ": ", e.what());
      }
    }
  L.info("Modo ", mid, " activado", origen.empty() ? "" : " desde " + origen);
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++seq_;
  }
  return {{"modo", mid}, {"aplicado", modo}};
}

json Hub::resumen_reloj() {
  json act = agents::activity().all();
  std::vector<json> ags;
  if (s && s->agents)
    for (auto& a : s->agents->list()) {
      json st = act.value(a.id, json::object());
      ags.push_back({{"id", a.id},
                     {"nombre", uleft(a.name, 22)},
                     {"color", a.color},
                     {"icono", a.icon},
                     {"estado", st.value("estado", json("idle"))},
                     {"escena", st.value("escena", json("idle"))},
                     {"tarea", uleft(jstr(st, "tarea"), 60)},
                     {"segundos", st.value("segundos", json(0))},
                     {"tareas", st.value("tareas", json(0))}});
    }
  static const std::map<std::string, int> orden = {{"tool", 0},       {"thinking", 1}, {"writing", 1},
                                                   {"delegating", 2}, {"meeting", 3},  {"idle", 9}};
  auto rank = [&](const json& a) {
    auto it = orden.find(jstr(a, "estado"));
    return it == orden.end() ? 5 : it->second;
  };
  std::stable_sort(ags.begin(), ags.end(), [&](const json& x, const json& y) {
    int rx = rank(x), ry = rank(y);
    return rx != ry ? rx < ry : jstr(x, "nombre") < jstr(y, "nombre");
  });
  if (ags.size() > 24) ags.resize(24);
  json ms = json::array();
  for (auto& m : modos()) ms.push_back({{"id", m["id"]}, {"nombre", m["nombre"]}, {"icono", m.value("icono", json(""))}});
  json out = snapshot();
  out["agentes"] = json(ags);
  out["modos"] = ms;
  out["hora"] = strftime_local("%H:%M");
  return out;
}

// ---------------------------------------------------------------- Pico W (tinta electronica)
std::string Hub::pico_version() {
  auto& cfg = get_config();
  std::string name = cfg.str("user.assistant_name", "Nexus-Office");
  if (name.empty()) name = "Nexus-Office";
  std::string style = cfg.str("dispositivos.pico.estilo", "avatar");
  return sha1_hex(name + "|" + style + "|3").substr(0, 10);
}

namespace {

struct Gray {
  int w, h;
  std::vector<uint8_t> px;
  Gray(int w_, int h_, uint8_t v = 255) : w(w_), h(h_), px((size_t)w_ * h_, v) {}
  uint8_t& at(int x, int y) { return px[(size_t)y * w + x]; }
  void put(int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < w && y < h) at(x, y) = v;
  }
  void rect(int x0, int y0, int x1, int y1, uint8_t v) {   // relleno, extremos incluidos (como Pillow)
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) put(x, y, v);
  }
};

// autocontraste de Pillow (cutoff en % por cada extremo del histograma)
void autocontrast(Gray& g, double cutoff) {
  long hist[256] = {0};
  for (auto v : g.px) ++hist[v];
  long n = (long)g.px.size(), cut = (long)(n * cutoff / 100);
  int lo = 0, hi = 255;
  for (long c = cut; lo < 255; ++lo) {
    if (hist[lo] > c) break;
    c -= hist[lo];
  }
  for (long c = cut; hi > 0; --hi) {
    if (hist[hi] > c) break;
    c -= hist[hi];
  }
  if (hi <= lo) return;
  double scale = 255.0 / (hi - lo), offset = -lo * scale;
  for (auto& v : g.px) v = (uint8_t)std::clamp((int)(v * scale + offset), 0, 255);
}

// thumbnail: reduce por promedio de area hasta caber en (mw, mh) sin cambiar la proporcion
Gray thumbnail(const Gray& g, int mw, int mh) {
  double f = std::min({1.0, (double)mw / g.w, (double)mh / g.h});
  int nw = std::max(1, (int)std::lround(g.w * f)), nh = std::max(1, (int)std::lround(g.h * f));
  if (nw == g.w && nh == g.h) return g;
  Gray o(nw, nh);
  for (int y = 0; y < nh; ++y)
    for (int x = 0; x < nw; ++x) {
      int x0 = x * g.w / nw, x1 = std::max(x0 + 1, (x + 1) * g.w / nw);
      int y0 = y * g.h / nh, y1 = std::max(y0 + 1, (y + 1) * g.h / nh);
      long s = 0, c = 0;
      for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) s += g.px[(size_t)yy * g.w + xx], ++c;
      o.at(x, y) = (uint8_t)(s / std::max(1L, c));
    }
  return o;
}

const std::string& font_bold() {
  static const std::string F = [] {
    for (auto* p : {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/seguisym.ttf",
                    "C:/Windows/Fonts/arialbd.ttf"})
      if (is_file(P(p))) return read_text(P(p));
    try {
      return raster::load_font();
    } catch (const std::exception&) {
      return std::string();
    }
  }();
  return F;
}

void png_sink(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, size); }

}  // namespace

std::vector<uint8_t> Hub::pico_imagen(const std::string& state_in, int ancho, int alto) {
  std::string state = COLORES.count(state_in) ? state_in : "idle";
  auto& cfg = get_config();
  Gray img(ancho, alto);
  fs::path face = paths::web() / "assets" / "geisha" / "face_front.png";
  int x0 = 0;
  if (is_file(face) && cfg.str("dispositivos.pico.estilo", "avatar") == "avatar") {
    try {
      raster::Image f = raster::to_rgba(raster::load(face));
      Gray g(f.w, f.h);
      for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
          const uint8_t* p = f.at(x, y);
          double a = p[3] / 255.0;
          double r = p[0] * a + 255 * (1 - a), gg = p[1] * a + 255 * (1 - a), b = p[2] * a + 255 * (1 - a);
          g.at(x, y) = (uint8_t)std::clamp((int)((r * 299 + gg * 587 + b * 114) / 1000), 0, 255);   // modo L
        }
      autocontrast(g, 2);
      g = thumbnail(g, alto, alto);
      if (state == "thinking") {   // cabeza ladeada: transform AFFINE (1, 0.12, -8, 0, 1, 0)
        Gray t(g.w, g.h);
        for (int y = 0; y < g.h; ++y)
          for (int x = 0; x < g.w; ++x) {
            int sx = (int)std::floor(x + 0.12 * y - 8 + 0.5);
            if (sx >= 0 && sx < g.w) t.at(x, y) = g.at(sx, y);
          }
        g = t;
      } else if (state == "error") {
        for (auto& v : g.px) v = 255 - v;
      }
      int oy = (alto - g.h) / 2;
      for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) img.put(x, oy + y, g.at(x, y));
      x0 = g.w + 4;
    } catch (const std::exception& e) {
      L.debug("avatar del Pico: ", e.what());
    }
  }
  const std::string& font = font_bold();
  std::string name = cfg.str("user.assistant_name", "Nexus-Office");
  if (name.empty()) name = "Nexus-Office";
  name = uleft(name, 16);
  if (!font.empty()) {
    raster::text_gray(img.px, img.w, img.h, x0, 4, font, name, 13, 0);
    raster::text_gray(img.px, img.w, img.h, x0, 28, font, CARAS.count(state) ? CARAS.at(state) : "", 20, 0);
    std::string label = upper(nombre_estado(state));
    int sz = 18;
    while (sz > 9 && raster::text_length(font, label, sz) > ancho - x0 - 4) --sz;
    raster::text_gray(img.px, img.w, img.h, x0, 62, font, label, sz, 0);
  }
  img.rect(x0, 21, ancho - 4, 21, 0);
  // icono del estado: ondas al escuchar, puntos al pensar, barras al hablar
  int cy = 98;
  if (state == "listening") {
    for (int r : {6, 12, 18})
      for (int wdt = 0; wdt < 2; ++wdt)
        for (double a = 300; a <= 420; a += 0.5) {
          double rad = a * 3.14159265358979323846 / 180, rr = r - wdt;
          img.put((int)std::lround(x0 + 20 + rr * std::cos(rad)), (int)std::lround(cy + rr * std::sin(rad)), 0);
        }
  } else if (state == "thinking") {
    for (int i = 0; i < 3; ++i) {
      double cx = x0 + 8 + i * 14;
      for (int y = cy - 4; y <= cy + 4; ++y)
        for (int x = (int)cx - 4; x <= (int)cx + 4; ++x)
          if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 16.5) img.put(x, y, 0);
    }
  } else if (state == "speaking") {
    int hs[] = {6, 14, 20, 12, 8, 16};
    for (int i = 0; i < 6; ++i) img.rect(x0 + 4 + i * 8, cy - hs[i] / 2, x0 + 9 + i * 8, cy + hs[i] / 2, 0);
  } else if (state == "tool") {
    for (int k = 0; k < 2; ++k) {
      img.rect(x0 + 4 + k, cy - 8 + k, x0 + 30 - k, cy - 8 + k, 0);
      img.rect(x0 + 4 + k, cy + 8 - k, x0 + 30 - k, cy + 8 - k, 0);
      img.rect(x0 + 4 + k, cy - 8 + k, x0 + 4 + k, cy + 8 - k, 0);
      img.rect(x0 + 30 - k, cy - 8 + k, x0 + 30 - k, cy + 8 - k, 0);
    }
    img.rect(x0 + 30, cy - 1, x0 + 44, cy + 1, 0);
  }
  // blanco y negro con tramado de Floyd-Steinberg (convert("1") de Pillow)
  std::vector<int> e(img.px.begin(), img.px.end());
  std::vector<uint8_t> out(img.px.size());
  for (int y = 0; y < alto; ++y)
    for (int x = 0; x < ancho; ++x) {
      size_t i = (size_t)y * ancho + x;
      int old = std::clamp(e[i], 0, 255), nv = old < 128 ? 0 : 255, err = old - nv;
      out[i] = (uint8_t)nv;
      if (x + 1 < ancho) e[i + 1] += err * 7 / 16;
      if (y + 1 < alto) {
        if (x > 0) e[i + ancho - 1] += err * 3 / 16;
        e[i + ancho] += err * 5 / 16;
        if (x + 1 < ancho) e[i + ancho + 1] += err / 16;
      }
    }
  return out;
}

std::string Hub::pico_buffer(const std::string& state) {
  // pantalla Waveshare 2.13" (V3/V4): 122x250 en vertical, MONO_HLSB, 1 = blanco. La imagen apaisada (250x122) se gira
  // 90 grados a la izquierda como hace el controlador de Waveshare
  const int W = 250, H = 122;
  auto img = pico_imagen(state, W, H);
  int w = H, h = W, wb = (w + 7) / 8;
  std::string out((size_t)wb * h, '\0');
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (img[(size_t)x * W + (W - 1 - y)]) out[(size_t)y * wb + x / 8] |= (char)(0x80 >> (x % 8));
  return out;
}

std::string Hub::pico_png(const std::string& state) {
  auto img = pico_imagen(state);
  std::string out;
  stbi_write_png_to_func(png_sink, &out, 250, 122, 1, img.data(), 250);
  return out;
}

json Hub::pico_manifiesto() {
  json est = json::array();
  for (auto& s : ESTADOS)
    if (s != "none") est.push_back(s);
  return {{"version", pico_version()}, {"ancho", 250}, {"alto", 122}, {"formato", "MONO_HLSB 122x250 vertical"},
          {"estados", est}};
}

namespace {
std::mutex g_mu;
std::unique_ptr<Hub> g_hub;
}  // namespace

Hub& hub(Services* s) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_hub)
    g_hub = std::make_unique<Hub>(s);
  else if (s)
    g_hub->s = s;
  return *g_hub;
}

void reset() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_hub.reset();
}

}  // namespace nx::devices
