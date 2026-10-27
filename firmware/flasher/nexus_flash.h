// nexus_flash.h - grabador nativo de Nexus para toda la familia ESP y el Pico W, en C++17 sin dependencias.
//
// Habla directamente con el cargador de la ROM de Espressif por el puerto serie (protocolo SLIP), asi que no
// necesita esptool ni Python. Vale para ESP8266 (ESP-01/01S, ESP-12, NodeMCU, Wemos D1 mini...), ESP32, ESP32-S2,
// S3, C2, C3, C5, C6, H2 y P4. El Pico W se graba copiando el .uf2 a la unidad RPI-RP2.
//
// Compila con MSVC, clang de macOS, gcc y el NDK de Android. Una sola cabecera: incluyela y listo.
// Nada es interactivo: el avance se informa por una funcion (fase, porcentaje, texto) y los fallos son excepciones
// nexusflash::Error con el mensaje en castellano. No instala drivers (eso lo hace Nexus en devices/drivers.cpp).
//
//   nexusflash::Grabador g("/dev/ttyUSB0", avance);
//   nexusflash::Chip c = g.conectar();            // reinicia en modo de carga y detecta el chip
//   g.grabar(imagen, 0x0);                        // borra, graba, verifica por MD5 (si la ROM lo permite)
//   g.reiniciar();                                // arranca el firmware nuevo
//
// O todo junto: nexusflash::grabar_archivo(puerto, "nexusled-esp32c3.bin", 0x0, opciones, avance).
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif
#endif

namespace nexusflash {

#define NEXUSFLASH_VERSION "1.0.0"

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// fase: "conectar", "detectar", "baudios", "borrar", "grabar", "verificar", "reiniciar", "configurar", "copiar"
using Avance = std::function<void(const std::string& fase, int pct, const std::string& texto)>;

namespace detail {
inline double ahora() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
inline void dormir(double s) { std::this_thread::sleep_for(std::chrono::microseconds((long long)(s * 1e6))); }
inline std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", (unsigned)v);
  return b;
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
inline uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// MD5 (RFC 1321) para comprobar lo grabado contra lo que calcula el chip
inline std::string md5_hex(const uint8_t* data, size_t len) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const uint8_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                4,  11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  std::vector<uint8_t> m(data, data + len);
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) m.push_back((uint8_t)(bits >> (8 * i)));
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = get32(&m[off + 4 * i]);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) f = (b & c) | (~b & d), g = i;
      else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
      else f = c ^ (b | ~d), g = (7 * i) % 16;
      uint32_t t = d;
      d = c;
      c = b;
      uint32_t x = a + f + K[i] + w[g];
      b = b + ((x << R[i]) | (x >> (32 - R[i])));
      a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
  }
  char out[33];
  for (int i = 0; i < 16; i++) std::snprintf(out + 2 * i, 3, "%02x", (unsigned)((h[i / 4] >> (8 * (i % 4))) & 0xFF));
  return std::string(out, 32);
}
}  // namespace detail

// ---------------------------------------------------------------- puerto serie
class Serie {
 public:
  Serie() = default;
  explicit Serie(const std::string& puerto, int baud = 115200) { abrir(puerto, baud); }
  ~Serie() { cerrar(); }
  Serie(const Serie&) = delete;
  Serie& operator=(const Serie&) = delete;

  void abrir(const std::string& puerto, int baud = 115200) {
    cerrar();
    nombre_ = puerto;
#ifdef _WIN32
    std::string dev = puerto.rfind("\\\\.\\", 0) == 0 ? puerto : "\\\\.\\" + puerto;
    h_ = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h_ == INVALID_HANDLE_VALUE)
      throw Error("no se puede abrir " + puerto + " (ocupado por otro programa o sin driver)");
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h_, &dcb);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(h_, &dcb);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;   // vuelve con lo que haya o tras ReadTotalTimeoutConstant
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 20;
    to.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(h_, &to);
#else
    fd_ = ::open(puerto.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0)
      throw Error("no se puede abrir " + puerto + ": " + std::strerror(errno) +
                  (errno == EACCES ? " (en Linux, anade tu usuario al grupo dialout)" : ""));
    ioctl(fd_, TIOCEXCL);
    termios t{};
    tcgetattr(fd_, &t);
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd_, TCSANOW, &t);
#endif
    velocidad(baud);
  }

  void cerrar() {
#ifdef _WIN32
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
#else
    if (fd_ >= 0) {
      ioctl(fd_, TIOCNXCL);   // el modo exclusivo dura mientras haya alguien con el puerto abierto
      ::close(fd_);
    }
    fd_ = -1;
#endif
  }

  bool abierto() const {
#ifdef _WIN32
    return h_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
  }

  const std::string& nombre() const { return nombre_; }
  int baudios() const { return baud_; }

  void velocidad(int baud) {
    baud_ = baud;
#ifdef _WIN32
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h_, &dcb);
    dcb.BaudRate = (DWORD)baud;
    if (!SetCommState(h_, &dcb)) throw Error("el puerto no admite " + std::to_string(baud) + " baudios");
#else
    termios t{};
    tcgetattr(fd_, &t);
    speed_t sp = 0;
    switch (baud) {
      case 9600: sp = B9600; break;
      case 19200: sp = B19200; break;
      case 38400: sp = B38400; break;
      case 57600: sp = B57600; break;
      case 74880: sp = 0; break;
      case 115200: sp = B115200; break;
      case 230400: sp = B230400; break;
#ifdef B460800
      case 460800: sp = B460800; break;
#endif
#ifdef B921600
      case 921600: sp = B921600; break;
#endif
#ifdef B1500000
      case 1500000: sp = B1500000; break;
#endif
      default: sp = 0;
    }
#ifdef __APPLE__
    if (!sp) {   // macOS: cualquier velocidad con IOSSIOSPEED, despues de tcsetattr
      cfsetspeed(&t, B115200);
      tcsetattr(fd_, TCSANOW, &t);
      speed_t s = (speed_t)baud;
      if (ioctl(fd_, IOSSIOSPEED, &s) < 0) throw Error("el puerto no admite " + std::to_string(baud) + " baudios");
      return;
    }
#endif
    if (!sp) throw Error("velocidad no admitida en este sistema: " + std::to_string(baud));
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    tcsetattr(fd_, TCSANOW, &t);
#endif
  }

  // DTR y RTS a la vez (el circuito de reinicio de las placas usa los dos)
  void lineas(bool dtr, bool rts) {
#ifdef _WIN32
    EscapeCommFunction(h_, dtr ? SETDTR : CLRDTR);
    EscapeCommFunction(h_, rts ? SETRTS : CLRRTS);
    EscapeCommFunction(h_, dtr ? SETDTR : CLRDTR);   // Windows solo aplica DTR al cambiar RTS en algunos drivers
#else
    int st = 0;
    if (ioctl(fd_, TIOCMGET, &st) < 0) return;   // un pty o un adaptador sin lineas de control
    st = dtr ? (st | TIOCM_DTR) : (st & ~TIOCM_DTR);
    st = rts ? (st | TIOCM_RTS) : (st & ~TIOCM_RTS);
    ioctl(fd_, TIOCMSET, &st);
#endif
  }

  void escribir(const uint8_t* p, size_t n) {
#ifdef _WIN32
    while (n) {
      DWORD w = 0;
      if (!WriteFile(h_, p, (DWORD)n, &w, nullptr) || w == 0) throw Error("se perdio el puerto " + nombre_);
      p += w, n -= w;
    }
#else
    double fin = detail::ahora() + 5;
    while (n) {
      ssize_t w = ::write(fd_, p, n);
      if (w > 0) {
        p += w, n -= (size_t)w;
        continue;
      }
      if (w < 0 && errno != EAGAIN && errno != EINTR) throw Error("se perdio el puerto " + nombre_ + ": " + std::strerror(errno));
      if (detail::ahora() > fin) throw Error("el puerto " + nombre_ + " no deja escribir");
      pollfd pf{fd_, POLLOUT, 0};
      poll(&pf, 1, 50);
    }
#endif
  }
  void escribir(const std::string& s) { escribir((const uint8_t*)s.data(), s.size()); }

  // lee lo que llegue, esperando como mucho `espera` segundos al primer byte
  size_t leer(uint8_t* buf, size_t max, double espera) {
#ifdef _WIN32
    double fin = detail::ahora() + espera;
    do {
      DWORD n = 0;
      if (!ReadFile(h_, buf, (DWORD)max, &n, nullptr)) throw Error("se perdio el puerto " + nombre_);
      if (n) return n;
    } while (detail::ahora() < fin);
    return 0;
#else
    pollfd pf{fd_, POLLIN, 0};
    int r = poll(&pf, 1, (int)(espera * 1000));
    if (r <= 0) return 0;
    if (pf.revents & (POLLERR | POLLNVAL)) throw Error("se perdio el puerto " + nombre_);
    ssize_t n = ::read(fd_, buf, max);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n <= 0) {
      if (pf.revents & POLLHUP) detail::dormir(0.02);   // pty sin el otro extremo
      return 0;
    }
    return (size_t)n;
#endif
  }

  std::string leer_linea(double espera) {   // hasta \n o `espera` segundos
    std::string s;
    double fin = detail::ahora() + espera;
    while (detail::ahora() < fin) {
      uint8_t c;
      if (leer(&c, 1, fin - detail::ahora()) == 1) {
        s.push_back((char)c);
        if (c == '\n') break;
      }
    }
    return s;
  }

  void vaciar_entrada() {
#ifdef _WIN32
    PurgeComm(h_, PURGE_RXCLEAR);
#else
    tcflush(fd_, TCIFLUSH);
#endif
  }

 private:
  std::string nombre_;
  int baud_ = 115200;
#ifdef _WIN32
  HANDLE h_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

// ---------------------------------------------------------------- puertos conectados
struct Puerto {
  std::string puerto, descripcion;
  std::string tipo;   // "esp" (adaptador serie de Espressif, CP210x, CH340, FTDI), "pico", "pico-bootsel" o ""
  int vid = 0, pid = 0;
};

namespace detail {
inline std::string tipo_de(int vid) {
  if (vid == 0x2E8A) return "pico";
  return vid == 0x303A || vid == 0x10C4 || vid == 0x1A86 || vid == 0x0403 ? "esp" : "";
}
#ifndef _WIN32
inline std::string leer_fichero(const std::string& p) {
  std::ifstream f(p);
  std::string s;
  std::getline(f, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}
inline std::vector<std::string> listar(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d))
      if (e->d_name[0] != '.') out.push_back(e->d_name);
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}
inline bool existe(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}
#else
inline std::string narrow(const wchar_t* w, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && w[i]; i++) s.push_back(w[i] < 128 ? (char)w[i] : '?');
  return s;
}
inline std::string reg_str(HKEY k, const wchar_t* name) {
  wchar_t buf[512];
  DWORD sz = sizeof buf, type = 0;
  if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS || type != REG_SZ) return "";
  return narrow(buf, sz / sizeof(wchar_t));
}
#endif
}  // namespace detail

// Unidades RPI-RP2 montadas (Pico conectado con BOOTSEL pulsado)
inline std::vector<std::string> unidades_pico() {
  std::vector<std::string> out;
#ifdef _WIN32
  for (char c = 'D'; c <= 'Z'; c++) {
    std::string r = std::string(1, c) + ":\\";
    if (GetFileAttributesA((r + "INFO_UF2.TXT").c_str()) != INVALID_FILE_ATTRIBUTES) out.push_back(r);
  }
#else
  const char* u = getenv("USER");
  std::string user = u ? u : "";
  for (std::string base : {std::string("/media"), "/media/" + user, "/run/media/" + user, std::string("/Volumes"),
                           std::string("/mnt"), std::string("/storage")})
    for (auto& n : detail::listar(base)) {
      std::string p = base + "/" + n;
      if (detail::existe(p + "/INFO_UF2.TXT") && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
#endif
  return out;
}

inline std::vector<Puerto> puertos() {
  std::vector<Puerto> out;
#if defined(_WIN32)
  std::vector<std::pair<std::string, std::pair<int, int>>> ids;   // COMx -> (vid, pid)
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
        std::string port = detail::reg_str(par, L"PortName");
        if (!port.empty()) ids.push_back({port, {(int)vid, (int)pid}});
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
      Puerto p;
      p.puerto = detail::narrow(data, dn / sizeof(wchar_t));
      p.descripcion = detail::narrow(vname, vn);
      for (auto& e : ids)
        if (e.first == p.puerto) p.vid = e.second.first, p.pid = e.second.second;
      p.tipo = detail::tipo_de(p.vid);
      out.push_back(p);
    }
    RegCloseKey(sc);
  }
#elif defined(__APPLE__)
  for (auto& n : detail::listar("/dev")) {
    if (n.rfind("cu.", 0) != 0 || n.find("Bluetooth") != std::string::npos || n.find("debug") != std::string::npos) continue;
    Puerto p;
    p.puerto = "/dev/" + n;
    p.descripcion = n;
    bool esp = n.find("usbserial") != std::string::npos || n.find("SLAB") != std::string::npos ||
               n.find("wchusbserial") != std::string::npos || n.find("usbmodem") != std::string::npos;
    p.tipo = esp ? "esp" : "";
    out.push_back(p);
  }
#else
  for (auto& n : detail::listar("/sys/class/tty")) {
    std::string dev = "/sys/class/tty/" + n + "/device";
    if (!detail::existe(dev)) continue;
    char real[4096];
    if (!realpath(dev.c_str(), real)) continue;
    Puerto p;
    for (std::string d = real; d.size() > 1; d = d.substr(0, d.rfind('/'))) {   // sube hasta el dispositivo USB
      if (detail::existe(d + "/idVendor")) {
        p.vid = (int)std::strtol(detail::leer_fichero(d + "/idVendor").c_str(), nullptr, 16);
        p.pid = (int)std::strtol(detail::leer_fichero(d + "/idProduct").c_str(), nullptr, 16);
        p.descripcion = detail::leer_fichero(d + "/product");
        break;
      }
    }
    if (!p.vid && n.rfind("ttyUSB", 0) != 0 && n.rfind("ttyACM", 0) != 0) continue;   // los de la placa base
    p.puerto = "/dev/" + n;
    if (p.descripcion.empty()) p.descripcion = n;
    p.tipo = detail::tipo_de(p.vid);
    out.push_back(p);
  }
#endif
  for (auto& d : unidades_pico()) out.push_back({d, "Pico en modo BOOTSEL (RPI-RP2)", "pico-bootsel", 0x2E8A, 0x0003});
  return out;
}

// El primer puerto de un tipo ("esp" o "pico"); error si no hay ninguno
inline std::string puerto_auto(const std::string& tipo) {
  for (auto& p : puertos())
    if (p.tipo == tipo) return p.puerto;
  throw Error(tipo == "pico" ? "no encuentro el Pico: conectalo por USB (o indica el puerto)"
                             : "no encuentro la placa: conectala por USB (o indica el puerto). Si no aparece, falta el "
                               "driver del adaptador USB (CH340, CP210x o FTDI)");
}

// ---------------------------------------------------------------- chips
struct Chip {
  std::string nombre;        // "esp8266", "esp32", "esp32s2", "esp32s3", "esp32c2", "esp32c3", "esp32c5", "esp32c6", "esp32h2", "esp32p4"
  std::string descripcion;   // "ESP32-C3"
  uint32_t magic = 0;        // registro 0x40001000
  int id = -1;               // chip_id de GET_SECURITY_INFO (-1 si la ROM no lo da)
  bool cambia_baudios = true;   // la ROM acepta CHANGE_BAUDRATE
  bool cifrado_en_begin = true; // FLASH_BEGIN lleva el campo "cifrado" (todos menos ESP32 y ESP8266)
  bool usb_jtag = false;        // conectado por el USB-JTAG-Serial integrado (S3, C3, C6...)
  bool wifi = true;             // H2 y P4 no tienen Wi-Fi
};

namespace detail {
struct ChipDef {
  const char* nombre;
  const char* desc;
  uint32_t magic[4];
  int id;
  bool baud, cifrado, wifi;
};
inline const std::vector<ChipDef>& chips() {
  static const std::vector<ChipDef> t = {
      {"esp8266", "ESP8266", {0xFFF0C101}, -1, false, false, true},
      {"esp32", "ESP32", {0x00F01D83}, -1, true, false, true},
      {"esp32s2", "ESP32-S2", {0x000007C6}, 2, true, true, true},
      {"esp32s3", "ESP32-S3", {0x00000009}, 9, true, true, true},
      {"esp32c3", "ESP32-C3", {0x6921506F, 0x1B31506F, 0x4881606F, 0x4361606F}, 5, true, true, true},
      // la ROM del C2 cree que su cristal es de 40 MHz y con el de 26 MHz cambiaria a otra velocidad
      {"esp32c2", "ESP32-C2", {0x6F51306F, 0x7C41A06F}, 12, false, true, true},
      {"esp32c6", "ESP32-C6", {0x2CE0806F}, 13, true, true, true},
      {"esp32h2", "ESP32-H2", {0xD7B73E80}, 16, true, true, false},
      {"esp32p4", "ESP32-P4", {0}, 18, true, true, false},
      {"esp32c61", "ESP32-C61", {0}, 20, true, true, true},
      {"esp32c5", "ESP32-C5", {0}, 23, true, true, true},
  };
  return t;
}
inline Chip de_def(const ChipDef& d) {
  Chip c;
  c.nombre = d.nombre;
  c.descripcion = d.desc;
  c.id = d.id;
  c.cambia_baudios = d.baud;
  c.cifrado_en_begin = d.cifrado;
  c.wifi = d.wifi;
  return c;
}
}  // namespace detail

// Lista de chips que reconoce el grabador (para la interfaz)
inline std::vector<Chip> chips_conocidos() {
  std::vector<Chip> out;
  for (auto& d : detail::chips()) out.push_back(detail::de_def(d));
  return out;
}

// ---------------------------------------------------------------- cargador de la ROM
struct Opciones {
  int baudios = 460800;     // para grabar (se conecta siempre a 115200); 0 = no cambiar
  double espera = 20;       // segundos intentando entrar en modo de carga (ESP-01: tiempo para poner GPIO0 a masa)
  bool verificar = true;    // MD5 al final (la ROM del ESP8266 no lo tiene: alli se fia de las sumas de cada bloque)
  bool reiniciar = true;    // arrancar el firmware al terminar
  std::string chip;         // si se indica, falla si el chip conectado es otro ("esp32c3"...)
};

class Grabador {
 public:
  enum : uint8_t {
    FLASH_BEGIN = 0x02, FLASH_DATA = 0x03, FLASH_END = 0x04, SYNC = 0x08, READ_REG = 0x0A,
    SPI_SET_PARAMS = 0x0B, SPI_ATTACH = 0x0D, CHANGE_BAUDRATE = 0x0F, SPI_FLASH_MD5 = 0x13, GET_SECURITY_INFO = 0x14
  };
  static constexpr uint32_t BLOQUE = 0x400;   // lo que escribe la ROM por paquete
  static constexpr uint32_t SECTOR = 0x1000;


  Grabador(const std::string& puerto, Avance avance = nullptr) : avance_(std::move(avance)) {
    s_.abrir(puerto, 115200);
    for (auto& p : puertos())
      if (p.puerto == puerto) vid_ = p.vid, pid_ = p.pid;
  }

  Serie& serie() { return s_; }
  const Chip& chip() const { return chip_; }

  // Reinicia en modo de carga (probando varias secuencias), sincroniza y detecta el chip
  Chip conectar(double espera = 20) {
    bool usb_jtag = vid_ == 0x303A && pid_ == 0x1001;
    std::vector<int> modos = usb_jtag ? std::vector<int>{2, 0, 1} : std::vector<int>{0, 1, 3};
    double fin = detail::ahora() + espera;
    int intento = 0;
    aviso("conectar", 0, "entrando en modo de carga por " + s_.nombre());
    bool avisado = false;
    while (true) {
      int modo = modos[intento % modos.size()];
      reinicio_carga(modo);
      for (int i = 0; i < 4; i++)
        if (sincronizar()) {
          chip_ = detectar();
          chip_.usb_jtag = usb_jtag;
          aviso("detectar", 100, chip_.descripcion + " conectado");
          return chip_;
        }
      intento++;
      if (detail::ahora() > fin) break;
      if (!avisado && intento >= (int)modos.size()) {
        aviso("conectar", 0, "la placa no entra sola en modo de carga: si es un ESP-01 o una placa sin boton, une GPIO0 a "
                             "GND y vuelve a darle corriente; si tiene boton BOOT, mantenlo pulsado y pulsa RESET");
        avisado = true;
      }
    }
    throw Error("no hay respuesta del cargador de la ROM en " + s_.nombre() +
                ": comprueba el cable (que sea de datos), el driver del adaptador USB y, en placas sin reinicio "
                "automatico como el ESP-01, pon GPIO0 a GND al darle corriente");
  }

  // Graba `datos` en `direccion`. Requiere conectar() antes.
  void grabar(std::vector<uint8_t> datos, uint32_t direccion, const Opciones& op = Opciones()) {
    if (chip_.nombre.empty()) throw Error("conecta primero con el chip");
    if (datos.empty()) throw Error("la imagen esta vacia");
    if (!op.chip.empty() && op.chip != chip_.nombre)
      throw Error("este firmware es para " + op.chip + " y el chip conectado es un " + chip_.descripcion);
    while (datos.size() % 4) datos.push_back(0xFF);
    if (chip_.nombre != "esp8266") {
      // la ROM de la familia ESP32 necesita enlazar la flash SPI y saber su tamano antes de grabar
      std::vector<uint8_t> a;
      detail::put32(a, 0);
      detail::put32(a, 0);
      orden(SPI_ATTACH, a);
      std::vector<uint8_t> p;
      for (uint32_t v : {0u, 16u * 1024 * 1024, 64u * 1024, SECTOR, 256u, 0xFFFFu}) detail::put32(p, v);
      orden(SPI_SET_PARAMS, p);
    }
    if (op.baudios && op.baudios != s_.baudios() && chip_.cambia_baudios && !chip_.usb_jtag) cambiar_baudios(op.baudios);

    uint32_t tam = (uint32_t)datos.size();
    uint32_t bloques = (tam + BLOQUE - 1) / BLOQUE;
    uint32_t borrar = chip_.nombre == "esp8266" ? tam_borrado_esp8266(direccion, tam) : tam;
    std::vector<uint8_t> b;
    for (uint32_t v : {borrar, bloques, BLOQUE, direccion}) detail::put32(b, v);
    if (chip_.cifrado_en_begin) detail::put32(b, 0);
    aviso("borrar", 0, "borrando " + std::to_string((tam + 1023) / 1024) + " KB en " + detail::hex32(direccion));
    double t_borrar = std::max(3.0, 30.0 * tam / (1024.0 * 1024.0));   // la ROM borra todo en FLASH_BEGIN
    orden(FLASH_BEGIN, b, t_borrar + 2);
    aviso("borrar", 100, "");

    double t0 = detail::ahora();
    for (uint32_t i = 0; i < bloques; i++) {
      std::vector<uint8_t> blk(datos.begin() + i * BLOQUE, datos.begin() + std::min<size_t>(tam, (i + 1) * BLOQUE));
      blk.resize(BLOQUE, 0xFF);
      std::vector<uint8_t> d;
      detail::put32(d, BLOQUE);
      detail::put32(d, i);
      detail::put32(d, 0);
      detail::put32(d, 0);
      d.insert(d.end(), blk.begin(), blk.end());
      uint8_t ck = 0xEF;
      for (uint8_t x : blk) ck ^= x;
      for (int r = 0;; r++) {
        try {
          orden(FLASH_DATA, d, 5, ck);
          break;
        } catch (const Error&) {
          if (r >= 2) throw;   // reintenta el bloque si se perdio un byte por el camino
          s_.vaciar_entrada();
        }
      }
      int pct = (int)((i + 1) * 100ull / bloques);
      if (pct != ultimo_pct_ || i + 1 == bloques) {
        ultimo_pct_ = pct;
        double kbs = (i + 1) * (BLOQUE / 1024.0) / std::max(0.001, detail::ahora() - t0);
        char txt[96];
        std::snprintf(txt, sizeof txt, "%u de %u KB (%.0f KB/s)", (unsigned)std::min(tam, (i + 1) * BLOQUE) / 1024,
                      (unsigned)tam / 1024, kbs);
        aviso("grabar", pct, txt);
      }
    }

    if (op.verificar && chip_.nombre != "esp8266") {
      aviso("verificar", 0, "comprobando lo grabado (MD5)");
      std::vector<uint8_t> m;
      for (uint32_t v : {direccion, tam, 0u, 0u}) detail::put32(m, v);
      auto r = orden(SPI_FLASH_MD5, m, std::max(3.0, 8.0 * tam / (1024.0 * 1024.0)));
      std::string chip_md5;
      if (r.size() >= 32) chip_md5.assign(r.begin(), r.begin() + 32);
      else if (r.size() >= 16)
        for (int i = 0; i < 16; i++) {
          char h[3];
          std::snprintf(h, 3, "%02x", r[i]);
          chip_md5 += h;
        }
      std::transform(chip_md5.begin(), chip_md5.end(), chip_md5.begin(), [](char c) { return (char)std::tolower(c); });
      std::string esperado = detail::md5_hex(datos.data(), datos.size());
      if (chip_md5 != esperado)
        throw Error("lo grabado no coincide (MD5 " + chip_md5 + ", esperado " + esperado + "): vuelve a intentarlo, "
                    "con otro cable o a menos baudios");
      aviso("verificar", 100, "MD5 correcto");
    }
    if (op.reiniciar) reiniciar();
  }

  // Sale del cargador y arranca el firmware
  void reiniciar() {
    aviso("reiniciar", 0, "arrancando el firmware");
    if (chip_.nombre == "esp8266") {
      // en placas sin RTS (ESP-01) la ROM salta sola al firmware con FLASH_END
      try {
        std::vector<uint8_t> b;
        for (uint32_t v : {0u, 0u, BLOQUE, 0u}) detail::put32(b, v);
        orden(FLASH_BEGIN, b, 2);
        std::vector<uint8_t> e;
        detail::put32(e, 0);
        orden(FLASH_END, e, 1);
      } catch (const Error&) {
      }
    }
    if (s_.baudios() != 115200) {
      try {
        s_.velocidad(115200);
      } catch (const Error&) {
      }
    }
    s_.lineas(false, true);   // EN a masa
    detail::dormir(chip_.usb_jtag ? 0.2 : 0.1);
    s_.lineas(false, false);
    aviso("reiniciar", 100, "");
  }

  // Envia una orden y devuelve los datos de la respuesta (sin los bytes de estado). Error si la ROM la rechaza.
  std::vector<uint8_t> orden(uint8_t op, const std::vector<uint8_t>& datos, double espera = 3, uint32_t suma = 0,
                             uint32_t* valor = nullptr) {
    std::vector<uint8_t> pkt = {0x00, op, (uint8_t)(datos.size() & 0xFF), (uint8_t)(datos.size() >> 8)};
    detail::put32(pkt, suma);
    pkt.insert(pkt.end(), datos.begin(), datos.end());
    enviar_slip(pkt);
    double fin = detail::ahora() + espera;
    std::vector<uint8_t> r;
    while (recibir_slip(r, fin)) {
      if (r.size() < 8 || r[0] != 0x01 || r[1] != op) continue;   // otra respuesta (sobrantes del SYNC)
      size_t n = r[2] | r[3] << 8;
      if (r.size() < 8 + n) continue;
      if (valor) *valor = detail::get32(&r[4]);
      std::vector<uint8_t> d(r.begin() + 8, r.begin() + 8 + n);
      if (estado_ == 0) estado_ = d.size() >= 4 ? 4 : 2;   // ESP8266: 2 bytes de estado; familia ESP32: 4
      if (d.size() < estado_) throw Error("respuesta corta de la ROM a la orden " + detail::hex32(op));
      uint8_t st = d[d.size() - estado_], err = d[d.size() - estado_ + 1];
      if (st != 0) throw Error("la ROM rechazo la orden " + detail::hex32(op) + ": " + error_rom(err));
      d.resize(d.size() - estado_);
      return d;
    }
    throw Error("la ROM no responde a la orden " + detail::hex32(op));
  }

  uint32_t leer_registro(uint32_t dir, double espera = 1) {
    std::vector<uint8_t> d;
    detail::put32(d, dir);
    uint32_t v = 0;
    orden(READ_REG, d, espera, 0, &v);
    return v;
  }

 private:
  Serie s_;
  Avance avance_;
  Chip chip_;
  int vid_ = 0, pid_ = 0;
  size_t estado_ = 0;
  int ultimo_pct_ = -1;

  void aviso(const std::string& fase, int pct, const std::string& txt) {
    if (avance_) avance_(fase, pct, txt);
  }

  static std::string error_rom(uint8_t e) {
    switch (e) {
      case 0x05: return "mensaje no valido";
      case 0x06: return "no se pudo hacer";
      case 0x07: return "suma de comprobacion incorrecta";
      case 0x08: return "error al escribir la flash";
      case 0x09: return "error al leer la flash";
      case 0x0A: return "longitud de lectura no valida";
      case 0x0B: return "error al descomprimir";
      default: return "codigo " + std::to_string(e);
    }
  }

  // modo 0 y 1: reinicio clasico con DTR/RTS (dos tiempos), 2: USB-JTAG-Serial, 3: sin tocar las lineas (ESP-01)
  void reinicio_carga(int modo) {
    s_.vaciar_entrada();
    if (modo == 0 || modo == 1) {
      s_.lineas(false, true);    // IO0 alto, EN bajo: chip parado
      detail::dormir(0.1);
      s_.lineas(true, false);    // IO0 bajo, EN alto: arranca en modo de carga
      detail::dormir(modo == 0 ? 0.05 : 0.55);
      s_.lineas(false, false);   // suelta IO0
    } else if (modo == 2) {
      s_.lineas(false, false);
      detail::dormir(0.1);
      s_.lineas(true, false);
      detail::dormir(0.1);
      s_.lineas(false, true);
      detail::dormir(0.1);
      s_.lineas(false, false);
    } else {
      detail::dormir(0.3);
    }
    // descarta el texto de arranque de la ROM
    uint8_t buf[256];
    double fin = detail::ahora() + 0.05;
    while (s_.leer(buf, sizeof buf, std::max(0.0, fin - detail::ahora())) > 0) {
    }
  }

  bool sincronizar() {
    std::vector<uint8_t> d = {0x07, 0x07, 0x12, 0x20};
    d.insert(d.end(), 32, 0x55);
    try {
      orden(SYNC, d, 0.1);
    } catch (const Error&) {
      return false;
    }
    // la ROM contesta varias veces al SYNC: deja que lleguen y las descarta
    std::vector<uint8_t> r;
    double fin = detail::ahora() + 0.1;
    while (recibir_slip(r, fin)) {
    }
    return true;
  }

  Chip detectar() {
    uint32_t magic = leer_registro(0x40001000);
    for (auto& d : detail::chips())
      for (uint32_t m : d.magic)
        if (m && m == magic) {
          Chip c = detail::de_def(d);
          c.magic = magic;
          return c;
        }
    // chips nuevos (C5, P4, C61...): el numero de chip viene en GET_SECURITY_INFO
    int id = -1;
    try {
      auto r = orden(GET_SECURITY_INFO, {}, 1);
      if (r.size() >= 16) id = (int)detail::get32(&r[12]);
    } catch (const Error&) {
    }
    for (auto& d : detail::chips())
      if (id >= 0 && d.id == id) {
        Chip c = detail::de_def(d);
        c.magic = magic;
        return c;
      }
    throw Error("chip de Espressif desconocido (magic " + detail::hex32(magic) + ", id " + std::to_string(id) + ")");
  }

  void cambiar_baudios(int baud) {
    std::vector<uint8_t> d;
    detail::put32(d, (uint32_t)baud);
    detail::put32(d, 0);
    try {
      orden(CHANGE_BAUDRATE, d, 1);
      s_.velocidad(baud);
      detail::dormir(0.05);
      s_.vaciar_entrada();
      aviso("baudios", 100, "grabando a " + std::to_string(baud) + " baudios");
    } catch (const Error& e) {
      aviso("baudios", 100, std::string("sigo a 115200 baudios (") + e.what() + ")");
      try {
        s_.velocidad(115200);
      } catch (const Error&) {
      }
    }
  }

  // la ROM del ESP8266 borra de mas: esptool corrige el tamano asi (get_erase_size)
  static uint32_t tam_borrado_esp8266(uint32_t offset, uint32_t size) {
    uint32_t por_bloque = 16, sectores = (size + SECTOR - 1) / SECTOR, inicio = offset / SECTOR;
    uint32_t cabeza = por_bloque - (inicio % por_bloque);
    if (sectores < cabeza) cabeza = sectores;
    if (sectores < 2 * cabeza) return (sectores + 1) / 2 * SECTOR;
    return (sectores - cabeza) * SECTOR;
  }

  void enviar_slip(const std::vector<uint8_t>& p) {
    std::vector<uint8_t> o;
    o.reserve(p.size() + 16);
    o.push_back(0xC0);
    for (uint8_t b : p) {
      if (b == 0xC0) o.push_back(0xDB), o.push_back(0xDC);
      else if (b == 0xDB) o.push_back(0xDB), o.push_back(0xDD);
      else o.push_back(b);
    }
    o.push_back(0xC0);
    s_.escribir(o.data(), o.size());
  }

  // un paquete SLIP completo, o false si se acaba el tiempo
  std::vector<uint8_t> pendiente_;
  bool recibir_slip(std::vector<uint8_t>& out, double fin) {
    out.clear();
    bool dentro = false, esc = false;
    size_t i = 0;
    while (true) {
      for (; i < pendiente_.size(); i++) {
        uint8_t b = pendiente_[i];
        if (!dentro) {
          if (b == 0xC0) dentro = true;
          continue;
        }
        if (b == 0xC0) {
          if (out.empty()) continue;   // dos delimitadores seguidos
          pendiente_.erase(pendiente_.begin(), pendiente_.begin() + i + 1);
          return true;
        }
        if (esc) {
          out.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
          esc = false;
        } else if (b == 0xDB) {
          esc = true;
        } else {
          out.push_back(b);
        }
      }
      double queda = fin - detail::ahora();
      if (queda <= 0) {
        // conserva un paquete a medias para la siguiente lectura
        pendiente_.clear();
        if (dentro) {
          pendiente_.push_back(0xC0);
          for (uint8_t b : out) {
            if (b == 0xC0) pendiente_.push_back(0xDB), pendiente_.push_back(0xDC);
            else if (b == 0xDB) pendiente_.push_back(0xDB), pendiente_.push_back(0xDD);
            else pendiente_.push_back(b);
          }
        }
        out.clear();
        return false;
      }
      uint8_t buf[512];
      size_t n = s_.leer(buf, sizeof buf, std::min(queda, 0.05));
      pendiente_.insert(pendiente_.end(), buf, buf + n);
    }
  }
};

// ---------------------------------------------------------------- utilidades de alto nivel
inline std::vector<uint8_t> leer_archivo(const std::string& ruta) {
  std::ifstream f(ruta, std::ios::binary);
  if (!f) throw Error("no se puede leer " + ruta);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Conecta, graba el archivo y reinicia. Devuelve el chip detectado.
inline Chip grabar_archivo(const std::string& puerto, const std::string& archivo, uint32_t direccion = 0,
                           const Opciones& op = Opciones(), Avance avance = nullptr) {
  auto datos = leer_archivo(archivo);
  Grabador g(puerto, avance);
  Chip c = g.conectar(op.espera);
  g.grabar(std::move(datos), direccion, op);
  return c;
}

// Detecta el chip sin grabar nada (y lo deja arrancar de nuevo)
inline Chip detectar_chip(const std::string& puerto, Avance avance = nullptr, double espera = 20) {
  Grabador g(puerto, avance);
  Chip c = g.conectar(espera);
  g.reiniciar();
  return c;
}

// Manda una linea (JSON de configuracion de NexusLED o NexusWatch) y devuelve la primera respuesta que empiece por
// "{". Reintenta durante `espera` segundos porque tras grabar el firmware tarda en arrancar.
inline std::string enviar_linea(const std::string& puerto, std::string linea, double espera = 8, Avance avance = nullptr) {
  if (linea.empty() || linea.back() != '\n') linea += "\n";
  Serie s(puerto, 115200);
  s.lineas(false, false);   // sin dejar el chip en reinicio
  if (avance) avance("configurar", 0, "enviando la configuracion por " + puerto);
  double fin = detail::ahora() + espera;
  while (detail::ahora() < fin) {
    s.escribir(linea);
    double t = detail::ahora() + 1.5;
    while (detail::ahora() < t) {
      std::string r = s.leer_linea(t - detail::ahora());
      while (!r.empty() && (r.back() == '\n' || r.back() == '\r' || r.back() == ' ')) r.pop_back();
      size_t p = r.find_first_not_of(' ');
      if (p != std::string::npos && r[p] == '{') {
        if (avance) avance("configurar", 100, "respuesta: " + r.substr(p));
        return r.substr(p);
      }
    }
  }
  throw Error("el dispositivo no responde por el puerto serie");
}

// Copia el .uf2 a la unidad RPI-RP2 (la primera que haya si `unidad` va vacia). Devuelve la unidad usada.
inline std::string copiar_uf2(const std::string& uf2, std::string unidad = "", Avance avance = nullptr) {
  if (unidad.empty()) {
    auto u = unidades_pico();
    if (u.empty()) throw Error("no veo la unidad RPI-RP2: conecta el Pico W manteniendo pulsado BOOTSEL");
    unidad = u[0];
  }
  auto datos = leer_archivo(uf2);
  std::string nombre = uf2.substr(uf2.find_last_of("/\\") + 1);
  std::string dest = unidad + ((unidad.back() == '/' || unidad.back() == '\\') ? "" : "/") + nombre;
  std::ofstream f(dest, std::ios::binary);
  if (!f) throw Error("no se puede escribir en " + unidad);
  const size_t trozo = 64 * 1024;
  for (size_t off = 0; off < datos.size(); off += trozo) {
    f.write((const char*)datos.data() + off, (std::streamsize)std::min(trozo, datos.size() - off));
    if (!f) throw Error("fallo al copiar a " + unidad + " (el Pico se desconecto?)");
    if (avance) avance("copiar", (int)(std::min(datos.size(), off + trozo) * 100 / datos.size()), nombre + " a " + unidad);
  }
  f.close();
  return unidad;
}

// Escribe /config.json en el Pico por el REPL en bruto de MicroPython y lo reinicia
inline void configurar_pico(const std::string& puerto, const std::string& json, Avance avance = nullptr) {
  std::string lit = "'";
  for (char c : json) lit += c == '\'' ? std::string("\\'") : c == '\\' ? std::string("\\\\") : std::string(1, c);
  lit += "'";
  std::string codigo = "f=open('/config.json','w')\nf.write(" + lit + ")\nf.close()\nprint('NEXUS-OK')\n";
  Serie s(puerto, 115200);
  if (avance) avance("configurar", 0, "escribiendo config.json en el Pico");
  s.escribir("\r\x03\x03");   // interrumpe el programa
  detail::dormir(0.3);
  s.escribir("\r\x01");       // REPL en bruto
  detail::dormir(0.3);
  s.vaciar_entrada();
  s.escribir(codigo + "\x04");
  double fin = detail::ahora() + 6;
  std::string salida;
  while (detail::ahora() < fin && salida.find("NEXUS-OK") == std::string::npos) {
    uint8_t buf[256];
    size_t n = s.leer(buf, sizeof buf, 0.2);
    salida.append((const char*)buf, n);
  }
  s.escribir("\x02");   // REPL normal
  if (salida.find("NEXUS-OK") == std::string::npos)
    throw Error("el Pico no acepto la configuracion: " + (salida.size() > 200 ? salida.substr(salida.size() - 200) : salida));
  s.escribir("import machine; machine.reset()\r");
  if (avance) avance("configurar", 100, "config.json grabado; el Pico se reinicia");
}

// ---------------------------------------------------------------- API corta para Nexus (devices/firmware.cpp)
// Todas lanzan nexusflash::Error (un std::runtime_error) con el motivo en castellano.

// Nombre del chip conectado: "esp8266", "esp32", "esp32c3"...
inline std::string detectar(const std::string& puerto, Avance progreso = nullptr, double espera = 20) {
  return detectar_chip(puerto, progreso, espera).nombre;
}

// Graba `bin` en `offset`. Si `chip` no va vacio, se niega a grabar en otro chip. Verifica y reinicia al final.
inline void grabar(const std::string& puerto, const std::string& chip, const std::string& bin, uint32_t offset = 0,
                   Avance progreso = nullptr, int baudios = 460800) {
  Opciones op;
  op.chip = chip;
  op.baudios = baudios;
  grabar_archivo(puerto, bin, offset, op, progreso);
}

// Manda la configuracion (Wi-Fi, LED...) como una linea JSON y devuelve la respuesta JSON del firmware
inline std::string wifi(const std::string& puerto, const std::string& json, Avance progreso = nullptr, double espera = 10) {
  return enviar_linea(puerto, json, espera, progreso);
}

// Nombre del binario de NexusLED para un chip ("esp32c3" -> "nexusled-esp32c3.bin"); el ESP8266 usa la imagen de
// 1 MB, que vale para el ESP-01S y cualquier placa mayor (la de 4 MB o 512 KB se piden con su modelo)
inline std::string binario_luces(const std::string& modelo) { return "nexusled-" + modelo + ".bin"; }

}  // namespace nexusflash
