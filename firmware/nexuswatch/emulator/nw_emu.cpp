// Emulador de NexusWatch para el PC: dibuja la pantalla del reloj (240x240) con el mismo nucleo del firmware.
//
//   g++ -O2 -std=c++17 -o nw-emu nw_emu.cpp
//   ./nw-emu estado.json salida.ppm [ms] [gesto:x:y ...]      gestos: left right up down tap double long
//
// estado.json es la respuesta de GET /api/dispositivos/reloj de Nexus. Sirve para probar las pantallas sin reloj.
#include <fstream>
#include <iostream>
#include <sstream>

#include "../nw_core.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "uso: nw-emu estado.json salida.ppm [ms] [gesto:x:y ...]\n";
    return 2;
  }
  std::ifstream f(argv[1]);
  std::stringstream ss;
  ss << f.rdbuf();
  static nw::Watch w;
  uint32_t now = argc > 3 ? (uint32_t)atol(argv[3]) : 1000;
  if (!ss.str().empty() && ss.str() != "offline") {
    if (!w.load(ss.str(), now)) { std::cerr << "JSON no valido\n"; return 1; }
  } else w.offline("no responde");
  w.ssid = "MiWifi"; w.ip = "192.168.1.77"; w.host = "192.168.1.10:8775";
  for (int i = 4; i < argc; i++) {
    std::string g = argv[i];
    int x = 120, y = 120;
    size_t p = g.find(':');
    if (p != std::string::npos) {
      sscanf(g.c_str() + p + 1, "%d:%d", &x, &y);
      g = g.substr(0, p);
    }
    uint8_t code = g == "left" ? nw::G_LEFT : g == "right" ? nw::G_RIGHT : g == "up" ? nw::G_UP : g == "down" ? nw::G_DOWN
                 : g == "double" ? nw::G_DOUBLE : g == "long" ? nw::G_LONG : nw::G_TAP;
    w.gesture(code, x, y, now);
  }
  static nw::Canvas c;
  std::ofstream o(argv[2], std::ios::binary);
  o << "P6\n240 240\n255\n";
  for (int top = 0; top < NW_H; top += NW_STRIP) {
    c.begin(top);
    w.render(c, now);
    for (int y = 0; y < c.h; y++)
      for (int x = 0; x < NW_W; x++) {
        int dx = x - 120, dy = top + y - 120;
        uint16_t v = (dx * dx + dy * dy <= 120 * 120) ? c.buf[y * NW_W + x] : 0;  // pantalla redonda
        unsigned char rgb[3] = {(unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                (unsigned char)((v & 31) * 255 / 31)};
        o.write((char*)rgb, 3);
      }
  }
  if (!w.pendingMode.empty()) std::cout << "modo:" << w.pendingMode << "\n";
  return 0;
}
