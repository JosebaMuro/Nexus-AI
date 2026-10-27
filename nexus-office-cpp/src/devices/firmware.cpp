// Grabar firmware (nexus/devices/firmware.py y firmware/flash.py) con puertos serie nativos en cada sistema.
#include "devices/firmware.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "devices/drivers.h"
#include "nexus_flash.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::devices::firmware {

const std::vector<std::string> MODELOS_LUCES = {"esp8266", "esp32",   "esp32s2", "esp32s3",
                                                "esp32c2", "esp32c3", "esp32c5", "esp32c6"};
// imagenes del ESP8266 por tamano de flash: la de 1 MB vale para el ESP-01S y cualquier placa mayor
const json VARIANTES_ESP8266 = jparse(R"J([
  {"modelo": "esp8266", "nombre": "ESP8266 1 MB (ESP-01S, ESP-12, NodeMCU, Wemos D1 mini)"},
  {"modelo": "esp8266-4m", "nombre": "ESP8266 4 MB (NodeMCU, Wemos D1 mini; mas espacio)"},
  {"modelo": "esp8266-512k", "nombre": "ESP8266 512 KB (ESP-01 antiguo, azul)"}
])J");

// toda la familia ESP que se puede grabar desde Nexus (con Wi-Fi: luces; sin Wi-Fi solo se reconocen)
const json FAMILIA = jparse(R"J([
  {"chip": "esp8266", "nombre": "ESP8266", "wifi": true, "usb_nativo": false,
   "placas": ["ESP-01", "ESP-01S", "ESP-12E/F", "NodeMCU", "Wemos D1 mini"],
   "nota": "ESP-01: une GPIO0 a GND al enchufarlo para entrar en modo de grabacion"},
  {"chip": "esp32", "nombre": "ESP32", "wifi": true, "usb_nativo": false,
   "placas": ["ESP32 DevKitC", "ESP32-WROOM-32", "ESP32-WROVER", "ESP32-CAM", "ESP32-PICO"]},
  {"chip": "esp32s2", "nombre": "ESP32-S2", "wifi": true, "usb_nativo": true, "placas": ["ESP32-S2 Mini", "Saola"]},
  {"chip": "esp32s3", "nombre": "ESP32-S3", "wifi": true, "usb_nativo": true,
   "placas": ["ESP32-S3 DevKitC-1", "XIAO ESP32S3", "T-Display S3"]},
  {"chip": "esp32c2", "nombre": "ESP32-C2 (ESP8684)", "wifi": true, "usb_nativo": false, "placas": ["ESP8684 DevKitM"]},
  {"chip": "esp32c3", "nombre": "ESP32-C3", "wifi": true, "usb_nativo": true,
   "placas": ["ESP32-C3 SuperMini", "XIAO ESP32C3", "ESP32-2424S012C (reloj)"]},
  {"chip": "esp32c5", "nombre": "ESP32-C5 (Wi-Fi 2,4 y 5 GHz)", "wifi": true, "usb_nativo": true, "placas": ["ESP32-C5 DevKitC-1"]},
  {"chip": "esp32c6", "nombre": "ESP32-C6", "wifi": true, "usb_nativo": true, "placas": ["ESP32-C6 DevKitC-1", "XIAO ESP32C6"]},
  {"chip": "esp32h2", "nombre": "ESP32-H2", "wifi": false, "usb_nativo": true, "placas": ["ESP32-H2 DevKitM-1"],
   "nota": "sin Wi-Fi (Thread y Zigbee): no puede hablar con Nexus"},
  {"chip": "esp32p4", "nombre": "ESP32-P4", "wifi": false, "usb_nativo": true, "placas": ["ESP32-P4 Function EV"],
   "nota": "sin Wi-Fi propio (va con un ESP32-C6 de acompanante)"}
])J");

namespace {

auto L = log::get("firmware");
// fabricantes USB: Espressif nativo, CP210x, CH340/CH9102, FTDI; y Raspberry Pi (Pico)
const std::vector<int> VID_ESP = {0x303A, 0x10C4, 0x1A86, 0x0403};
const int VID_PICO = 0x2E8A;

std::string tipo_de(int vid) {
  if (vid == VID_PICO) return "pico";
  return std::find(VID_ESP.begin(), VID_ESP.end(), vid) != VID_ESP.end() ? "esp32" : "";
}

std::string hex4(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%04x", v);
  return b;
}

void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::milliseconds((long long)(s * 1000))); }

// ---------------------------------------------------------------- puerto serie (pyserial minimo)
class Serial {
 public:
  explicit Serial(const std::string& port, int baud = 115200) {
#ifdef _WIN32
    std::string dev = starts_with(port, "\\\\.\\") ? port : "\\\\.\\" + port;
    h_ = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) throw std::runtime_error("no se puede abrir " + port);
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h_, &dcb);
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(h_, &dcb);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 300;
    to.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(h_, &to);
#else
    fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) throw std::runtime_error("no se puede abrir " + port + ": " + std::strerror(errno));
    termios t{};
    tcgetattr(fd_, &t);
    cfmakeraw(&t);
    speed_t sp = baud == 115200 ? B115200 : B9600;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 3;   // 0,3 s como el timeout de pyserial
    tcsetattr(fd_, TCSANOW, &t);
#endif
  }
  ~Serial() {
#ifdef _WIN32
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
  }
  void write(const std::string& s) {
#ifdef _WIN32
    DWORD n = 0;
    WriteFile(h_, s.data(), (DWORD)s.size(), &n, nullptr);
#else
    size_t off = 0;
    while (off < s.size()) {
      ssize_t n = ::write(fd_, s.data() + off, s.size() - off);
      if (n <= 0) break;
      off += (size_t)n;
    }
#endif
  }
  std::string read(size_t max) {   // lo que llegue en ~0,3 s
    std::string buf(max, '\0');
#ifdef _WIN32
    DWORD n = 0;
    if (!ReadFile(h_, buf.data(), (DWORD)max, &n, nullptr)) n = 0;
#else
    ssize_t n = ::read(fd_, buf.data(), max);
    if (n < 0) n = 0;
#endif
    buf.resize((size_t)n);
    return buf;
  }
  std::string readline() {   // hasta \n o 0,3 s sin datos
    std::string line;
    while (true) {
      std::string c = read(1);
      if (c.empty()) break;
      line += c;
      if (c == "\n") break;
    }
    return line;
  }
  void reset_input() {
#ifdef _WIN32
    PurgeComm(h_, PURGE_RXCLEAR);
#else
    tcflush(fd_, TCIFLUSH);
#endif
  }

 private:
#ifdef _WIN32
  HANDLE h_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

#ifdef _WIN32
std::wstring wreg_str(HKEY k, const wchar_t* name) {
  wchar_t buf[512];
  DWORD sz = sizeof buf, type = 0;
  if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS || type != REG_SZ) return L"";
  return std::wstring(buf, wcsnlen(buf, sz / sizeof(wchar_t)));
}
std::string narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?');
  return s;
}
#endif

std::string puerto_auto(const std::string& tipo) {
  for (auto& p : puertos())
    if (jstr(p, "tipo") == tipo) return jstr(p, "puerto");
  json st = drivers::estado();
  if (!st["faltan"].empty())
    throw std::runtime_error("la placa esta enchufada pero falta el driver USB-serie (" + join(st["faltan"].get<std::vector<std::string>>(), ", ") +
                             "): instalalo en Grabar firmware > Drivers o con nexus-office firmware drivers --instalar");
  throw std::runtime_error("no encuentro el dispositivo: conectalo por USB (o indica el puerto)");
}

fs::path binario(const std::string& tipo, const std::string& modelo = "") {
  fs::path bin = carpeta() / "bin";
  if (tipo == "luces") return bin / ("nexusled-" + (modelo.empty() ? std::string("esp32") : modelo) + ".bin");
  if (tipo == "reloj") return bin / "nexuswatch-esp32c3.bin";
  if (tipo == "pico") return bin / "nexuspico-picow.uf2";
  throw std::invalid_argument("tipo desconocido: " + tipo);
}

// el grabador nativo (nexus-flash) es el de siempre; NEXUS_GRABADOR=esptool usa esptool si esta instalado
bool usar_esptool() { return lower(env("NEXUS_GRABADOR")) == "esptool"; }

// avance de nexus-flash al registro: cada fase nueva y cada 10 %
nexusflash::Avance avance(const Log& log) {
  auto ultimo = std::make_shared<std::pair<std::string, int>>("", -100);
  return [log, ultimo](const std::string& fase, int pct, const std::string& texto) {
    if (fase == ultimo->first && pct < 100 && pct - ultimo->second < 10) return;
    ultimo->first = fase;
    ultimo->second = pct;
    log(fase == "grabar" || fase == "copiar" ? texto + " (" + std::to_string(pct) + " %)" : texto);
  };
}

std::string esptool(const std::vector<std::string>& args, const Log& log) {
  auto cmd = esptool_cmd();
  if (cmd.empty()) throw std::runtime_error("falta esptool: copia el ejecutable esptool en firmware/tools o instala Python con esptool");
  log("$ esptool " + join(args, " "));
  cmd.insert(cmd.end(), args.begin(), args.end());
  fs::path out = fs::temp_directory_path() / ("nexus-esptool-" + token_hex(4) + ".log");
  proc::Options opt;
  opt.output_file = out;
  proc::Process p;
  if (!p.start(cmd, opt)) throw std::runtime_error("no se pudo ejecutar esptool: " + p.error());
  size_t shown = 0;
  std::string all;
  std::optional<int> code;
  while (true) {   // registro en vivo para la web
    code = p.poll();
    all = read_text(out);
    while (true) {
      size_t nl = all.find_first_of("\r\n", shown);
      if (nl == std::string::npos) break;
      std::string line = rtrim(all.substr(shown, nl - shown));
      if (!line.empty()) log(line);
      shown = nl + 1;
    }
    if (code) break;
    sleep_s(0.2);
  }
  std::error_code ec;
  fs::remove(out, ec);
  if (*code != 0) {
    auto lines = splitlines(all);
    std::vector<std::string> tail(lines.size() > 8 ? lines.end() - 8 : lines.begin(), lines.end());
    throw std::runtime_error("esptool fallo:\n" + join(tail, "\n"));
  }
  return all;
}

}  // namespace

std::string chip_de_texto(const std::string& out) {
  static const Rx R(R"((?:Chip is|Chip type:|Detecting chip type\.\.\.)\s*(ESP8266\w*|ESP32(?:-[A-Z0-9]+)?|ESP8684))");
  auto m = R.search(out);
  if (!m) return "";
  std::string chip = replace_all(lower(m->str(1)), "-", "");
  if (starts_with(chip, "esp8266")) return "esp8266";
  if (chip == "esp8684") return "esp32c2";
  // el ESP32 clasico aparece como ESP32-D0WD-V3, ESP32-PICO-D4...
  for (auto c : {"esp32s2", "esp32s3", "esp32c2", "esp32c3", "esp32c5", "esp32c61", "esp32c6", "esp32h2", "esp32p4"})
    if (starts_with(chip, c)) return c;
  return "esp32";
}

namespace {

std::string detectar_chip(const std::string& puerto, const Log& log) {
  if (!usar_esptool()) return nexusflash::detectar(puerto, avance(log), 20);   // ya avisa del GPIO0 del ESP-01
  std::string out;
  try {
    out = esptool({"--port", puerto, "chip_id"}, log);
  } catch (const std::runtime_error& e) {
    throw std::runtime_error(std::string(e.what()) +
                             "\nSi es un ESP-01 o una placa sin boton BOOT: une GPIO0 a GND al enchufarla y vuelve a probar.");
  }
  std::string chip = chip_de_texto(out);
  if (chip.empty()) throw std::runtime_error("no se reconoce el chip");
  return chip;
}

}  // namespace

fs::path carpeta() {
  std::vector<fs::path> cands;
  std::string e = env("NEXUS_FIRMWARE");
  if (!e.empty()) cands.push_back(P(e));
  cands.push_back(paths::root() / "firmware");
  cands.push_back(paths::package().parent_path() / "firmware");
  cands.push_back(paths::package().parent_path().parent_path() / "firmware");   // repositorio: nexus-office/nexus
  for (auto& c : cands)
    if (is_file(c / "bin" / "firmware.json") || is_file(c / "flash.py")) return c;
  return {};
}

std::vector<std::string> esptool_cmd() {
  fs::path base = carpeta();
  for (auto n : {"esptool", "esptool.exe"})
    if (!base.empty() && is_file(base / "tools" / n)) return {S(base / "tools" / n)};
  std::string w = proc::which("esptool");
  if (w.empty()) w = proc::which("esptool.py");
  if (!w.empty()) return {w};
  for (auto py : {"python3", "python"}) {
    std::string pw = proc::which(py);
    if (pw.empty()) continue;
    auto r = proc::run({pw, "-c", "import esptool"}, 20);
    if (r.ok()) return {pw, "-m", "esptool"};
  }
  return {};
}

json disponible() {
  json falta = json::array();
  bool et = usar_esptool();
  if (et && esptool_cmd().empty()) falta.push_back("esptool");
  return {{"carpeta", carpeta().empty() ? "" : S(carpeta())}, {"falta", falta},
          {"grabador", et ? "esptool" : std::string("nexus-flash ") + NEXUSFLASH_VERSION}};
}

json binarios() {
  fs::path bin = carpeta() / "bin";
  json info = jparse(read_text(bin / "firmware.json"));
  json arch = info.is_object() ? info.value("archivos", json::object()) : json::object();
  std::vector<fs::path> files;
  std::error_code ec;
  if (!carpeta().empty())
    for (auto& ext : {".bin", ".uf2"}) {
      std::vector<fs::path> one;
      for (auto& e : fs::directory_iterator(bin, ec))
        if (e.path().extension() == ext) one.push_back(e.path());
      std::sort(one.begin(), one.end());
      files.insert(files.end(), one.begin(), one.end());
    }
  json out = json::array();
  for (auto& p : files) {
    json o = {{"archivo", S(p.filename())}, {"bytes", (long long)fs::file_size(p, ec)}};
    json extra = arch.value(S(p.filename()), json::object());
    if (extra.is_object())
      for (auto it = extra.begin(); it != extra.end(); ++it) o[it.key()] = it.value();
    out.push_back(o);
  }
  return out;
}

std::vector<fs::path> unidades_pico() {
  std::vector<fs::path> cand;
  std::error_code ec;
#ifdef _WIN32
  for (char c = 'D'; c <= 'Z'; ++c) cand.push_back(fs::path(std::string(1, c) + ":/"));
#else
  std::string user = env("USER");
  for (auto base : {std::string("/media"), "/media/" + user, "/run/media/" + user, std::string("/Volumes"), std::string("/mnt")})
    if (fs::is_directory(base, ec))
      for (auto& e : fs::directory_iterator(base, ec))
        if (e.is_directory(ec)) cand.push_back(e.path());
#endif
  std::vector<fs::path> out;
  for (auto& p : cand)
    if (fs::exists(p / "INFO_UF2.TXT", ec)) out.push_back(p);
  return out;
}

json puertos() {
  json out = json::array();
  std::error_code ec;
#if defined(_WIN32)
  std::map<std::string, std::pair<int, int>> ids;   // COMx -> (vid, pid) de los dispositivos USB
  HKEY usb;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usb) == ERROR_SUCCESS) {
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
      DWORD n = 256;
      if (RegEnumKeyExW(usb, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      unsigned vid = 0, pid = 0;
      if (swscanf(name, L"VID_%4x&PID_%4x", &vid, &pid) != 2) continue;
      HKEY dev;
      if (RegOpenKeyExW(usb, name, 0, KEY_READ, &dev) != ERROR_SUCCESS) continue;
      wchar_t inst[256];
      for (DWORD j = 0;; ++j) {
        DWORD m = 256;
        if (RegEnumKeyExW(dev, j, inst, &m, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY par;
        std::wstring sub = std::wstring(inst) + L"\\Device Parameters";
        if (RegOpenKeyExW(dev, sub.c_str(), 0, KEY_READ, &par) != ERROR_SUCCESS) continue;
        std::string port = narrow(wreg_str(par, L"PortName"));
        if (!port.empty()) ids[port] = {(int)vid, (int)pid};
        RegCloseKey(par);
      }
      RegCloseKey(dev);
    }
    RegCloseKey(usb);
  }
  HKEY sc;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &sc) == ERROR_SUCCESS) {
    wchar_t vname[256], data[64];
    for (DWORD i = 0;; ++i) {
      DWORD vn = 256, dn = sizeof data, type = 0;
      if (RegEnumValueW(sc, i, vname, &vn, nullptr, &type, (LPBYTE)data, &dn) != ERROR_SUCCESS) break;
      if (type != REG_SZ) continue;
      std::string port = narrow(std::wstring(data, wcsnlen(data, dn / sizeof(wchar_t))));
      auto it = ids.find(port);
      int vid = it == ids.end() ? 0 : it->second.first, pid = it == ids.end() ? 0 : it->second.second;
      out.push_back({{"puerto", port}, {"descripcion", narrow(vname)}, {"tipo", tipo_de(vid)},
                     {"vid", vid ? hex4(vid) : ""}, {"pid", pid ? hex4(pid) : ""}});
    }
    RegCloseKey(sc);
  }
#elif defined(__APPLE__)
  for (auto& e : fs::directory_iterator("/dev", ec)) {
    std::string n = S(e.path().filename());
    if (!starts_with(n, "cu.") || n == "cu.Bluetooth-Incoming-Port" || n.find("debug") != std::string::npos) continue;
    std::string tipo = n.find("usbserial") != std::string::npos || n.find("SLAB") != std::string::npos ||
                               n.find("wchusbserial") != std::string::npos
                           ? "esp32"
                           : "";
    out.push_back({{"puerto", S(e.path())}, {"descripcion", n}, {"tipo", tipo}, {"vid", ""}, {"pid", ""}});
  }
#else
  for (auto& e : fs::directory_iterator("/sys/class/tty", ec)) {
    std::string n = S(e.path().filename());
    fs::path dev = e.path() / "device";
    if (!fs::exists(dev, ec)) continue;
    fs::path real = fs::canonical(dev, ec);
    if (ec) continue;
    int vid = 0, pid = 0;
    std::string desc;
    for (fs::path p = real; !p.empty() && p != p.root_path(); p = p.parent_path()) {   // sube hasta el dispositivo USB
      if (is_file(p / "idVendor")) {
        vid = (int)std::strtol(trim(read_text(p / "idVendor")).c_str(), nullptr, 16);
        pid = (int)std::strtol(trim(read_text(p / "idProduct")).c_str(), nullptr, 16);
        desc = trim(read_text(p / "product"));
        break;
      }
    }
    if (!vid && !starts_with(n, "ttyUSB") && !starts_with(n, "ttyACM")) continue;   // puertos serie de la placa base
    out.push_back({{"puerto", "/dev/" + n}, {"descripcion", desc.empty() ? n : desc}, {"tipo", tipo_de(vid)},
                   {"vid", vid ? hex4(vid) : ""}, {"pid", pid ? hex4(pid) : ""}});
  }
#endif
  for (auto& d : unidades_pico())
    out.push_back({{"puerto", S(d)}, {"descripcion", "Pico en modo BOOTSEL (RPI-RP2)"}, {"tipo", "pico-bootsel"}});
  return out;
}

json flashear_esp32(const std::string& tipo, std::string puerto, std::string modelo, const Log& log) {
  if (puerto.empty()) puerto = puerto_auto("esp32");
  // una sola conexion con la ROM: detectar, comprobar y grabar (en las placas con USB nativo, reiniciar entre medias
  // haria desaparecer el puerto un momento)
  std::unique_ptr<nexusflash::Grabador> g;
  std::string chip;
  if (usar_esptool()) {
    chip = detectar_chip(puerto, log);
  } else {
    g = std::make_unique<nexusflash::Grabador>(puerto, avance(log));
    chip = g->conectar(20).nombre;
  }
  try {
    if (tipo == "reloj" && chip != "esp32c3")
      throw std::runtime_error("el reloj ESP32-2424S012C lleva un ESP32-C3 y este es un " + chip);
    if (modelo.empty()) modelo = chip;
    std::string base = starts_with(modelo, "esp8266") ? "esp8266" : modelo;   // esp8266-4m, esp8266-512k
    bool conocido = std::find(MODELOS_LUCES.begin(), MODELOS_LUCES.end(), modelo) != MODELOS_LUCES.end();
    for (auto& v : VARIANTES_ESP8266) conocido |= jstr(v, "modelo") == modelo;
    if (tipo == "luces" && !conocido)
      throw std::runtime_error("no hay NexusLED para " + modelo + " (hay: " + join(MODELOS_LUCES, ", ") + ")");
    if (tipo == "luces" && base != chip)
      throw std::runtime_error("la placa conectada es un " + chip + " y el firmware elegido es para " + modelo);
    fs::path fw = binario(tipo, modelo);
    if (!is_file(fw)) throw std::runtime_error("falta " + S(fw.filename()) + " en firmware/bin");
    if (g) {
      log("grabando " + S(fw.filename()) + " en " + puerto + " con nexus-flash");
      // borra, graba, verifica por MD5 (menos el ESP8266, cuya ROM no lo tiene) y reinicia
      nexusflash::Opciones op;
      op.chip = chip;
      g->grabar(nexusflash::leer_archivo(S(fw)), 0x0, op);
      g.reset();   // suelta el puerto para la configuracion
    } else {
      std::vector<std::string> args = {"--chip", base, "--port", puerto, "--baud", base == "esp8266" ? "115200" : "460800",
                                       "write_flash"};
      if (base == "esp8266") args.insert(args.end(), {"--flash_size", "detect"});   // ESP-01 con 512 KB o 1 MB
      args.insert(args.end(), {"0x0", S(fw)});
      esptool(args, log);
    }
    log(S(fw.filename()) + " grabado en " + puerto);
    return {{"ok", true}, {"puerto", puerto}, {"chip", chip}, {"archivo", S(fw.filename())}};
  } catch (...) {
    if (g) {
      try {
        g->reiniciar();   // que vuelva a arrancar su firmware
      } catch (...) {
      }
    }
    throw;
  }
}

json configurar_esp32(const std::string& tipo, std::string puerto, const json& cfg, const Log& log, double espera_s) {
  if (puerto.empty()) puerto = puerto_auto("esp32");
  json msg = json::object();
  if (tipo == "luces") {
    json c = json::object();
    for (auto it = cfg.begin(); it != cfg.end(); ++it)
      if (it.key() != "nexus") c[it.key()] = it.value();
    msg["config"] = c;
  } else {
    for (auto k : {"wifi", "nexus"})
      if (cfg.contains(k)) msg[k] = cfg[k];
  }
  std::string linea = jdump(msg) + "\n";
  Serial s(puerto);
  double fin = now() + espera_s;
  while (now() < fin) {   // tras grabar, el ESP32 tarda un poco en arrancar
    s.write(linea);
    double t = now() + 1.5;
    while (now() < t) {
      std::string r = trim(s.readline());
      if (starts_with(r, "{")) {
        log("respuesta: " + r);
        json j = jparse(r);
        return j.is_discarded() ? json{{"respuesta", r}} : j;
      }
    }
  }
  throw std::runtime_error("el dispositivo no responde por el puerto serie");
}

json flashear_pico(const std::string& unidad, const Log& log, double espera_s) {
  fs::path fw = binario("pico");
  if (!is_file(fw)) throw std::runtime_error("falta " + S(fw.filename()) + " en firmware/bin");
  std::vector<fs::path> dest = unidad.empty() ? unidades_pico() : std::vector<fs::path>{P(unidad)};
  if (dest.empty()) throw std::runtime_error("no veo la unidad RPI-RP2: conecta el Pico W manteniendo pulsado BOOTSEL");
  fs::copy_file(fw, dest[0] / fw.filename(), fs::copy_options::overwrite_existing);
  log(S(fw.filename()) + " copiado a " + S(dest[0]) + "; el Pico se reinicia");
  double fin = now() + espera_s;
  while (now() < fin) {
    try {
      return {{"ok", true}, {"puerto", puerto_auto("pico")}};
    } catch (const std::runtime_error&) {
      sleep_s(1);
    }
  }
  return {{"ok", true}, {"puerto", ""}};
}

json configurar_pico(std::string puerto, const json& cfg, const Log& log) {
  // escribe /config.json en el Pico por el REPL en bruto de MicroPython y lo reinicia
  if (puerto.empty()) puerto = puerto_auto("pico");
  std::string datos = jdump(cfg), lit = "'";
  for (char c : datos) lit += c == '\'' ? std::string("\\'") : c == '\\' ? std::string("\\\\") : std::string(1, c);
  lit += "'";
  std::string codigo = "f=open('/config.json','w')\nf.write(" + lit + ")\nf.close()\nprint('NEXUS-OK')\n";
  Serial s(puerto);
  s.write("\r\x03\x03");   // interrumpe el programa
  sleep_s(0.3);
  s.write("\r\x01");       // REPL en bruto
  sleep_s(0.3);
  s.reset_input();
  s.write(codigo + "\x04");
  double fin = now() + 6;
  std::string salida;
  while (now() < fin && salida.find("NEXUS-OK") == std::string::npos) salida += s.read(256);
  s.write("\x02");         // REPL normal
  if (salida.find("NEXUS-OK") == std::string::npos)
    throw std::runtime_error("el Pico no acepto la configuracion: " + (salida.size() > 200 ? salida.substr(salida.size() - 200) : salida));
  s.write("import machine; machine.reset()\r");
  log("config.json grabado en el Pico; reiniciando");
  return {{"ok", true}, {"puerto", puerto}};
}

// ---------------------------------------------------------------- trabajos
json Trabajos::lanzar(const std::string& tipo, const std::string& puerto, const std::string& modelo, const json& config,
                      bool solo_config) {
  if (tipo != "luces" && tipo != "reloj" && tipo != "pico") throw std::invalid_argument("tipo: luces, reloj o pico");
  std::string tid = uuid_hex().substr(0, 8);
  json job = {{"id", tid}, {"tipo", tipo}, {"estado", "en_curso"}, {"log", json::array()}, {"inicio", now()},
              {"resultado", nullptr}};
  {
    std::lock_guard<std::mutex> lk(mu_);
    t_[tid] = job;
    while (t_.size() > 20) {
      auto oldest = std::min_element(t_.begin(), t_.end(), [](auto& a, auto& b) {
        return jnum(a.second["inicio"]) < jnum(b.second["inicio"]);
      });
      t_.erase(oldest);
    }
  }
  auto upd = [this, tid](const std::function<void(json&)>& f) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = t_.find(tid);
    if (it != t_.end()) f(it->second);
  };
  Log log_ = [upd](const std::string& linea) {
    upd([&](json& j) {
      j["log"].push_back(linea.size() > 300 ? linea.substr(linea.size() - 300) : linea);
      while (j["log"].size() > 400) j["log"].erase(0);
    });
  };
  std::thread([this, tipo, puerto, modelo, config, solo_config, upd, log_] {
    std::unique_lock<std::mutex> busy(ocupado_, std::defer_lock);
    for (int i = 0; i < 10 && !busy.try_lock(); ++i) sleep_s(0.1);
    if (!busy.owns_lock()) {
      upd([](json& j) {
        j["estado"] = "error";
        j["resultado"] = {{"error", "ya hay otro grabado en curso"}};
      });
      return;
    }
    try {
      json res = json::object();
      bool hay_cfg = config.is_object() && !config.empty();
      if (tipo == "pico") {
        std::string p = puerto;
        if (!solo_config) {
          std::error_code ec;
          p = jstr(flashear_pico(!puerto.empty() && fs::is_directory(P(puerto), ec) ? puerto : "", log_), "puerto");
        }
        if (hay_cfg) res = configurar_pico(p, config, log_);
      } else {
        std::string p = puerto;
        if (!solo_config) {
          res = flashear_esp32(tipo, puerto, modelo, log_);
          p = jstr(res, "puerto");
        }
        if (hay_cfg) res["config"] = configurar_esp32(tipo, p, config, log_);
      }
      upd([&](json& j) {
        j["estado"] = "hecho";
        j["resultado"] = res.empty() ? json{{"ok", true}} : res;
      });
      log_("Listo.");
    } catch (const std::exception& e) {
      L.warning("firmware ", tipo, ": ", e.what());
      log_(std::string("Error: ") + e.what());
      upd([&](json& j) {
        j["estado"] = "error";
        j["resultado"] = {{"error", e.what()}};
      });
    }
    upd([](json& j) { j["fin"] = now(); });
  }).detach();
  return job;
}

json Trabajos::get(const std::string& tid) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = t_.find(tid);
  return it == t_.end() ? json() : it->second;
}

Trabajos& trabajos() {
  static Trabajos T;
  return T;
}

}  // namespace nx::devices::firmware
