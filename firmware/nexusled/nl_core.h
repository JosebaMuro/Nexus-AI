// NexusLED - nucleo portable: estado, segmentos, presets, API HTTP/UDP/serie y render.
// No depende de Arduino: el mismo codigo corre en el ESP32 y en el emulador del PC.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include "nl_color.h"
#include "nl_effects.h"
#include "nl_json.h"

#define NEXUSLED_VERSION "1.0.0"
#define NEXUSLED_API 1

namespace nl {

static const int MAX_OUTPUTS = 4;
static const int MAX_LEDS = 1500;
static const int MAX_SEGMENTS = 8;
static const int MAX_PRESETS = 16;

struct OutputCfg {
  int pin = 16;
  int count = 30;
  std::string order = "GRB";  // orden de color del chip (WS2812: GRB)
  bool rgbw = false;          // SK6812 RGBW
  bool reverse = false;
};

struct Preset {
  int id = 0;
  std::string name;
  std::string state;  // JSON del estado
};

struct Request {
  std::string method;  // GET, POST, DELETE, OPTIONS
  std::string path;
  std::vector<std::pair<std::string, std::string>> query;
  std::string body;
  std::string token;   // cabecera X-Nexus-Token
  std::string q(const char* k, const char* def = "") const {
    for (auto& kv : query)
      if (kv.first == k) return kv.second;
    return def;
  }
};

struct Response {
  int code = 200;
  std::string type = "application/json";
  std::string body;
};

inline std::string lower(std::string s) {
  for (auto& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
  return s;
}

class Device {
 public:
  // ------------------------------------------------ configuracion (se guarda)
  std::string name = "NexusLED";
  std::string deviceId = "000000";  // lo pone la plataforma (MAC)
  std::vector<OutputCfg> outputs{OutputCfg()};
  std::string token;                 // vacio = sin clave
  std::string ssid, pass;
  std::string apPass = "nexusled";
  int maxmA = 0;                     // limite de corriente (0 = sin limite)
  int ledmA = 55;                    // consumo de un LED en blanco a tope
  bool gamma = true;
  int fps = 50;
  int relayPin = -1;
  bool relayInvert = false;
  int buttonPin = -1;
  bool udpLive = true;               // aceptar tramas de pixeles por UDP
  int udpPort = 4210;
  int httpPort = 80;

  // ------------------------------------------------ estado
  bool on = true;
  uint8_t bri = 128;
  uint16_t transition = 700;  // ms
  int preset = -1;
  std::vector<Segment> segs;
  NexusStatus nx;
  std::vector<Preset> presets;

  // ------------------------------------------------ tiempo real y salida
  std::vector<RGB> live;
  uint32_t liveUntil = 0;
  std::vector<RGB> frame;  // ultima imagen (con brillo y limite de corriente)
  int powermA = 0;

  // banderas para la plataforma
  bool configDirty = false, presetsDirty = false, outputsChanged = false, wifiChanged = false;
  bool rebootRequested = false, factoryRequested = false;
  uint32_t stateDirtyAt = 0;  // 0 = guardado
  Rng rng;
  std::function<void(JWriter&)> extraInfo;  // la plataforma anade IP, RSSI, memoria...
  std::function<uint32_t()> clock;          // ms desde el arranque

  Device() { ensureSegments(); }

  int total() const {
    int t = 0;
    for (auto& o : outputs) t += o.count;
    return t;
  }

  uint32_t now() const { return clock ? clock() : 0; }

  void ensureSegments() {
    int n = total();
    if (segs.empty()) {
      Segment s;
      s.start = 0;
      s.stop = n;
      s.fx = 5;  // arcoiris en movimiento: se ve que funciona nada mas encender
      segs.push_back(s);
    }
    for (auto& s : segs) {
      if (s.stop > n) s.stop = n;
      if (s.start > s.stop) s.start = s.stop;
    }
    frame.resize(n);
  }

  // ------------------------------------------------ busquedas por nombre
  static int findEffect(const JVal& v) {
    if (v.isNum()) {
      int i = (int)v.n;
      return i >= 0 && i < EFFECT_COUNT ? i : -1;
    }
    if (!v.isStr()) return -1;
    std::string k = lower(v.s);
    for (int i = 0; i < EFFECT_COUNT; i++)
      if (k == EFFECTS[i].key || k == lower(EFFECTS[i].name)) return i;
    char* end = nullptr;
    long n = strtol(v.s.c_str(), &end, 10);
    if (end && *end == 0 && n >= 0 && n < EFFECT_COUNT) return (int)n;
    return -1;
  }
  static int findPalette(const JVal& v) {
    if (v.isNum()) {
      int i = (int)v.n;
      return i >= 0 && i < PALETTE_COUNT ? i : -1;
    }
    if (!v.isStr()) return -1;
    std::string k = lower(v.s);
    for (int i = 0; i < PALETTE_COUNT; i++)
      if (k == PALETTES[i].key || k == lower(PALETTES[i].name)) return i;
    return -1;
  }
  static int findNexusState(const std::string& s) {
    std::string k = lower(s);
    if (k.empty() || k == "off" || k == "clear") return 0;
    for (int i = 0; i < NEXUS_STATE_COUNT; i++)
      if (k == NEXUS_STATES[i].key) return i;
    return -1;
  }
  static bool parseColor(const JVal& v, RGB& out) {
    if (v.isStr()) return parseHex(v.s, out);
    if (v.isArr() && v.a.size() >= 3) {
      int c[3];
      for (int i = 0; i < 3; i++) {
        double x = v.a[i].num(-1);
        if (x < 0 || x > 255) return false;
        c[i] = (int)x;
      }
      out = RGB(c[0], c[1], c[2]);
      return true;
    }
    if (v.isNum() && v.n >= 0 && v.n <= 0xFFFFFF) {
      uint32_t x = (uint32_t)v.n;
      out = RGB((x >> 16) & 255, (x >> 8) & 255, x & 255);
      return true;
    }
    return false;
  }
  static uint8_t clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5); }

  // ------------------------------------------------ aplicar estado
  // Devuelve false y rellena err si algo no es valido; lo valido se aplica igualmente.
  bool applySegment(Segment& s, const JVal& p, std::string& err) {
    bool ok = true;
    int n = total();
    if (const JVal* v = p.get("start")) s.start = (int)v->num(s.start);
    if (const JVal* v = p.get("stop")) s.stop = (int)v->num(s.stop);
    if (s.start < 0) s.start = 0;
    if (s.stop > n) s.stop = n;
    if (s.start > s.stop) s.start = s.stop;
    if (const JVal* v = p.get("on")) s.on = v->truthy(s.on);
    if (const JVal* v = p.get("bri")) s.bri = clamp8(v->num(s.bri));
    const JVal* fx = p.get("fx");
    if (!fx) fx = p.get("effect");
    if (fx) {
      int i = findEffect(*fx);
      if (i < 0) { err = "efecto desconocido"; ok = false; }
      else s.fx = (uint8_t)i;
    }
    const JVal* sp = p.get("sx");
    if (!sp) sp = p.get("speed");
    if (sp) s.speed = clamp8(sp->num(s.speed));
    const JVal* ix = p.get("ix");
    if (!ix) ix = p.get("intensity");
    if (ix) s.intensity = clamp8(ix->num(s.intensity));
    const JVal* pal = p.get("pal");
    if (!pal) pal = p.get("palette");
    if (pal) {
      int i = findPalette(*pal);
      if (i < 0) { err = "paleta desconocida"; ok = false; }
      else s.pal = (uint8_t)i;
    }
    const JVal* col = p.get("col");
    if (!col) col = p.get("colors");
    if (col && col->isArr()) {
      // ["#ff0000", ...] o [[255,0,0], ...]; un solo [r,g,b] tambien vale
      bool single = col->a.size() >= 3 && col->a[0].isNum();
      if (single) {
        RGB c;
        if (parseColor(*col, c)) s.col[0] = c;
        else { err = "color no valido"; ok = false; }
      } else {
        for (size_t i = 0; i < col->a.size() && i < 3; i++) {
          if (col->a[i].isNull()) continue;
          RGB c;
          if (parseColor(col->a[i], c)) s.col[i] = c;
          else { err = "color no valido"; ok = false; }
        }
      }
    }
    if (const JVal* v = p.get("color")) {
      RGB c;
      if (parseColor(*v, c)) s.col[0] = c;
      else { err = "color no valido"; ok = false; }
    }
    if (const JVal* v = p.get("rev")) s.reverse = v->truthy(s.reverse);
    if (const JVal* v = p.get("mi")) s.mirror = v->truthy(s.mirror);
    if (const JVal* v = p.get("name")) if (v->isStr()) s.name = v->s.substr(0, 32);
    if (s.lastFx != s.fx) {
      s.reset();
      s.lastFx = s.fx;
    }
    return ok;
  }

  void startTransition(uint32_t ms) {
    transFrom_ = frame;
    transStart_ = now();
    transDur_ = ms;
  }

  bool applyState(const JVal& p, std::string& err, bool fromPreset = false) {
    if (!p.isObj()) { err = "se esperaba un objeto JSON"; return false; }
    bool ok = true;
    bool visual = false;
    uint16_t trans = transition;
    if (const JVal* v = p.get("transition")) {
      double t = v->num(transition);
      trans = (uint16_t)(t < 0 ? 0 : t > 60000 ? 60000 : t);
      if (!p.has("tt")) transition = trans;  // "tt" = solo para este cambio
    }
    if (const JVal* v = p.get("tt")) trans = (uint16_t)v->num(trans);

    const JVal* ps = p.get("preset");
    if (!ps) ps = p.get("ps");
    if (ps && !fromPreset) {
      int id = (int)ps->num(-1);
      bool found = false;
      for (auto& pr : presets)
        if (pr.id == id) {
          JVal st;
          if (parseJson(pr.state, st)) {
            applyState(st, err, true);
            preset = id;
            found = true;
            visual = true;
          }
        }
      if (!found) { err = "preset inexistente"; ok = false; }
    }
    if (const JVal* v = p.get("on")) {
      if (v->isStr() && v->s == "t") on = !on;
      else on = v->truthy(on);
      visual = true;
    }
    if (const JVal* v = p.get("bri")) { bri = clamp8(v->num(bri)); visual = true; }

    // segmentos: "seg": [{id, ...}] o un solo objeto
    if (const JVal* sg = p.get("seg")) {
      std::vector<const JVal*> list;
      if (sg->isArr()) for (auto& e : sg->a) list.push_back(&e);
      else if (sg->isObj()) list.push_back(sg);
      for (size_t k = 0; k < list.size(); k++) {
        const JVal& e = *list[k];
        int id = (int)(e.get("id") ? e.get("id")->num(k) : k);
        if (id < 0 || id >= MAX_SEGMENTS) { err = "segmento fuera de rango"; ok = false; continue; }
        // stop 0 (con id > 0) borra el segmento
        if (id > 0 && e.get("stop") && e.get("stop")->num(1) == 0 && !e.get("start")) {
          if (id < (int)segs.size()) segs.erase(segs.begin() + id);
          visual = true;
          continue;
        }
        while ((int)segs.size() <= id) {
          Segment ns;
          ns.start = segs.empty() ? 0 : segs.back().stop;
          ns.stop = total();
          segs.push_back(ns);
        }
        if (!applySegment(segs[id], e, err)) ok = false;
        visual = true;
      }
    }
    // atajos de nivel superior: se aplican a todos los segmentos
    static const char* SHORT[] = {"fx", "effect", "sx", "speed", "ix", "intensity", "pal", "palette", "col", "colors", "color"};
    bool hasShort = false;
    for (auto k : SHORT)
      if (p.has(k)) hasShort = true;
    if (hasShort) {
      JVal sub;
      sub.t = JVal::Obj;
      for (auto& kv : p.o)
        for (auto k : SHORT)
          if (kv.first == k) sub.o.push_back(kv);
      for (auto& s : segs)
        if (!applySegment(s, sub, err)) ok = false;
      visual = true;
    }
    if (!fromPreset && (hasShort || p.has("seg") || p.has("bri") || p.has("on"))) preset = ps ? preset : -1;

    // estado de NEXUS
    if (const JVal* n = p.get("nexus")) {
      if (!applyNexus(*n, err)) ok = false;
    }
    if (visual && !fromPreset) {
      startTransition(trans);
      stateDirtyAt = now() ? now() : 1;
    }
    if (const JVal* v = p.get("save"))
      if (v->truthy()) stateDirtyAt = 1;  // guardar ya
    return ok;
  }

  bool applyNexus(const JVal& n, std::string& err) {
    std::string st;
    if (n.isStr()) st = n.s;
    else if (const JVal* v = n.get("status")) st = v->isStr() ? v->s : "";
    else if (const JVal* v2 = n.get("state")) st = v2->isStr() ? v2->s : "";
    bool hasStatus = n.isStr() || n.has("status") || n.has("state");
    if (hasStatus) {
      int i = findNexusState(st);
      if (i < 0) { err = "estado de NEXUS desconocido"; return false; }
      if (i != nx.state) {
        startTransition(transition < 300 ? transition : 300);
        nx.since = now();
      }
      nx.state = (uint8_t)i;
      nx.color = NEXUS_STATES[i].color;
      nx.until = 0;
      nx.level = -1;
    }
    if (n.isObj()) {
      if (const JVal* v = n.get("color")) {
        RGB c;
        if (parseColor(*v, c)) nx.color = c;
      }
      if (const JVal* v = n.get("level")) {
        double l = v->num(-1);
        nx.level = l < 0 ? -1 : (float)(l > 1 ? 1 : l);
        nx.levelAt = now();
      }
      if (const JVal* v = n.get("ttl")) {
        double t = v->num(0);
        nx.until = t > 0 ? now() + (uint32_t)(t * 1000) : 0;
        if (nx.until == 0 && t > 0) nx.until = 1;
      }
    }
    return true;
  }

  // ------------------------------------------------ configuracion
  bool applyConfig(const JVal& c, std::string& err) {
    if (!c.isObj()) { err = "se esperaba un objeto JSON"; return false; }
    bool ok = true;
    if (const JVal* v = c.get("name")) if (v->isStr() && !v->s.empty()) name = v->s.substr(0, 32);
    if (const JVal* v = c.get("token")) if (v->isStr()) token = v->s.substr(0, 64);
    if (const JVal* v = c.get("max_ma")) maxmA = (int)v->num(maxmA);
    if (const JVal* v = c.get("led_ma")) ledmA = (int)v->num(ledmA);
    if (const JVal* v = c.get("gamma")) gamma = v->truthy(gamma);
    if (const JVal* v = c.get("fps")) { fps = (int)v->num(fps); if (fps < 5) fps = 5; if (fps > 120) fps = 120; }
    if (const JVal* v = c.get("udp_live")) udpLive = v->truthy(udpLive);
    if (const JVal* v = c.get("relay_pin")) { relayPin = (int)v->num(relayPin); outputsChanged = true; }
    if (const JVal* v = c.get("relay_invert")) relayInvert = v->truthy(relayInvert);
    if (const JVal* v = c.get("button_pin")) { buttonPin = (int)v->num(buttonPin); outputsChanged = true; }
    if (const JVal* v = c.get("ap_pass")) {
      if (v->isStr() && (v->s.empty() || v->s.size() >= 8)) { apPass = v->s; wifiChanged = true; }
      else { err = "la clave del punto de acceso necesita 8 caracteres o mas"; ok = false; }
    }
    if (const JVal* w = c.get("wifi")) {
      if (const JVal* v = w->get("ssid")) if (v->isStr()) { ssid = v->s.substr(0, 32); wifiChanged = true; }
      if (const JVal* v = w->get("pass")) if (v->isStr()) { pass = v->s.substr(0, 64); wifiChanged = true; }
    }
    if (const JVal* o = c.get("outputs")) {
      if (!o->isArr() || o->a.empty() || o->a.size() > MAX_OUTPUTS) {
        err = "outputs: entre 1 y 4 salidas";
        ok = false;
      } else {
        std::vector<OutputCfg> outs;
        int sum = 0;
        for (auto& e : o->a) {
          OutputCfg oc;
          if (const JVal* v = e.get("pin")) oc.pin = (int)v->num(oc.pin);
          if (const JVal* v = e.get("count")) oc.count = (int)v->num(oc.count);
          if (const JVal* v = e.get("order")) {
            std::string ord = v->isStr() ? v->s : "";
            for (auto& ch : ord) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            if (ord.size() == 3 && ord.find('R') != std::string::npos && ord.find('G') != std::string::npos &&
                ord.find('B') != std::string::npos)
              oc.order = ord;
            else { err = "orden de color no valido (GRB, RGB, BRG...)"; ok = false; }
          }
          if (const JVal* v = e.get("rgbw")) oc.rgbw = v->truthy();
          if (const JVal* v = e.get("reverse")) oc.reverse = v->truthy();
          if (oc.count < 1) oc.count = 1;
          if (oc.pin < 0 || oc.pin > 48) { err = "pin no valido"; ok = false; }
          sum += oc.count;
          outs.push_back(oc);
        }
        if (sum > MAX_LEDS) { err = "demasiados LED (maximo 1500)"; ok = false; }
        else {
          int oldTotal = total();
          outputs = outs;
          outputsChanged = true;
          // si el unico segmento cubria toda la tira, que siga cubriendola
          if (segs.size() == 1 && segs[0].start == 0 && segs[0].stop == oldTotal) segs[0].stop = total();
          ensureSegments();
          for (auto& s : segs) s.reset();
        }
      }
    }
    configDirty = true;
    return ok;
  }

  // ------------------------------------------------ JSON de salida
  void writeSegment(JWriter& w, const Segment& s, int id) const {
    w.obj().kv("id", id).kv("start", s.start).kv("stop", s.stop).kv("len", s.len());
    w.kvb("on", s.on).kv("bri", (int)s.bri).kv("fx", (int)s.fx).kv("fx_key", EFFECTS[s.fx].key);
    w.kv("sx", (int)s.speed).kv("ix", (int)s.intensity).kv("pal", (int)s.pal).kv("pal_key", PALETTES[s.pal].key);
    w.key("col").arr();
    for (int i = 0; i < 3; i++) w.str(toHex(s.col[i]));
    w.end();
    w.kvb("rev", s.reverse).kvb("mi", s.mirror).kv("name", s.name).end();
  }
  // full=false: solo lo que se guarda en un preset
  std::string stateJson(bool full = true) const {
    JWriter w;
    w.obj().kvb("on", on).kv("bri", (int)bri).kv("transition", (int)transition);
    if (full) w.kv("preset", preset);
    w.key("seg").arr();
    for (size_t i = 0; i < segs.size(); i++) writeSegment(w, segs[i], (int)i);
    w.end();
    if (full) {
      w.key("nexus").obj().kv("status", NEXUS_STATES[nx.state].key).kv("color", toHex(nx.color));
      w.kv("level", (double)nx.level).end();
      w.kvb("live", liveActive());
      w.kv("power_ma", powermA);
    }
    w.end();
    return w.out;
  }
  std::string infoJson() const {
    JWriter w;
    w.obj().kv("firmware", "nexusled").kv("version", NEXUSLED_VERSION).kv("api", NEXUSLED_API);
    w.kv("name", name).kv("id", deviceId).kv("leds", total()).kv("segments", (int)segs.size());
    w.kv("effects", EFFECT_COUNT).kv("palettes", PALETTE_COUNT).kv("presets", (int)presets.size());
    w.kvb("auth", !token.empty()).kv("udp_port", udpPort).kvb("udp_live", udpLive);
    w.kv("max_ma", maxmA).kv("power_ma", powermA).kv("fps", fps);
    w.key("outputs").arr();
    for (auto& o : outputs) w.obj().kv("pin", o.pin).kv("count", o.count).kv("order", o.order).kvb("rgbw", o.rgbw).kvb("reverse", o.reverse).end();
    w.end();
    if (extraInfo) extraInfo(w);
    w.end();
    return w.out;
  }
  std::string configJson(bool secrets) const {
    JWriter w;
    w.obj().kv("name", name).kv("max_ma", maxmA).kv("led_ma", ledmA).kvb("gamma", gamma).kv("fps", fps);
    w.kv("relay_pin", relayPin).kvb("relay_invert", relayInvert).kv("button_pin", buttonPin).kvb("udp_live", udpLive);
    w.key("outputs").arr();
    for (auto& o : outputs) w.obj().kv("pin", o.pin).kv("count", o.count).kv("order", o.order).kvb("rgbw", o.rgbw).kvb("reverse", o.reverse).end();
    w.end();
    w.key("wifi").obj().kv("ssid", ssid);
    if (secrets) w.kv("pass", pass);
    else w.kvb("has_pass", !pass.empty());
    w.end();
    if (secrets) w.kv("token", token).kv("ap_pass", apPass);
    else w.kvb("has_token", !token.empty());
    w.end();
    return w.out;
  }
  std::string effectsJson() const {
    JWriter w;
    w.arr();
    for (int i = 0; i < EFFECT_COUNT; i++) w.obj().kv("id", i).kv("key", EFFECTS[i].key).kv("name", EFFECTS[i].name).end();
    w.end();
    return w.out;
  }
  std::string palettesJson() const {
    JWriter w;
    w.arr();
    for (int i = 0; i < PALETTE_COUNT; i++) w.obj().kv("id", i).kv("key", PALETTES[i].key).kv("name", PALETTES[i].name).end();
    w.end();
    return w.out;
  }
  std::string nexusStatesJson() const {
    JWriter w;
    w.arr();
    for (int i = 0; i < NEXUS_STATE_COUNT; i++)
      w.obj().kv("id", i).kv("key", NEXUS_STATES[i].key).kv("name", NEXUS_STATES[i].name).kv("color", toHex(NEXUS_STATES[i].color)).end();
    w.end();
    return w.out;
  }
  std::string presetsJson(bool withState = true) const {
    JWriter w;
    w.arr();
    for (auto& p : presets) {
      w.obj().kv("id", p.id).kv("name", p.name);
      if (withState) w.key("state").raw(p.state);
      w.end();
    }
    w.end();
    return w.out;
  }
  std::string frameJson() const {
    static const char* H = "0123456789abcdef";
    std::string hex;
    hex.reserve(frame.size() * 6);
    for (auto& c : frame) {
      uint8_t b[3] = {c.r, c.g, c.b};
      for (int i = 0; i < 3; i++) {
        hex += H[b[i] >> 4];
        hex += H[b[i] & 15];
      }
    }
    JWriter w;
    w.obj().kv("n", (int)frame.size()).kv("hex", hex).kvb("on", on).end();
    return w.out;
  }

  // ------------------------------------------------ persistencia
  bool loadConfig(const std::string& s) {
    JVal v;
    if (!parseJson(s, v)) return false;
    std::string err;
    applyConfig(v, err);
    if (const JVal* t = v.get("token")) if (t->isStr()) token = t->s;
    configDirty = outputsChanged = wifiChanged = false;
    return true;
  }
  bool loadState(const std::string& s) {
    JVal v;
    if (!parseJson(s, v)) return false;
    std::string err;
    segs.clear();
    ensureSegments();
    applyState(v, err, true);
    if (const JVal* p = v.get("preset")) preset = (int)p->num(-1);
    stateDirtyAt = 0;
    return true;
  }
  bool loadPresets(const std::string& s) {
    JVal v;
    if (!parseJson(s, v) || !v.isArr()) return false;
    presets.clear();
    for (auto& e : v.a) {
      Preset p;
      p.id = (int)(e.get("id") ? e.get("id")->num(0) : 0);
      if (const JVal* n = e.get("name")) p.name = n->s;
      if (const JVal* st = e.get("state")) p.state = writeVal(*st);
      if (p.id > 0) presets.push_back(p);
    }
    return true;
  }
  static std::string writeVal(const JVal& v) {
    JWriter w;
    writeValTo(w, v);
    return w.out;
  }
  static void writeValTo(JWriter& w, const JVal& v) {
    switch (v.t) {
      case JVal::Null: w.null(); break;
      case JVal::Bool: w.boolean(v.b); break;
      case JVal::Num: w.num(v.n); break;
      case JVal::Str: w.str(v.s); break;
      case JVal::Arr:
        w.arr();
        for (auto& e : v.a) writeValTo(w, e);
        w.end();
        break;
      case JVal::Obj:
        w.obj();
        for (auto& kv : v.o) {
          w.key(kv.first.c_str());
          writeValTo(w, kv.second);
        }
        w.end();
        break;
    }
  }
  // el estado se guarda 5 s despues del ultimo cambio para no gastar la flash
  bool stateNeedsSave(uint32_t t) const { return stateDirtyAt && (stateDirtyAt == 1 || t - stateDirtyAt > 5000); }

  // ------------------------------------------------ presets
  bool savePreset(const JVal& p, std::string& err, int& outId) {
    int id = p.get("id") ? (int)p.get("id")->num(0) : 0;
    if (id <= 0) {
      id = 1;
      for (auto& pr : presets)
        if (pr.id >= id) id = pr.id + 1;
    }
    if (id > 250) { err = "id de preset no valido"; return false; }
    std::string nm = p.get("name") && p.get("name")->isStr() ? p.get("name")->s.substr(0, 32) : "";
    std::string st;
    if (const JVal* s = p.get("state")) {
      if (!s->isObj()) { err = "state debe ser un objeto"; return false; }
      st = writeVal(*s);
    } else {
      st = stateJson(false);
    }
    for (auto& pr : presets)
      if (pr.id == id) {
        pr.state = st;
        if (!nm.empty()) pr.name = nm;
        presetsDirty = true;
        outId = id;
        return true;
      }
    if ((int)presets.size() >= MAX_PRESETS) { err = "maximo 16 presets"; return false; }
    Preset np;
    np.id = id;
    np.name = nm.empty() ? "Preset " + std::to_string(id) : nm;
    np.state = st;
    presets.push_back(np);
    presetsDirty = true;
    outId = id;
    return true;
  }
  bool deletePreset(int id) {
    for (size_t i = 0; i < presets.size(); i++)
      if (presets[i].id == id) {
        presets.erase(presets.begin() + i);
        presetsDirty = true;
        if (preset == id) preset = -1;
        return true;
      }
    return false;
  }

  // ------------------------------------------------ tiempo real
  bool liveActive() const { return liveUntil && (int32_t)(liveUntil - now()) > 0; }
  void setLive(int offset, const uint8_t* rgb, size_t n, uint32_t timeoutMs) {
    int t = total();
    if ((int)live.size() != t) live.assign(t, RGB());
    for (size_t k = 0; k < n; k++) {
      int idx = offset + (int)k;
      if (idx >= t) break;
      live[idx] = RGB(rgb[k * 3], rgb[k * 3 + 1], rgb[k * 3 + 2]);
    }
    liveUntil = now() + (timeoutMs ? timeoutMs : 2500);
    if (!liveUntil) liveUntil = 1;
  }

  // ------------------------------------------------ render
  void render() {
    uint32_t t = now();
    int n = total();
    if ((int)frame.size() != n) frame.assign(n, RGB());
    std::vector<RGB>& cur = work_;
    cur.assign(n, RGB());
    if (nx.until && (int32_t)(nx.until - t) <= 0) {
      startTransition(300);
      nx.state = 0;
      nx.until = 0;
    }
    if (liveActive()) {
      for (int i = 0; i < n && i < (int)live.size(); i++) cur[i] = live[i];
    } else if (nx.state != 0) {
      renderNexus(nx, cur.data(), n, t, rng);
    } else {
      for (auto& s : segs) {
        int L = s.len();
        if (!s.on || L <= 0) continue;
        int vl = s.mirror ? (L + 1) / 2 : L;
        segBuf_.assign(vl, RGB());
        FxCtx c{s, segBuf_.data(), vl, t, rng};
        if (s.lastFx != s.fx) {
          s.reset();
          s.lastFx = s.fx;
        }
        EFFECTS[s.fx < EFFECT_COUNT ? s.fx : 0].fn(c);
        for (int i = 0; i < L; i++) {
          int vi = i;
          if (s.mirror) vi = i < vl ? i : L - 1 - i;
          if (s.reverse) vi = (s.mirror ? vl : L) - 1 - vi;
          if (vi < 0) vi = 0;
          if (vi >= vl) vi = vl - 1;
          RGB c2 = segBuf_[vi];
          if (s.bri != 255) c2 = scale(c2, s.bri);
          cur[s.start + i] = c2;
        }
      }
    }
    // brillo global y apagado (el directo y los estados de NEXUS se ven aunque este apagado)
    uint8_t b = (on || liveActive() || nx.state != 0) ? bri : 0;
    if (nx.state != 0 && !on && b < 40) b = 40;
    for (auto& c : cur) c = scale(c, b);
    // transicion suave desde la imagen anterior
    if (transDur_ && (int)transFrom_.size() == n) {
      uint32_t el = t - transStart_;
      if (el >= transDur_) {
        transDur_ = 0;
      } else {
        uint8_t k = (uint8_t)((uint64_t)el * 255 / transDur_);
        for (int i = 0; i < n; i++) cur[i] = blend(transFrom_[i], cur[i], k);
      }
    }
    // limite de corriente estimado
    uint64_t sum = 0;
    for (auto& c : cur) sum += c.r + c.g + c.b;
    int mA = (int)(sum * ledmA / 765) + n;  // ~1 mA en reposo por LED
    if (maxmA > 0 && mA > maxmA) {
      uint8_t k = (uint8_t)((uint64_t)(maxmA - n > 0 ? maxmA - n : 0) * 255 / (mA - n));
      for (auto& c : cur) c = scale(c, k);
      mA = maxmA;
    }
    powermA = mA;
    frame.swap(cur);
  }

  // bytes para una salida (orden de color, gamma, RGBW e inversion aplicados)
  void outputBytes(int idx, std::vector<uint8_t>& out) const {
    out.clear();
    if (idx < 0 || idx >= (int)outputs.size()) return;
    int base = 0;
    for (int i = 0; i < idx; i++) base += outputs[i].count;
    const OutputCfg& o = outputs[idx];
    int ch = o.rgbw ? 4 : 3;
    out.resize((size_t)o.count * ch);
    for (int i = 0; i < o.count; i++) {
      int li = base + (o.reverse ? o.count - 1 - i : i);
      RGB c = li < (int)frame.size() ? frame[li] : RGB();
      uint8_t r = c.r, g = c.g, bl = c.b, wv = 0;
      if (gamma) { r = gamma8(r); g = gamma8(g); bl = gamma8(bl); }
      if (o.rgbw) {
        wv = r < g ? (r < bl ? r : bl) : (g < bl ? g : bl);
        r -= wv; g -= wv; bl -= wv;
      }
      uint8_t* p = &out[(size_t)i * ch];
      for (int k = 0; k < 3; k++) {
        char cc = o.order[k];
        p[k] = cc == 'R' ? r : cc == 'G' ? g : bl;
      }
      if (o.rgbw) p[3] = wv;
    }
  }

  // ------------------------------------------------ API HTTP
  bool authorized(const Request& r) const {
    if (token.empty()) return true;
    return r.token == token || r.q("token") == token;
  }
  static Response json(int code, const std::string& body) {
    Response r;
    r.code = code;
    r.body = body;
    return r;
  }
  static Response error(int code, const std::string& msg) {
    JWriter w;
    w.obj().kv("error", msg).end();
    return json(code, w.out);
  }

  Response handle(const Request& r) {
    const std::string& m = r.method;
    const std::string& p = r.path;
    if (m == "OPTIONS") return json(204, "");
    bool write = m != "GET";
    if (p == "/api/info" && m == "GET") return json(200, infoJson());
    if (p == "/api/effects" && m == "GET") return json(200, effectsJson());
    if (p == "/api/palettes" && m == "GET") return json(200, palettesJson());
    if (p == "/api/nexus" && m == "GET") return json(200, nexusStatesJson());
    if (p == "/api/frame" && m == "GET") return json(200, frameJson());
    if (p == "/api/state" && m == "GET") return json(200, stateJson(true));
    if (p == "/api/presets" && m == "GET") return json(200, presetsJson(true));
    if (write || p == "/api/config") {
      if (!authorized(r)) return error(401, "falta la clave del dispositivo (X-Nexus-Token)");
    }
    if (p == "/api/state" && m == "POST") {
      JVal v;
      if (!parseJson(r.body, v)) return error(400, "JSON no valido");
      std::string err;
      bool ok = applyState(v, err);
      if (!ok) {
        JWriter w;
        w.obj().kv("error", err).key("state").raw(stateJson(true)).end();
        return json(400, w.out);
      }
      return json(200, stateJson(true));
    }
    if (p == "/api/nexus" && m == "POST") {
      JVal v;
      if (!parseJson(r.body, v)) return error(400, "JSON no valido");
      std::string err;
      if (!applyNexus(v, err)) return error(400, err);
      return json(200, stateJson(true));
    }
    if (p == "/api/live" && m == "POST") {
      JVal v;
      if (!parseJson(r.body, v)) return error(400, "JSON no valido");
      std::vector<uint8_t> rgb;
      if (const JVal* l = v.get("leds")) {
        if (l->isStr()) {
          const std::string& h = l->s;
          for (size_t i = 0; i + 1 < h.size(); i += 2) {
            int a = hexv(h[i]), b = hexv(h[i + 1]);
            if (a < 0 || b < 0) return error(400, "hex no valido");
            rgb.push_back((uint8_t)(a * 16 + b));
          }
        } else if (l->isArr()) {
          for (auto& e : l->a) {
            RGB c;
            if (!parseColor(e, c)) return error(400, "color no valido");
            rgb.push_back(c.r); rgb.push_back(c.g); rgb.push_back(c.b);
          }
        }
      }
      int off = v.get("offset") ? (int)v.get("offset")->num(0) : 0;
      double to = v.get("timeout") ? v.get("timeout")->num(2.5) : 2.5;
      if (off < 0) off = 0;
      setLive(off, rgb.data(), rgb.size() / 3, (uint32_t)(to * 1000));
      return json(200, "{\"ok\":true}");
    }
    if (p == "/api/presets" && m == "POST") {
      JVal v;
      if (!parseJson(r.body.empty() ? "{}" : r.body, v)) return error(400, "JSON no valido");
      std::string err;
      int id = 0;
      if (!savePreset(v, err, id)) return error(400, err);
      JWriter w;
      w.obj().kvb("ok", true).kv("id", id).end();
      return json(200, w.out);
    }
    if (p == "/api/presets" && m == "DELETE") {
      int id = atoi(r.q("id", "0").c_str());
      if (!deletePreset(id)) return error(404, "preset inexistente");
      return json(200, "{\"ok\":true}");
    }
    if (p == "/api/config" && m == "GET") return json(200, configJson(false));
    if (p == "/api/config" && m == "POST") {
      JVal v;
      if (!parseJson(r.body, v)) return error(400, "JSON no valido");
      std::string err;
      bool ok = applyConfig(v, err);
      JWriter w;
      w.obj().kvb("ok", ok);
      if (!ok) w.kv("error", err);
      w.kvb("reboot", wifiChanged).key("config").raw(configJson(false)).end();
      return json(ok ? 200 : 400, w.out);
    }
    if (p == "/api/reboot" && m == "POST") {
      rebootRequested = true;
      return json(200, "{\"ok\":true}");
    }
    if (p == "/api/factory" && m == "POST") {
      factoryRequested = true;
      return json(200, "{\"ok\":true}");
    }
    return error(404, "ruta desconocida");
  }

  // ------------------------------------------------ UDP: descubrimiento y pixeles en directo
  // Devuelve la respuesta a enviar al remitente (vacia = ninguna).
  std::string handleUdp(const uint8_t* d, size_t n) {
    static const char Q[] = "NEXUSLED?";
    if (n >= 9 && memcmp(d, Q, 9) == 0) {
      JWriter w;
      w.obj().kv("nexusled", NEXUSLED_API).kv("name", name).kv("id", deviceId).kv("version", NEXUSLED_VERSION);
      w.kv("leds", total()).kvb("auth", !token.empty()).kv("http_port", httpPort).end();
      return w.out;
    }
    // trama: 'N' 'X' 'L' 1 | timeout (s, 0 = 2.5) | offset (2 bytes, big endian) | R G B ...
    if (udpLive && n >= 7 && d[0] == 'N' && d[1] == 'X' && d[2] == 'L' && d[3] == 1) {
      uint32_t to = d[4] ? d[4] * 1000u : 2500u;
      int off = (d[5] << 8) | d[6];
      setLive(off, d + 7, (n - 7) / 3, to);
    }
    // estado de NEXUS: 'N' 'X' 'L' 2 | estado | nivel (0-254, 255 = simulado) | R G B | ttl (s, 0 = sin caducidad)
    if (udpLive && n >= 10 && d[0] == 'N' && d[1] == 'X' && d[2] == 'L' && d[3] == 2 && d[4] < NEXUS_STATE_COUNT) {
      if (d[4] != nx.state) {
        startTransition(transition < 300 ? transition : 300);
        nx.since = now();
      }
      nx.state = d[4];
      nx.level = d[5] == 255 ? -1.0f : d[5] / 254.0f;
      nx.levelAt = now();
      nx.color = (d[6] || d[7] || d[8]) ? RGB(d[6], d[7], d[8]) : NEXUS_STATES[d[4]].color;
      nx.until = d[9] ? now() + d[9] * 1000u : 0;
      if (d[9] && !nx.until) nx.until = 1;
    }
    return "";
  }

  // ------------------------------------------------ puerto serie (USB): una linea JSON por orden
  // {"cmd":"info"|"state"|"config"|"effects"|"palettes"|"presets"|"reboot"} o {"config":{...}} o un cambio de estado.
  // El cable USB es acceso fisico: no pide la clave.
  std::string handleSerial(const std::string& line) {
    JVal v;
    if (!parseJson(line, v) || !v.isObj()) return "{\"error\":\"JSON no valido\"}";
    if (const JVal* c = v.get("cmd")) {
      std::string k = c->isStr() ? c->s : "";
      if (k == "info") return infoJson();
      if (k == "state") return stateJson(true);
      if (k == "config") return configJson(false);
      if (k == "effects") return effectsJson();
      if (k == "palettes") return palettesJson();
      if (k == "presets") return presetsJson(true);
      if (k == "reboot") { rebootRequested = true; return "{\"ok\":true}"; }
      return "{\"error\":\"orden desconocida\"}";
    }
    if (const JVal* c = v.get("config")) {
      std::string err;
      bool ok = applyConfig(*c, err);
      JWriter w;
      w.obj().kvb("ok", ok);
      if (!ok) w.kv("error", err);
      w.kvb("reboot", wifiChanged).end();
      return w.out;
    }
    std::string err;
    if (!applyState(v, err)) {
      JWriter w;
      w.obj().kv("error", err).end();
      return w.out;
    }
    return stateJson(true);
  }

 private:
  std::vector<RGB> transFrom_, work_, segBuf_;
  uint32_t transStart_ = 0, transDur_ = 0;
};

}  // namespace nl
