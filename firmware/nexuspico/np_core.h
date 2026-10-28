// NexusPico - logica del avatar de bolsillo sin hardware (se compila igual en el Pico W y en el PC para las pruebas).
//
// Nexus genera una imagen por estado (en linea, escuchando, pensando, usando herramienta, hablando, hecho, error,
// aviso) a partir del avatar. El Pico las descarga una vez, las guarda en su flash y despues solo pregunta el
// estado cada 2 s: cuando cambia, pinta la imagen guardada. Si Nexus cambia de nombre o de estilo, la version
// cambia y el Pico vuelve a descargarlas.
//
// La configuracion llega por el USB como una linea JSON (igual que las luces y el reloj), o desde la pagina del
// propio Pico (sin Wi-Fi crea la red "NexusPico-XXXX", clave "nexuspico", en http://192.168.4.1):
//   {"wifi": {"ssid": "...", "pass": "..."}, "nexus": {"host": "192.168.1.10:8775", "key": "..."},
//    "cada_s": 2, "completo_cada": 20, "pines": {"rst": 12, "dc": 8, "cs": 9, "clk": 10, "din": 11, "busy": 13}}
// Tambien entiende la escritura de /config.json por el REPL en bruto de MicroPython que usaban las versiones
// anteriores de Nexus, para que un Nexus antiguo pueda configurarlo.
#pragma once
#include <stdint.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include "../nexusled/nl_json.h"
#include "../nexuswatch/nw_assets.h"

#ifndef pgm_read_byte
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#endif

namespace np {

static const char* const VERSION = "2.0.0";
static const int ANCHO = 122, ALTO = 250;           // pantalla en vertical
static const int BYTES = ((ANCHO + 7) / 8) * ALTO;  // 4000: MONO_HLSB, 1 = blanco
static const int APA_W = 250, APA_H = 122;          // lienzo apaisado para los textos

struct Pines {
  int rst = 12, dc = 8, cs = 9, clk = 10, din = 11, busy = 13;
};

struct Config {
  std::string ssid, pass, host, key;
  int cada_s = 2, completo_cada = 20;
  Pines pines;
  bool valida() const { return !ssid.empty() && !host.empty(); }
};

// ---------------------------------------------------------------- configuracion en JSON
inline void leer_int(const nl::JVal* o, const char* k, int& v) {
  if (o)
    if (const nl::JVal* x = o->get(k))
      if (x->isNum() || x->isStr()) v = (int)x->num(v);
}
inline void leer_str(const nl::JVal* o, const char* k, std::string& v) {
  if (o)
    if (const nl::JVal* x = o->get(k))
      if (x->isStr()) v = x->s;
}

// Mezcla en `cfg` lo que traiga el JSON. Devuelve false si no es un objeto JSON valido.
inline bool mezclar_config(const std::string& texto, Config& cfg) {
  nl::JVal j;
  if (!nl::JParser(texto).parse(j) || !j.isObj()) return false;
  if (const nl::JVal* w = j.get("wifi")) {
    leer_str(w, "ssid", cfg.ssid);
    leer_str(w, "pass", cfg.pass);
  }
  if (const nl::JVal* n = j.get("nexus")) {
    leer_str(n, "host", cfg.host);
    leer_str(n, "key", cfg.key);
  }
  leer_int(&j, "cada_s", cfg.cada_s);
  leer_int(&j, "completo_cada", cfg.completo_cada);
  if (cfg.cada_s < 1) cfg.cada_s = 1;
  if (cfg.completo_cada < 1) cfg.completo_cada = 1;
  if (const nl::JVal* p = j.get("pines")) {
    leer_int(p, "rst", cfg.pines.rst);
    leer_int(p, "dc", cfg.pines.dc);
    leer_int(p, "cs", cfg.pines.cs);
    leer_int(p, "clk", cfg.pines.clk);
    leer_int(p, "din", cfg.pines.din);
    leer_int(p, "busy", cfg.pines.busy);
  }
  return true;
}

inline std::string config_a_json(const Config& c) {
  nl::JWriter w;
  w.obj();
  w.key("wifi").obj().kv("ssid", c.ssid).kv("pass", c.pass).end();
  w.key("nexus").obj().kv("host", c.host).kv("key", c.key).end();
  w.kv("cada_s", c.cada_s).kv("completo_cada", c.completo_cada);
  w.key("pines").obj().kv("rst", c.pines.rst).kv("dc", c.pines.dc).kv("cs", c.pines.cs).kv("clk", c.pines.clk);
  w.kv("din", c.pines.din).kv("busy", c.pines.busy).end();
  w.end();
  return w.out;
}

// ---------------------------------------------------------------- almacen en sectores de flash
// `Sectores` es la flash cruda: N sectores de 4 KB. Cada dato guardado ocupa un sector con cabecera
// (magia, longitud, suma); el sector 0 es el indice de nombres.
struct Sectores {
  virtual ~Sectores() = default;
  virtual int cuantos() const = 0;
  virtual const uint8_t* leer(int sector) = 0;                         // 4096 bytes
  virtual bool escribir(int sector, const uint8_t* datos4096) = 0;     // borra y programa
};

class Almacen {
 public:
  static const int SECTOR = 4096;
  static const uint32_t MAGIA = 0x314B504E;   // "NPK1"
  static const int MAX = SECTOR - 12;

  explicit Almacen(Sectores& s) : s_(s) { cargar_indice(); }

  bool leer(const std::string& nombre, std::string& out) {
    int i = buscar(nombre);
    return i >= 0 && leer_sector(i + 1, out);
  }

  bool escribir(const std::string& nombre, const std::string& datos) {
    if ((int)datos.size() > MAX) return false;
    int i = buscar(nombre);
    if (i < 0) {
      if ((int)nombres_.size() + 1 >= s_.cuantos()) return false;
      nombres_.push_back(nombre);
      i = (int)nombres_.size() - 1;
      if (!guardar_indice()) return false;
    }
    return escribir_sector(i + 1, datos);
  }

  void borrar_todo() {
    nombres_.clear();
    guardar_indice();
  }

 private:
  Sectores& s_;
  std::vector<std::string> nombres_;

  int buscar(const std::string& n) const {
    for (size_t i = 0; i < nombres_.size(); i++)
      if (nombres_[i] == n) return (int)i;
    return -1;
  }
  static uint32_t suma(const std::string& d) {
    uint32_t h = 2166136261u;   // FNV-1a
    for (unsigned char c : d) h = (h ^ c) * 16777619u;
    return h;
  }
  static uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
  static void p32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
  }
  bool leer_sector(int sec, std::string& out) {
    const uint8_t* p = s_.leer(sec);
    if (!p || u32(p) != MAGIA) return false;
    uint32_t n = u32(p + 4);
    if (n > (uint32_t)MAX) return false;
    std::string d((const char*)p + 12, n);
    if (suma(d) != u32(p + 8)) return false;
    out.swap(d);
    return true;
  }
  bool escribir_sector(int sec, const std::string& d) {
    std::vector<uint8_t> b(SECTOR, 0xFF);
    p32(&b[0], MAGIA);
    p32(&b[4], (uint32_t)d.size());
    p32(&b[8], suma(d));
    memcpy(&b[12], d.data(), d.size());
    return s_.escribir(sec, b.data());
  }
  void cargar_indice() {
    std::string t;
    nombres_.clear();
    if (!leer_sector(0, t)) return;
    size_t a = 0;
    while (a < t.size()) {
      size_t b = t.find('\n', a);
      if (b == std::string::npos) b = t.size();
      if (b > a) nombres_.push_back(t.substr(a, b - a));
      a = b + 1;
    }
  }
  bool guardar_indice() {
    std::string t;
    for (auto& n : nombres_) t += n + "\n";
    return escribir_sector(0, t);
  }
};

// ---------------------------------------------------------------- imagenes
// Gira 90 grados (como hace Nexus) una imagen apaisada MONO_HLSB de ancho x alto al formato de la pantalla.
inline std::vector<uint8_t> girar(const std::vector<uint8_t>& apa, int ancho = APA_W, int alto = APA_H) {
  int wb_in = (ancho + 7) / 8, wb = (alto + 7) / 8;
  std::vector<uint8_t> out((size_t)wb * ancho, 0xFF);
  for (int yv = 0; yv < ancho; yv++) {
    int xo = ancho - 1 - yv;
    int bo = xo >> 3, mo = 0x80 >> (xo & 7);
    int base = yv * wb;
    for (int xv = 0; xv < alto; xv++)
      if (!(apa[(size_t)xv * wb_in + bo] & mo)) out[(size_t)base + (xv >> 3)] &= (uint8_t)~(0x80 >> (xv & 7));
  }
  return out;
}

// Lienzo apaisado 250x122, 1 bit (1 = blanco), con la fuente del reloj (16 px, Latin-1)
class Lienzo {
 public:
  std::vector<uint8_t> b = std::vector<uint8_t>(((APA_W + 7) / 8) * APA_H, 0xFF);
  void px(int x, int y) {
    if (x < 0 || y < 0 || x >= APA_W || y >= APA_H) return;
    b[(size_t)y * ((APA_W + 7) / 8) + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
  }
  void marco() {
    for (int x = 0; x < APA_W; x++) px(x, 0), px(x, APA_H - 1);
    for (int y = 0; y < APA_H; y++) px(0, y), px(APA_W - 1, y);
  }
  static int cp_sig(const char*& s) {
    unsigned char c = (unsigned char)*s;
    if (!c) return 0;
    if (c < 0x80) return s++, c;
    if ((c & 0xE0) == 0xC0 && s[1]) {
      int cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
      s += 2;
      return cp;
    }
    s++;
    while ((*s & 0xC0) == 0x80) s++;
    return '?';
  }
  int texto(int x, int y, const char* s, int maxw = APA_W) {
    int x0 = x;
    while (int cp = cp_sig(s)) {
      if (cp < 0x20 || cp > 0xFF) cp = '?';
      const uint16_t* gi = NW_FONT_IDX[cp - 0x20];
      int off = gi[0], w = gi[1], bpr = (w + 7) / 8;
      if (x + w - x0 > maxw) break;
      for (int gy = 0; gy < NW_FONT_H; gy++)
        for (int gx = 0; gx < w; gx++)
          if (pgm_read_byte(NW_FONT + off + gy * bpr + gx / 8) & (0x80 >> (gx & 7))) px(x + gx, y + gy);
      x += w;
    }
    return x - x0;
  }
};

// Pantalla de texto (hasta 6 lineas) ya girada para la tinta
inline std::vector<uint8_t> pantalla_texto(const std::vector<std::string>& lineas) {
  Lienzo l;
  l.marco();
  for (size_t i = 0; i < lineas.size() && i < 6; i++) l.texto(8, 6 + (int)i * 18, lineas[i].c_str(), APA_W - 16);
  return girar(l.b);
}

// ---------------------------------------------------------------- nucleo
using HttpGet = std::function<int(const std::string& ruta, std::string& cuerpo)>;   // codigo HTTP (<0 sin red)
using Pinta = std::function<void(const std::vector<uint8_t>& buf, const std::string& motivo)>;

class Nucleo {
 public:
  Nucleo(HttpGet get, Pinta pinta, Almacen& alm, std::string cliente = "pico")
      : get_(std::move(get)), pinta_(std::move(pinta)), alm_(alm), cliente_(std::move(cliente)) {
    alm_.leer("version", version_);
  }

  const std::string& estado() const { return estado_; }
  const std::string& version() const { return version_; }
  int fallos() const { return fallos_; }
  std::string host;   // solo para el mensaje de "sin conexion"

  // descarga el manifiesto y todas las imagenes; lanza el motivo si falla
  bool sincronizar(std::string& error) {
    std::string cuerpo;
    int cod = get_("/api/dispositivos/pico/manifiesto", cuerpo);
    if (cod != 200) return error = "manifiesto: HTTP " + std::to_string(cod), false;
    nl::JVal man;
    if (!nl::JParser(cuerpo).parse(man) || !man.isObj()) return error = "manifiesto no valido", false;
    const nl::JVal* est = man.get("estados");
    const nl::JVal* ver = man.get("version");
    if (!est || !est->isArr() || !ver || !ver->isStr()) return error = "manifiesto sin estados", false;
    for (auto& e : est->a) {
      if (!e.isStr()) continue;
      std::string datos;
      cod = get_("/api/dispositivos/pico/" + e.s + ".bin", datos);
      if (cod != 200 || (int)datos.size() != BYTES)
        return error = "imagen " + e.s + ": HTTP " + std::to_string(cod) + ", " + std::to_string(datos.size()) + " bytes", false;
      if (!alm_.escribir("img:" + e.s, datos)) return error = "sin sitio en la flash para " + e.s, false;
    }
    alm_.escribir("version", ver->s);
    version_ = ver->s;
    estado_.clear();   // repintar con las imagenes nuevas
    return true;
  }

  bool imagen(const std::string& est, std::string& out) {
    return alm_.leer("img:" + est, out) || alm_.leer("img:idle", out);
  }

  // una consulta: devuelve el estado mostrado ("" si aun no hay nada)
  std::string paso() {
    std::string cuerpo;
    int cod = get_("/api/dispositivos/estado?cliente=" + cliente_ + "&tipo=pico", cuerpo);
    nl::JVal snap;
    if (cod != 200 || !nl::JParser(cuerpo).parse(snap) || !snap.isObj()) {
      if (++fallos_ == 3)
        pinta_(pantalla_texto({std::string("NexusPico ") + VERSION, "", "Sin conexión con Nexus", host, "",
                               cod < 0 ? "sin red" : "HTTP " + std::to_string(cod)}),
               "sin conexion");
      return estado_;
    }
    fallos_ = 0;
    std::string pv, est = "idle";
    leer_str(&snap, "pico_version", pv);
    leer_str(&snap, "estado", est);
    if (est.empty() || est == "none") est = "idle";
    if (!pv.empty() && pv != version_) {
      std::string err;
      if (!sincronizar(err)) {
        pinta_(pantalla_texto({std::string("NexusPico ") + VERSION, "", "No pude bajar", "las imágenes:", err.substr(0, 28)}),
               "error");
        return estado_;
      }
    }
    if (est != estado_) {
      std::string buf;
      if (imagen(est, buf)) {
        pinta_(std::vector<uint8_t>(buf.begin(), buf.end()), est);
        estado_ = est;
      }
    }
    return estado_;
  }

 private:
  HttpGet get_;
  Pinta pinta_;
  Almacen& alm_;
  std::string cliente_, estado_, version_;
  int fallos_ = 0;
};

// ---------------------------------------------------------------- consola por el USB
// Recibe bytes del puerto serie. Una linea JSON cambia la configuracion; tambien entiende la escritura de
// /config.json por el REPL en bruto (Ctrl-A ... Ctrl-D) de las versiones en MicroPython.
class Consola {
 public:
  // guardar(cfg) persiste la configuracion; responder(texto) escribe por el USB; info() devuelve el estado
  std::function<bool(const Config&)> guardar;
  std::function<void(const std::string&)> responder;
  std::function<std::string()> info;
  Config* cfg = nullptr;
  bool reiniciar = false;   // se pide reiniciar tras guardar

  void byte(uint8_t c) {
    if (bruto_) {
      if (c == 0x04) {   // fin del codigo en el REPL en bruto
        responder("OK");
        procesar_repl(buf_);
        responder("\x04\x04>");
        buf_.clear();
      } else if (c == 0x02) {
        bruto_ = false;
        buf_.clear();
      } else if (buf_.size() < 6000) {
        buf_.push_back((char)c);
      }
      return;
    }
    if (c == 0x01) {   // Ctrl-A: REPL en bruto
      bruto_ = true;
      buf_.clear();
      responder("raw REPL; CTRL-B to exit\r\n>");
      return;
    }
    if (c == 0x03) {   // Ctrl-C
      linea_.clear();
      return;
    }
    if (c == '\n' || c == '\r') {
      std::string l;
      l.swap(linea_);
      size_t p = l.find_first_not_of(" \t");
      if (p == std::string::npos) return;
      l = l.substr(p);
      if (l[0] == '{') procesar_json(l);
      else if (l.find("machine.reset") != std::string::npos) reiniciar = true;
      return;
    }
    if (linea_.size() < 4000) linea_.push_back((char)c);
  }

 private:
  bool bruto_ = false;
  std::string linea_, buf_;

  void procesar_json(const std::string& l) {
    Config nueva = *cfg;
    nl::JVal j;
    if (!nl::JParser(l).parse(j) || !j.isObj()) {
      responder("{\"ok\":false,\"error\":\"JSON no valido\"}\r\n");
      return;
    }
    bool cambia = j.has("wifi") || j.has("nexus") || j.has("cada_s") || j.has("completo_cada") || j.has("pines");
    if (!cambia) {   // {"info": true} o cualquier otra cosa: solo informa
      responder(info ? info() + "\r\n" : "{\"ok\":true}\r\n");
      return;
    }
    mezclar_config(l, nueva);
    if (guardar && !guardar(nueva)) {
      responder("{\"ok\":false,\"error\":\"no se pudo guardar\"}\r\n");
      return;
    }
    *cfg = nueva;
    reiniciar = true;
    responder("{\"ok\":true,\"guardado\":true,\"dispositivo\":\"nexuspico\",\"version\":\"" + std::string(VERSION) + "\"}\r\n");
  }

  // f=open('/config.json','w')\nf.write('...')\nf.close()\nprint('NEXUS-OK')
  void procesar_repl(const std::string& codigo) {
    size_t a = codigo.find("f.write(");
    if (codigo.find("/config.json") == std::string::npos || a == std::string::npos) {
      if (codigo.find("machine.reset") != std::string::npos) reiniciar = true;
      responder("\x04Traceback: solo se admite escribir /config.json\r\n");
      return;
    }
    a += 8;
    if (a >= codigo.size() || (codigo[a] != '\'' && codigo[a] != '"')) return responder("\x04literal no valido\r\n");
    char q = codigo[a++];
    std::string lit;
    for (; a < codigo.size() && codigo[a] != q; a++) {
      if (codigo[a] == '\\' && a + 1 < codigo.size()) a++;
      lit.push_back(codigo[a]);
    }
    Config nueva = *cfg;
    if (!mezclar_config(lit, nueva) || (guardar && !guardar(nueva))) return responder("\x04config no valida\r\n");
    *cfg = nueva;
    reiniciar = true;
    responder("NEXUS-OK\r\n\x04");
  }
};

// ---------------------------------------------------------------- portal de configuracion (HTTP)
inline std::string desescapar(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '+') out.push_back(' ');
    else if (c == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      out.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else out.push_back(c);
  }
  return out;
}

inline std::string html_esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '\'' || c == '"') o += "&#39;";
    else o.push_back(c);
  }
  return o;
}

// Responde a una peticion HTTP del portal. Si trae el formulario, actualiza `cfg` y pone `guardado`.
inline std::string portal(const std::string& peticion, Config& cfg, bool& guardado) {
  guardado = false;
  std::string linea = peticion.substr(0, peticion.find("\r\n"));
  size_t q = linea.find('?');
  if (q != std::string::npos && linea.find("ssid=") != std::string::npos) {
    std::string query = linea.substr(q + 1, linea.find(' ', q) - q - 1);
    Config n = cfg;
    size_t a = 0;
    while (a <= query.size()) {
      size_t b = query.find('&', a);
      if (b == std::string::npos) b = query.size();
      std::string par = query.substr(a, b - a);
      size_t e = par.find('=');
      if (e != std::string::npos) {
        std::string k = par.substr(0, e), v = desescapar(par.substr(e + 1));
        if (k == "ssid") n.ssid = v;
        else if (k == "pass") n.pass = v;
        else if (k == "host") n.host = v;
        else if (k == "key") n.key = v;
      }
      a = b + 1;
    }
    cfg = n;
    guardado = true;
    return "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
           "<meta name=viewport content='width=device-width'><h2>Guardado. Reiniciando...</h2>";
  }
  return "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
         "<html><head><meta name=viewport content='width=device-width'><title>NexusPico</title></head>"
         "<body style='font-family:sans-serif;max-width:420px;margin:auto'><h2>NexusPico</h2><form>"
         "Wi-Fi<br><input name=ssid value='" + html_esc(cfg.ssid) + "'><br>Clave<br><input name=pass type=password><br>"
         "Nexus (ip:puerto)<br><input name=host placeholder='192.168.1.10:8775' value='" + html_esc(cfg.host) + "'><br>"
         "Clave de Nexus (Red COOP &gt; Conectar dispositivos)<br><input name=key><br><br>"
         "<button>Guardar</button></form></body></html>";
}

// "192.168.1.10:8775" o "http://nexus.local:8775/" -> host y puerto
inline bool partir_host(const std::string& h, std::string& nombre, int& puerto) {
  std::string s = h;
  if (s.rfind("http://", 0) == 0) s = s.substr(7);
  size_t barra = s.find('/');
  if (barra != std::string::npos) s = s.substr(0, barra);
  size_t dp = s.rfind(':');
  puerto = 8775;
  if (dp != std::string::npos) {
    puerto = atoi(s.c_str() + dp + 1);
    s = s.substr(0, dp);
  }
  nombre = s;
  return !nombre.empty() && puerto > 0 && puerto < 65536;
}

// Separa la respuesta HTTP/1.x en codigo y cuerpo (sin chunked: se pide HTTP/1.0)
inline int respuesta_http(const std::string& r, std::string& cuerpo) {
  if (r.compare(0, 5, "HTTP/") != 0) return -2;
  size_t sp = r.find(' ');
  int cod = sp == std::string::npos ? -2 : atoi(r.c_str() + sp + 1);
  size_t fin = r.find("\r\n\r\n");
  cuerpo = fin == std::string::npos ? std::string() : r.substr(fin + 4);
  return cod;
}

}  // namespace np
