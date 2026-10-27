// NexusWatch - nucleo portable del reloj: estado que llega de Nexus, pantallas, gestos y dibujo.
// El hardware (pantalla, tactil, Wi-Fi) esta en nexuswatch.ino; el emulador del PC usa este mismo nucleo.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "nl_json.h"
#include "nw_gfx.h"

#define NEXUSWATCH_VERSION "1.0.0"

namespace nw {

enum Gesture : uint8_t { G_NONE = 0, G_UP = 1, G_DOWN = 2, G_LEFT = 3, G_RIGHT = 4, G_TAP = 5, G_DOUBLE = 11, G_LONG = 12 };
enum Screen : uint8_t { S_AVATAR = 0, S_AGENTS, S_MODES, S_INFO, S_COUNT, S_AGENT = 100 };

struct Agent {
  std::string id, name, state, scene, task, icon;
  uint16_t color = 0x07FF;
  int secs = 0, tasks = 0;
};
struct Mode {
  std::string id, name, icon;
};

struct Watch {
  // ------------------------------------------------ datos que manda Nexus
  std::string state = "idle", stateName = "En línea", mode, clock = "--:--", text;
  uint16_t stateColor = 0x07FF;
  float level = 0;
  std::vector<Agent> agents;
  std::vector<Mode> modes;
  bool online = false;
  uint32_t lastData = 0;
  // ------------------------------------------------ conexion (lo rellena el .ino)
  std::string ssid, ip, host, error;
  // ------------------------------------------------ interfaz
  uint8_t screen = S_AVATAR;
  int scroll = 0, sel = 0, agentIdx = 0;
  std::string pendingMode;      // modo que el .ino tiene que enviar a Nexus
  std::string toast;
  uint32_t toastUntil = 0, stateSince = 0, lastTouch = 0;

  static uint16_t colorFor(const std::string& st) {
    if (st == "listening") return rgb(124, 255, 107);
    if (st == "thinking") return rgb(166, 107, 255);
    if (st == "tool") return rgb(255, 176, 32);
    if (st == "speaking") return rgb(255, 61, 242);
    if (st == "error") return rgb(255, 20, 20);
    if (st == "success") return rgb(40, 255, 90);
    return rgb(0, 240, 255);
  }

  // JSON de GET /api/dispositivos/reloj
  bool load(const std::string& body, uint32_t now) {
    nl::JVal v;
    if (!nl::parseJson(body, v) || !v.isObj()) return false;
    auto str = [&](const nl::JVal* x, const char* def = "") { return x && x->isStr() ? x->s : std::string(def); };
    std::string st = str(v.get("estado"), "idle");
    if (st != state) stateSince = now;
    state = st;
    stateName = str(v.get("nombre"), "En línea");
    stateColor = hex(str(v.get("color"), "#00f0ff").c_str(), colorFor(state));
    level = v.get("nivel") ? (float)v.get("nivel")->num(0) : 0;
    mode = str(v.get("modo"));
    clock = str(v.get("hora"), "--:--");
    text = str(v.get("texto"));
    if (const nl::JVal* a = v.get("agentes")) {
      if (a->isArr()) {
        agents.clear();
        for (auto& e : a->a) {
          Agent g;
          g.id = str(e.get("id"));
          g.name = str(e.get("nombre"));
          g.state = str(e.get("estado"), "idle");
          g.scene = str(e.get("escena"), "idle");
          g.task = str(e.get("tarea"));
          g.icon = str(e.get("icono"));
          g.color = hex(str(e.get("color"), "#00f0ff").c_str());
          g.secs = e.get("segundos") ? (int)e.get("segundos")->num(0) : 0;
          g.tasks = e.get("tareas") ? (int)e.get("tareas")->num(0) : 0;
          agents.push_back(g);
        }
      }
    }
    if (const nl::JVal* m = v.get("modos")) {
      if (m->isArr()) {
        modes.clear();
        for (auto& e : m->a) modes.push_back({str(e.get("id")), str(e.get("nombre")), str(e.get("icono"))});
      }
    }
    online = true;
    lastData = now;
    error.clear();
    return true;
  }
  void offline(const std::string& why) {
    online = false;
    error = why;
  }

  static const char* activity(const Agent& a) {
    const std::string& s = a.scene;
    if (a.state == "idle") return a.secs > 300 ? "Tomando un café" : "Libre";
    if (a.state == "meeting") return "En reunión";
    if (a.state == "thinking") return "Pensando";
    if (a.state == "writing") return "Escribiendo";
    if (a.state == "delegating") return "Delegando";
    if (s == "browsing") return "Buscando en la web";
    if (s == "reading") return "Leyendo documentos";
    if (s == "drawing") return "Dibujando";
    if (s == "music") return "Componiendo";
    if (s == "calculating") return "Calculando";
    if (s == "coding") return "Programando";
    if (s == "phone") return "Consultando al profesor";
    return "Trabajando";
  }

  void say(const std::string& t, uint32_t now, uint32_t ms = 1800) {
    toast = t;
    toastUntil = now + ms;
  }

  // ------------------------------------------------ gestos
  void gesture(uint8_t g, int x, int y, uint32_t now) {
    (void)x;
    lastTouch = now;
    if (screen == S_AGENT) {
      if (g == G_RIGHT || g == G_TAP || g == G_DOWN) screen = S_AGENTS;
      return;
    }
    if (g == G_LEFT) { screen = (screen + 1) % S_COUNT; scroll = 0; return; }
    if (g == G_RIGHT) { screen = (screen + S_COUNT - 1) % S_COUNT; scroll = 0; return; }
    int n = screen == S_AGENTS ? (int)agents.size() : screen == S_MODES ? (int)modes.size() : 0;
    if (g == G_UP && scroll + 3 < n) scroll++;
    if (g == G_DOWN && scroll > 0) scroll--;
    if (g == G_TAP && (screen == S_AGENTS || screen == S_MODES)) {
      int row = rowAt(y);
      int i = scroll + row;
      if (row >= 0 && i < n) {
        if (screen == S_AGENTS) { agentIdx = i; screen = S_AGENT; }
        else {
          pendingMode = modes[i].id;
          say("Modo " + modes[i].name, now);
        }
      }
    }
    if (g == G_DOUBLE && screen == S_AVATAR) say(stateName, now);
  }
  static int rowAt(int y) {  // filas de las listas: y = 62 + 44*i
    if (y < 58 || y > 58 + 44 * 3) return -1;
    return (y - 58) / 44;
  }

  // ------------------------------------------------ dibujo de una franja
  void render(Canvas& c, uint32_t now) {
    c.clear(BG);
    switch (screen) {
      case S_AVATAR: drawAvatar(c, now); break;
      case S_AGENTS: drawAgents(c, now); break;
      case S_AGENT: drawAgent(c, now); break;
      case S_MODES: drawModes(c, now); break;
      default: drawInfo(c, now); break;
    }
    if (screen != S_AGENT) drawDots(c);
    if (now < toastUntil && !toast.empty()) {
      int w = Canvas::textW(toast.c_str()) + 24;
      if (w > 200) w = 200;
      c.fillRect(120 - w / 2, 196, w, 24, dim(stateColor, 70));
      c.text(120, 200, toast.c_str(), WHITE, 1, 1, 190);
    }
  }

  void drawDots(Canvas& c) {
    for (int i = 0; i < S_COUNT; i++) c.fillCircle(102 + i * 12, 228, i == screen ? 3.2f : 2.2f, i == screen ? WHITE : MUTED);
  }

  void drawAvatar(Canvas& c, uint32_t now) {
    float t = now / 1000.0f;
    uint16_t col = online ? stateColor : MUTED;
    c.vgradient(dim(col, 28), BG);
    const float cx = 120, cy = 112;
    // partículas de memoria orbitando
    for (int i = 0; i < 14; i++) {
      float a = t * (0.25f + (i % 3) * 0.12f) + i * 0.45f;
      float r = 98 + 10 * sinf(i * 1.7f + t * 0.6f);
      c.fillCircle(cx + r * sinf(a), cy - r * cosf(a) * 0.92f, 1.6f + (i % 2), mix(col, WHITE, 90), 170);
    }
    // anillo de estado animado
    if (!online) {
      c.ring(cx, cy, 82, 85, MUTED);
    } else if (state == "listening") {
      for (int k = 0; k < 3; k++) {
        float ph = fmodf(t * 0.8f + k / 3.0f, 1.0f);
        c.ring(cx, cy, 80 + ph * 26, 83 + ph * 26, col, 0, 6.2832f, (uint8_t)(220 * (1 - ph)));
      }
      c.ring(cx, cy, 80, 84, col);
    } else if (state == "thinking") {
      c.ring(cx, cy, 81, 83, dim(col, 90));
      for (int k = 0; k < 2; k++) {
        float a = t * 2.4f + k * 3.1416f;
        for (int j = 0; j < 10; j++) c.ring(cx, cy, 79, 85, col, a - j * 0.09f, a - j * 0.09f + 0.09f, (uint8_t)(255 - j * 24));
      }
    } else if (state == "tool") {
      for (int k = 0; k < 8; k++) {
        float a = t * 1.2f + k * 0.785f;
        c.ring(cx, cy, 80, 86, col, a, a + 0.45f);
      }
    } else if (state == "speaking") {
      float lv = level > 0.02f ? level : 0.35f + 0.3f * sinf(t * 9) * sinf(t * 3.3f);
      c.ring(cx, cy, 80, 82 + lv * 12, col);
      for (int k = 0; k < 36; k++) {
        float a = k * 0.1745f, amp = 4 + 10 * lv * (0.5f + 0.5f * sinf(t * 11 + k * 1.3f));
        c.line(cx + 88 * sinf(a), cy - 88 * cosf(a), cx + (88 + amp) * sinf(a), cy - (88 + amp) * cosf(a), 2.2f, col, 200);
      }
    } else {
      float br = 0.5f + 0.5f * sinf(t * 1.6f);
      c.ring(cx, cy, 80, 83 + br * 2, col, 0, 6.2832f, (uint8_t)(150 + br * 105));
    }
    c.ring(cx, cy, 76, 80, dim(col, 60));
    // cara del avatar (respira un poco)
    int dy = (int)(sinf(t * 1.2f) * 1.5f);
    c.avatar((int)cx - NW_AVATAR / 2, (int)cy - NW_AVATAR / 2 + dy, online ? 255 : 120);
    // parpadeo: dos sombras sobre los ojos cada pocos segundos
    float bl = fmodf(t, 4.7f);
    if (bl < 0.14f && online) {
      c.fillRect((int)cx - 34, (int)cy - 18 + dy, 24, 8, dim(rgb(60, 50, 50), 255));
      c.fillRect((int)cx + 10, (int)cy - 18 + dy, 24, 8, dim(rgb(60, 50, 50), 255));
    }
    c.text(120, 14, clock.c_str(), WHITE, 1, 1);
    c.text(120, 196, online ? stateName.c_str() : "SIN CONEXIÓN", online ? col : MUTED, 1, 1, 170);
    if (!online && !error.empty()) c.text(120, 212, error.c_str(), MUTED, 1, 1, 150);
  }

  void listHeader(Canvas& c, const char* title, const char* sub) {
    c.text(120, 18, title, stateColor, 1, 1);
    c.text(120, 36, sub, MUTED, 1, 1, 170);
  }

  void drawAgents(Canvas& c, uint32_t now) {
    char sub[48];
    int busy = 0;
    for (auto& a : agents) busy += a.state != "idle";
    snprintf(sub, sizeof sub, "%d trabajando de %d", busy, (int)agents.size());
    listHeader(c, "AGENTES", sub);
    if (agents.empty()) { c.text(120, 110, online ? "Sin agentes" : "Sin conexión", MUTED, 1, 1); return; }
    float t = now / 1000.0f;
    for (int r = 0; r < 3 && scroll + r < (int)agents.size(); r++) {
      const Agent& a = agents[scroll + r];
      int y = 62 + r * 44;
      int inset = r == 1 ? 26 : 38;   // la pantalla es redonda
      c.fillRect(inset, y, 240 - 2 * inset, 40, dim(a.color, 30));
      bool working = a.state != "idle";
      float p = working ? 0.5f + 0.5f * sinf(t * 5 + r) : 1;
      c.fillCircle(inset + 14, y + 20, 6, working ? mix(a.color, WHITE, (uint8_t)(p * 120)) : MUTED);
      c.text(inset + 28, y + 3, a.name.c_str(), WHITE, 1, 0, 240 - 2 * inset - 36);
      c.text(inset + 28, y + 21, activity(a), working ? a.color : MUTED, 1, 0, 240 - 2 * inset - 36);
    }
    if (scroll + 3 < (int)agents.size()) c.text(120, 196, "desliza hacia arriba", MUTED, 1, 1);
  }

  // dibujo pequeño de la escena del agente
  void sceneIcon(Canvas& c, const Agent& a, float cx, float cy, float t) {
    uint16_t col = a.color;
    const std::string& s = a.state == "tool" ? a.scene : a.state;
    if (s == "idle") {  // taza de café con vapor
      c.fillRect((int)cx - 18, (int)cy - 4, 32, 26, mix(col, WHITE, 60));
      c.ring(cx + 18, cy + 8, 5, 8, mix(col, WHITE, 60));
      for (int k = 0; k < 3; k++) {
        float yy = cy - 10 - fmodf(t * 14 + k * 8, 24);
        c.fillCircle(cx - 8 + k * 8 + 3 * sinf(t * 3 + k), yy, 2.5f, MUTED, 160);
      }
    } else if (s == "meeting") {
      for (int k = 0; k < 4; k++) {
        float a2 = k * 1.5708f + t * 0.5f;
        c.fillCircle(cx + 22 * sinf(a2), cy - 22 * cosf(a2), 7, mix(col, WHITE, (uint8_t)(k * 40)));
      }
      c.fillCircle(cx, cy, 9, dim(col, 120));
    } else if (s == "thinking" || s == "writing") {
      for (int k = 0; k < 3; k++) c.fillCircle(cx - 16 + k * 16, cy + 4 * sinf(t * 6 + k), 5, col);
    } else if (s == "browsing") {
      c.ring(cx, cy, 20, 23, col);
      c.ring(cx, cy, 2, 23, col, t, t + 0.35f, 120);
      c.line(cx - 22, cy, cx + 22, cy, 2, col);
      c.ring(cx, cy, 9, 11, dim(col, 160));
    } else if (s == "drawing") {
      c.line(cx - 18, cy + 18, cx + 10 + 6 * sinf(t * 4), cy - 14, 4, col);
      c.fillCircle(cx + 10 + 6 * sinf(t * 4), cy - 14, 4, WHITE);
    } else if (s == "reading") {
      c.fillRect((int)cx - 24, (int)cy - 14, 22, 30, mix(col, WHITE, 80));
      c.fillRect((int)cx + 2, (int)cy - 14, 22, 30, mix(col, WHITE, 120));
      int ly = (int)fmodf(t * 10, 24);
      c.fillRect((int)cx + 6, (int)cy - 10 + ly, 14, 2, BG);
    } else {  // engranaje que gira (trabajando, programando, calculando...)
      for (int k = 0; k < 8; k++) c.ring(cx, cy, 14, 24, col, t * 2 + k * 0.785f, t * 2 + k * 0.785f + 0.35f);
      c.ring(cx, cy, 8, 16, col);
    }
  }

  void drawAgent(Canvas& c, uint32_t now) {
    if (agentIdx >= (int)agents.size()) { screen = S_AGENTS; return; }
    const Agent& a = agents[agentIdx];
    float t = now / 1000.0f;
    c.vgradient(dim(a.color, 40), BG);
    c.text(120, 22, a.name.c_str(), WHITE, 1, 1, 180);
    c.text(120, 42, activity(a), a.color, 1, 1, 190);
    sceneIcon(c, a, 120, 98, t);
    // tarea en dos lineas
    std::string task = a.task.empty() ? (a.state == "idle" ? "Esperando trabajo" : "") : a.task;
    size_t cut = std::string::npos;
    if (Canvas::textW(task.c_str()) > 196) {   // parte en dos lineas por la ultima palabra que quepa
      for (size_t i = task.find(' '); i != std::string::npos; i = task.find(' ', i + 1)) {
        if (Canvas::textW(task.substr(0, i).c_str()) > 196) break;
        cut = i;
      }
    }
    if (cut == std::string::npos) c.text(120, 140, task.c_str(), WHITE, 1, 1, 196);
    else {
      c.text(120, 136, task.substr(0, cut).c_str(), WHITE, 1, 1, 196);
      c.text(120, 154, task.substr(cut + 1).c_str(), WHITE, 1, 1, 176);
    }
    char line[48];
    int m = a.secs / 60;
    if (m > 0) snprintf(line, sizeof line, "%d min · %d tareas hoy", m, a.tasks);
    else snprintf(line, sizeof line, "%d s · %d tareas", a.secs, a.tasks);
    c.text(120, 180, line, MUTED, 1, 1);
    c.text(120, 206, "toca para volver", MUTED, 1, 1);
  }

  void drawModes(Canvas& c, uint32_t now) {
    std::string cur = "activo: ";
    for (auto& m : modes)
      if (m.id == mode) cur += m.name;
    listHeader(c, "MODOS", mode.empty() ? "toca uno para cambiar" : cur.c_str());
    if (modes.empty()) { c.text(120, 110, online ? "Sin modos" : "Sin conexión", MUTED, 1, 1); return; }
    for (int r = 0; r < 3 && scroll + r < (int)modes.size(); r++) {
      const Mode& m = modes[scroll + r];
      int y = 62 + r * 44, inset = r == 1 ? 26 : 38;
      bool on = m.id == mode;
      c.fillRect(inset, y, 240 - 2 * inset, 40, on ? dim(stateColor, 90) : dim(MUTED, 50));
      c.text(120, y + 12, m.name.c_str(), on ? WHITE : rgb(200, 220, 230), 1, 1, 240 - 2 * inset - 10);
    }
    (void)now;
  }

  void drawInfo(Canvas& c, uint32_t now) {
    listHeader(c, "NEXUSWATCH", "v" NEXUSWATCH_VERSION);
    char l[64];
    c.text(120, 66, online ? "Conectado a Nexus" : "Sin conexión", online ? stateColor : rgb(255, 80, 80), 1, 1);
    snprintf(l, sizeof l, "Wi-Fi: %s", ssid.empty() ? "-" : ssid.c_str());
    c.text(120, 92, l, WHITE, 1, 1, 200);
    snprintf(l, sizeof l, "IP: %s", ip.empty() ? "-" : ip.c_str());
    c.text(120, 112, l, WHITE, 1, 1, 200);
    snprintf(l, sizeof l, "Nexus: %s", host.empty() ? "sin configurar" : host.c_str());
    c.text(120, 132, l, WHITE, 1, 1, 200);
    if (!error.empty()) c.text(120, 156, error.c_str(), rgb(255, 120, 120), 1, 1, 190);
    c.text(120, 184, "Mantén pulsado: Wi-Fi", MUTED, 1, 1);
    (void)now;
  }
};

}  // namespace nw
