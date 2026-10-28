// Pruebas de la logica de NexusPico en el PC (sin Pico ni pantalla):
//   g++ -std=c++17 -O1 -fsanitize=address,undefined -o np_test firmware/nexuspico/tests/np_test.cpp && ./np_test [salida.pgm]
#include <cassert>
#include <cstdio>
#include <map>

#include "../np_core.h"

static int fallos = 0;
#define OK(c)                                                       \
  do {                                                              \
    if (!(c)) {                                                     \
      std::fprintf(stderr, "FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fallos++;                                                     \
    }                                                               \
  } while (0)

struct RamSectores : np::Sectores {
  std::vector<std::vector<uint8_t>> s;
  int escrituras = 0;
  explicit RamSectores(int n) : s(n, std::vector<uint8_t>(4096, 0xFF)) {}
  int cuantos() const override { return (int)s.size(); }
  const uint8_t* leer(int i) override { return s[i].data(); }
  bool escribir(int i, const uint8_t* d) override {
    escrituras++;
    s[i].assign(d, d + 4096);
    return true;
  }
};

struct NexusFalso {
  std::string estado = "idle", version = "v1";
  bool caido = false;
  int pedidas_bin = 0;
  std::map<std::string, std::string> imgs;
  NexusFalso() {
    for (auto e : {"idle", "listening", "thinking", "tool", "speaking", "success", "error", "alert"})
      imgs[e] = std::string(np::BYTES, (char)(imgs.size() + 1));
  }
  int get(const std::string& ruta, std::string& cuerpo) {
    if (caido) return -1;
    if (ruta == "/api/dispositivos/pico/manifiesto") {
      cuerpo = "{\"version\":\"" + version + "\",\"ancho\":250,\"alto\":122,\"estados\":[";
      bool primero = true;
      for (auto& kv : imgs) cuerpo += (primero ? "\"" : ",\"") + kv.first + "\"", primero = false;
      cuerpo += "]}";
      return 200;
    }
    if (ruta.rfind("/api/dispositivos/pico/", 0) == 0) {
      std::string e = ruta.substr(23, ruta.size() - 23 - 4);
      pedidas_bin++;
      if (!imgs.count(e)) return 404;
      cuerpo = imgs[e];
      return 200;
    }
    if (ruta.rfind("/api/dispositivos/estado?", 0) == 0) {
      OK(ruta.find("tipo=pico") != std::string::npos);
      cuerpo = "{\"estado\":\"" + estado + "\",\"pico_version\":\"" + version + "\",\"seq\":3}";
      return 200;
    }
    return 404;
  }
};

int main(int argc, char** argv) {
  // ---- configuracion
  np::Config c;
  OK(!c.valida());
  OK(np::mezclar_config(R"({"wifi":{"ssid":"Casa","pass":"x y"},"nexus":{"host":"192.168.1.10:8775","key":"K"},"cada_s":3,"pines":{"busy":14}})", c));
  OK(c.valida() && c.ssid == "Casa" && c.pass == "x y" && c.key == "K" && c.cada_s == 3 && c.pines.busy == 14 && c.pines.rst == 12);
  np::Config c2;
  OK(np::mezclar_config(np::config_a_json(c), c2));
  OK(c2.ssid == c.ssid && c2.host == c.host && c2.pines.busy == 14 && c2.cada_s == 3);
  OK(!np::mezclar_config("no es json", c2));

  std::string h;
  int p;
  OK(np::partir_host("192.168.1.10:8775", h, p) && h == "192.168.1.10" && p == 8775);
  OK(np::partir_host("http://nexus.local/", h, p) && h == "nexus.local" && p == 8775);
  std::string cuerpo;
  OK(np::respuesta_http("HTTP/1.0 200 OK\r\nA: b\r\n\r\nhola", cuerpo) == 200 && cuerpo == "hola");
  OK(np::respuesta_http("basura", cuerpo) < 0);

  // ---- almacen en flash
  RamSectores ram(16);
  {
    np::Almacen a(ram);
    std::string v;
    OK(!a.leer("x", v));
    OK(a.escribir("x", "uno") && a.leer("x", v) && v == "uno");
    OK(a.escribir("x", "dos") && a.leer("x", v) && v == "dos");
    OK(!a.escribir("grande", std::string(5000, 'a')));
  }
  {
    np::Almacen a(ram);   // tras "reiniciar", el indice sigue ahi
    std::string v;
    OK(a.leer("x", v) && v == "dos");
    ram.s[1][13] ^= 0xFF;   // corrupcion: la suma lo detecta
    OK(!a.leer("x", v));
    for (int i = 0; i < 14; i++) OK(a.escribir("n" + std::to_string(i), "z"));
    OK(!a.escribir("lleno", "z"));   // 16 sectores: indice + 15
  }

  // ---- giro: el pixel (x, y) apaisado acaba en (y, 249 - x) vertical
  {
    std::vector<uint8_t> apa(((np::APA_W + 7) / 8) * np::APA_H, 0xFF);
    int x = 3, y = 7;
    apa[y * 32 + (x >> 3)] &= ~(0x80 >> (x & 7));
    auto v = np::girar(apa);
    OK((int)v.size() == np::BYTES);
    int xv = y, yv = np::APA_W - 1 - x;
    OK(!(v[yv * 16 + (xv >> 3)] & (0x80 >> (xv & 7))));
    int negros = 0;
    for (uint8_t b : v)
      for (int i = 0; i < 8; i++) negros += !(b & (1 << i));
    OK(negros == 1);
  }

  // ---- texto
  auto txt = np::pantalla_texto({"NexusPico 2.0.0", "", "Configúrame:", "Wi-Fi NexusPico-AB12", "clave: nexuspico", "http://192.168.4.1"});
  OK((int)txt.size() == np::BYTES);
  int negros = 0;
  for (uint8_t b : txt)
    for (int i = 0; i < 8; i++) negros += !(b & (1 << i));
  OK(negros > 1500 && negros < 12000);
  if (argc > 1) {   // imagen para mirarla: PGM 122x250
    FILE* f = std::fopen(argv[1], "wb");
    std::fprintf(f, "P5 %d %d 255\n", np::ANCHO, np::ALTO);
    for (int y = 0; y < np::ALTO; y++)
      for (int x = 0; x < np::ANCHO; x++) std::fputc(txt[y * 16 + (x >> 3)] & (0x80 >> (x & 7)) ? 255 : 0, f);
    std::fclose(f);
  }

  // ---- nucleo contra un Nexus falso
  RamSectores flash(16);
  np::Almacen alm(flash);
  NexusFalso nx;
  std::vector<std::string> pintado;
  std::vector<uint8_t> ultimo;
  np::Nucleo n([&](const std::string& r, std::string& b) { return nx.get(r, b); },
               [&](const std::vector<uint8_t>& b, const std::string& m) { pintado.push_back(m), ultimo = b; }, alm, "pico-abc123");
  OK(n.paso() == "idle");
  OK(nx.pedidas_bin == 8 && n.version() == "v1");
  OK(pintado.size() == 1 && ultimo == std::vector<uint8_t>(nx.imgs["idle"].begin(), nx.imgs["idle"].end()));
  OK(n.paso() == "idle" && pintado.size() == 1);   // sin cambios no repinta
  nx.estado = "thinking";
  OK(n.paso() == "thinking" && pintado.back() == "thinking");
  nx.estado = "desconocido";
  OK(n.paso() == "desconocido" && ultimo[0] == nx.imgs["idle"][0]);   // estado sin imagen: la de idle
  nx.estado = "none";
  OK(n.paso() == "idle");
  // Nexus cambia de nombre: nueva version y vuelve a bajar todo
  nx.version = "v2";
  nx.imgs["idle"] = std::string(np::BYTES, 'Z');
  OK(n.paso() == "idle" && nx.pedidas_bin == 16 && ultimo[0] == 'Z');
  // sin conexion: al tercer fallo, pantalla de aviso
  nx.caido = true;
  size_t antes = pintado.size();
  n.paso(), n.paso();
  OK(pintado.size() == antes);
  n.paso();
  OK(pintado.size() == antes + 1 && pintado.back() == "sin conexion");
  n.paso();
  OK(pintado.size() == antes + 1);
  nx.caido = false;
  nx.estado = "speaking";
  OK(n.paso() == "speaking");
  // tras un reinicio, las imagenes y la version siguen en la flash: no se descargan otra vez
  {
    np::Almacen alm2(flash);
    np::Nucleo n2([&](const std::string& r, std::string& b) { return nx.get(r, b); },
                  [&](const std::vector<uint8_t>&, const std::string&) {}, alm2);
    int pedidas = nx.pedidas_bin;
    OK(n2.paso() == "speaking" && nx.pedidas_bin == pedidas);
  }
  // imagen con tamano equivocado: no se guarda nada a medias como valido
  nx.version = "v3";
  nx.imgs["alert"] = "corta";
  std::string err;
  OK(!n.sincronizar(err) && err.find("alert") != std::string::npos && n.version() == "v2");

  // ---- consola: linea JSON (nexus-flash y Nexus-Office la mandan asi)
  np::Config cfg;
  std::string salida;
  np::Config guardada;
  np::Consola con;
  con.cfg = &cfg;
  con.responder = [&](const std::string& s) { salida += s; };
  con.guardar = [&](const np::Config& g) { guardada = g; return true; };
  con.info = [] { return std::string("{\"ok\":true,\"dispositivo\":\"nexuspico\"}"); };
  auto enviar = [&](const std::string& s) {
    for (unsigned char ch : s) con.byte(ch);
  };
  enviar("{\"info\":true}\n");
  OK(salida.find("nexuspico") != std::string::npos && !con.reiniciar);
  salida.clear();
  enviar("{\"wifi\":{\"ssid\":\"Casa\",\"pass\":\"p\"},\"nexus\":{\"host\":\"10.0.0.2:8775\",\"key\":\"k\"}}\r\n");
  OK(salida.find("\"ok\":true") != std::string::npos && con.reiniciar && guardada.ssid == "Casa" && cfg.host == "10.0.0.2:8775");
  salida.clear();
  con.reiniciar = false;
  enviar("{roto\n");
  OK(salida.find("\"ok\":false") != std::string::npos && !con.reiniciar);

  // ---- consola: lo que mandan los Nexus antiguos por el REPL en bruto de MicroPython
  salida.clear();
  std::string lit = "'{\"wifi\": {\"ssid\": \"Vieja\", \"pass\": \"it\\'s\"}, \"nexus\": {\"host\": \"1.2.3.4:8775\", \"key\": \"q\"}}'";
  enviar("\r\x03\x03");
  enviar("\r\x01");
  enviar("f=open('/config.json','w')\nf.write(" + lit + ")\nf.close()\nprint('NEXUS-OK')\n\x04");
  OK(salida.find("NEXUS-OK") != std::string::npos);
  OK(cfg.ssid == "Vieja" && cfg.pass == "it's" && cfg.host == "1.2.3.4:8775");
  enviar("\x02");
  con.reiniciar = false;
  enviar("import machine; machine.reset()\r");
  OK(con.reiniciar);

  // ---- portal
  np::Config pc;
  bool g = false;
  std::string r = np::portal("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", pc, g);
  OK(!g && r.find("<form>") != std::string::npos);
  r = np::portal("GET /?ssid=Mi+Red&pass=a%26b&host=192.168.1.5%3A8775&key=K HTTP/1.1\r\n\r\n", pc, g);
  OK(g && pc.ssid == "Mi Red" && pc.pass == "a&b" && pc.host == "192.168.1.5:8775" && pc.key == "K");
  pc.ssid = "<x>";
  r = np::portal("GET / HTTP/1.1\r\n\r\n", pc, g);
  OK(r.find("<x>") == std::string::npos && r.find("&lt;x&gt;") != std::string::npos);

  if (fallos) {
    std::fprintf(stderr, "np_test: %d fallos\n", fallos);
    return 1;
  }
  std::printf("np_test OK\n");
  return 0;
}
