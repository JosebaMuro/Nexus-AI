// Pruebas del nucleo de NexusLED en el PC: g++ -std=c++17 -fsanitize=address,undefined core_test.cpp
#include <assert.h>
#include <stdio.h>
#include "../nexusled/nl_core.h"

static uint32_t T = 1000;
static uint32_t clk() { return T; }

static nl::JVal J(const char* s) { nl::JVal v; bool ok = nl::parseJson(s, v); assert(ok); return v; }

int main() {
  // JSON
  nl::JVal v;
  assert(nl::parseJson("{\"a\":[1,2.5,\"x\\u00e9\\n\"],\"b\":{\"c\":true,\"d\":null}}", v));
  assert(v.get("a")->a[1].n == 2.5 && v.get("a")->a[2].s == "x\xc3\xa9\n" && v.get("b")->get("c")->b);
  assert(!nl::parseJson("{\"a\":}", v) && !nl::parseJson("[1,2", v) && !nl::parseJson("{} x", v));
  assert(nl::Device::writeVal(J("{\"k\":[1,\"q\\\"\",false,null,{}]}")) == "{\"k\":[1,\"q\\\"\",false,null,{}]}");
  std::string deep(40, '['); assert(!nl::parseJson(deep, v));

  // todos los efectos con varias longitudes, espejo e inversion
  for (int L : {1, 2, 7, 60, 300}) {
    nl::Device d; d.clock = clk; d.outputs[0].count = L; d.segs.clear(); d.ensureSegments();
    for (int fx = 0; fx < nl::EFFECT_COUNT; fx++)
      for (int mode = 0; mode < 4; mode++) {
        std::string err;
        char b[200];
        snprintf(b, sizeof b, "{\"fx\":%d,\"pal\":%d,\"sx\":%d,\"ix\":%d,\"seg\":[{\"id\":0,\"rev\":%s,\"mi\":%s}]}", fx, fx % nl::PALETTE_COUNT,
                 (fx * 37) % 256, (fx * 91) % 256, mode & 1 ? "true" : "false", mode & 2 ? "true" : "false");
        assert(d.applyState(J(b), err));
        for (int k = 0; k < 120; k++) { T += 17; d.render(); }
        std::vector<uint8_t> out; d.outputBytes(0, out); assert((int)out.size() == L * 3);
      }
    for (int st = 0; st < nl::NEXUS_STATE_COUNT; st++) {
      std::string err; char b[120];
      snprintf(b, sizeof b, "{\"nexus\":{\"status\":\"%s\",\"level\":0.7}}", nl::NEXUS_STATES[st].key);
      assert(d.applyState(J(b), err));
      for (int k = 0; k < 200; k++) { T += 17; d.render(); }
    }
  }
  // estado, colores y nombres
  nl::Device d; d.clock = clk; d.outputs[0].count = 10; d.segs.clear(); d.ensureSegments();
  std::string err;
  assert(d.applyState(J("{\"on\":true,\"bri\":255,\"fx\":\"solid\",\"col\":[\"#ff0000\"],\"transition\":0}"), err));
  T += 50; d.render();
  assert(d.frame[0] == nl::RGB(255, 0, 0));
  assert(d.applyState(J("{\"color\":[0,0,255],\"tt\":0}"), err)); T += 5; d.render(); assert(d.frame[3] == nl::RGB(0, 0, 255));
  assert(!d.applyState(J("{\"fx\":\"noexiste\"}"), err) && err == "efecto desconocido");
  assert(d.applyState(J("{\"fx\":\"Arcoíris\"}"), err) && d.segs[0].fx == 4);
  // orden de color GRB y gamma
  d.gamma = false; d.applyState(J("{\"fx\":0,\"col\":[[10,20,30]],\"tt\":0}"), err); T += 5; d.render();
  std::vector<uint8_t> out; d.outputBytes(0, out); assert(out[0] == 20 && out[1] == 10 && out[2] == 30);
  // segmentos
  assert(d.applyState(J("{\"seg\":[{\"id\":0,\"stop\":5},{\"id\":1,\"start\":5,\"stop\":10,\"fx\":\"solid\",\"col\":[\"#00ff00\"]}],\"tt\":0}"), err));
  T += 5; d.render(); assert(d.segs.size() == 2 && d.frame[7] == nl::RGB(0, 255, 0));
  assert(d.applyState(J("{\"seg\":[{\"id\":1,\"stop\":0}]}"), err) && d.segs.size() == 1);
  // apagado y estado de NEXUS por encima
  d.applyState(J("{\"on\":false,\"tt\":0}"), err); T += 5; d.render(); assert(d.frame[0] == nl::RGB());
  d.applyState(J("{\"nexus\":{\"status\":\"error\",\"ttl\":1}}"), err); T += 400; d.render(); assert(d.frame[0] != nl::RGB());
  T += 1000; d.render(); T += 400; d.render(); assert(d.nx.state == 0 && d.frame[0] == nl::RGB());
  // presets
  d.applyState(J("{\"on\":true,\"fx\":\"fire\",\"tt\":0}"), err);
  int id = 0; assert(d.savePreset(J("{\"name\":\"Fuego\"}"), err, id) && id == 1);
  d.applyState(J("{\"fx\":\"solid\"}"), err); assert(d.segs[0].fx == 0);
  assert(d.applyState(J("{\"preset\":1}"), err) && d.segs[0].fx == 11 && d.preset == 1);
  nl::Device d2; d2.clock = clk; d2.loadPresets(d.presetsJson()); assert(d2.presets.size() == 1 && d2.presets[0].name == "Fuego");
  // persistencia de configuracion con secretos
  d.applyState(J("{\"seg\":[{\"id\":0,\"start\":0,\"stop\":10}]}"), err);
  d.applyConfig(J("{\"token\":\"abc\",\"wifi\":{\"ssid\":\"casa\",\"pass\":\"12345678\"},\"outputs\":[{\"pin\":16,\"count\":10},{\"pin\":17,\"count\":5,\"order\":\"rgb\",\"reverse\":true}]}"), err);
  assert(d.total() == 15 && d.segs[0].stop == 15);
  nl::Device d3; d3.clock = clk; d3.loadConfig(d.configJson(true)); assert(d3.token == "abc" && d3.pass == "12345678" && d3.total() == 15 && d3.outputs[1].order == "RGB");
  assert(d.configJson(false).find("12345678") == std::string::npos);
  // API y clave
  nl::Request r; r.method = "POST"; r.path = "/api/state"; r.body = "{\"bri\":10}";
  assert(d.handle(r).code == 401); r.token = "abc"; assert(d.handle(r).code == 200 && d.bri == 10);
  r.method = "GET"; r.path = "/api/config"; r.token = ""; assert(d.handle(r).code == 401);
  r.method = "GET"; r.path = "/api/info"; assert(d.handle(r).code == 200);
  // UDP: descubrimiento y tramas en directo
  std::string rep = d.handleUdp((const uint8_t*)"NEXUSLED?", 9); assert(rep.find("\"nexusled\":1") != std::string::npos);
  uint8_t pkt[7 + 6] = {'N', 'X', 'L', 1, 1, 0, 2, 255, 0, 0, 0, 255, 0};
  d.handleUdp(pkt, sizeof pkt); d.bri = 255; T += 5; d.render(); assert(d.liveActive() && d.frame[2] == nl::RGB(255, 0, 0) && d.frame[3] == nl::RGB(0, 255, 0));
  T += 1500; d.render(); assert(!d.liveActive());
  uint8_t big[7 + 3 * 40] = {'N', 'X', 'L', 1, 0, 0, 10}; d.handleUdp(big, sizeof big);  // se recorta sin salirse
  // estado de NEXUS por UDP
  uint8_t st[10] = {'N', 'X', 'L', 2, 5, 127, 0, 0, 0, 3};
  d.handleUdp(st, sizeof st); assert(d.nx.state == 5 && d.nx.level > 0.49f && d.nx.level < 0.51f && d.nx.color == nl::NEXUS_STATES[5].color);
  uint8_t bad[10] = {'N', 'X', 'L', 2, 99, 0, 0, 0, 0, 0}; d.handleUdp(bad, sizeof bad); assert(d.nx.state == 5);
  // serie
  assert(d.handleSerial("{\"cmd\":\"info\"}").find("nexusled") != std::string::npos);
  assert(d.handleSerial("{\"bri\":99}").find("\"bri\":99") != std::string::npos);
  // limite de corriente
  d.applyConfig(J("{\"max_ma\":200}"), err); d.applyState(J("{\"fx\":0,\"col\":[\"#ffffff\"],\"bri\":255,\"seg\":[{\"id\":0,\"bri\":255}],\"tt\":0}"), err);
  T += 5; d.render(); assert(d.powermA <= 200);
  puts("core_test OK");
  return 0;
}
