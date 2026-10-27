// Drivers USB-serie (ver drivers.h): deteccion por VID:PID e instalacion sin preguntar en cada sistema.
#include "devices/drivers.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "devices/firmware.h"
#include "util/log.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::devices::drivers {

namespace {

auto L = log::get("drivers");

const std::vector<Puente> PUENTES = {
    {0x1A86, 0x7523, "CH340", "ch34x"},       {0x1A86, 0x7522, "CH340K", "ch34x"},
    {0x1A86, 0x5523, "CH341", "ch34x"},       {0x1A86, 0x55D4, "CH9102", "ch343"},
    {0x1A86, 0x55D3, "CH343", "ch343"},       {0x1A86, 0x55D2, "CH342", "ch343"},
    {0x10C4, 0xEA60, "CP210x", "cp210x"},     {0x10C4, 0xEA70, "CP2105", "cp210x"},
    {0x10C4, 0xEA71, "CP2108", "cp210x"},     {0x0403, 0x6001, "FT232R", "ftdi"},
    {0x0403, 0x6010, "FT2232", "ftdi"},       {0x0403, 0x6011, "FT4232", "ftdi"},
    {0x0403, 0x6014, "FT232H", "ftdi"},       {0x0403, 0x6015, "FT231X", "ftdi"},
    {0x303A, 0x1001, "USB nativo de Espressif (S3/C3/C5/C6/H2/P4)", "cdc"},
    {0x303A, 0x0002, "USB nativo del ESP32-S2", "cdc"},
    {0x303A, 0x4001, "USB nativo de Espressif (TinyUSB)", "cdc"},
    {0x2E8A, 0x0005, "Raspberry Pi Pico W (MicroPython)", "cdc"},
};

const std::map<std::string, std::string> WEB = {
    {"ch34x", "https://www.wch-ic.com/downloads/CH341SER_EXE.html"},
    {"ch343", "https://www.wch-ic.com/downloads/CH343SER_EXE.html"},
    {"cp210x", "https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers"},
    {"ftdi", "https://ftdichip.com/drivers/vcp-drivers/"},
};

std::mutex mu;
fs::path sysfs_root = "/sys";

std::string hex4(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%04x", v);
  return b;
}

std::string sistema() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return sys::is_android() ? "android" : "linux";
#endif
}

json fila(const Puente& p, const std::string& puerto, bool instalado, const std::string& problema) {
  return {{"vid", hex4(p.vid)}, {"pid", hex4(p.pid)}, {"chip", p.chip},     {"driver", p.driver},
          {"puerto", puerto},   {"instalado", instalado}, {"problema", problema}};
}

// ---------------------------------------------------------------- Linux
#if !defined(_WIN32) && !defined(__APPLE__)
fs::path dev_root() { return sysfs_root == "/sys" ? fs::path("/dev") : sysfs_root / "dev"; }

std::string enlace(const fs::path& p) {
  std::error_code ec;
  fs::path t = fs::read_symlink(p, ec);
  return ec ? "" : S(t.filename());
}

bool brltty_instalado() {
  for (auto p : {"/usr/bin/brltty", "/bin/brltty", "/usr/lib/udev/rules.d/85-brltty.rules", "/lib/udev/rules.d/85-brltty.rules"})
    if (path_exists(sysfs_root == "/sys" ? fs::path(p) : sysfs_root / fs::path(p).relative_path())) return true;
  return false;
}

json linux_dispositivos() {
  json out = json::array();
  std::error_code ec;
  fs::path usb = sysfs_root / "bus" / "usb" / "devices";
  if (!fs::is_directory(usb, ec)) return out;
  for (auto& e : fs::directory_iterator(usb, ec)) {
    fs::path d = e.path();
    if (!is_file(d / "idVendor")) continue;
    int vid = (int)std::strtol(trim(read_text(d / "idVendor")).c_str(), nullptr, 16);
    int pid = (int)std::strtol(trim(read_text(d / "idProduct")).c_str(), nullptr, 16);
    const Puente* p = puente(vid, pid);
    if (!p) continue;
    std::string base = S(d.filename()), drv, tty;
    for (auto& i : fs::directory_iterator(d, ec)) {   // interfaces: "1-2:1.0"
      std::string n = S(i.path().filename());
      if (!starts_with(n, base + ":")) continue;
      std::string di = enlace(i.path() / "driver");
      if (!di.empty() && (drv.empty() || drv == "usbfs")) drv = di;
      for (auto& t : fs::directory_iterator(i.path(), ec)) {
        std::string tn = S(t.path().filename());
        if (starts_with(tn, "ttyUSB") || starts_with(tn, "ttyACM")) tty = tn;
        if (tn == "tty")
          for (auto& tt : fs::directory_iterator(t.path(), ec)) tty = S(tt.path().filename());
      }
    }
    std::string problema;
    bool ok = !drv.empty() && drv != "usbfs";
    if (drv == "usbfs") problema = brltty_instalado() ? "brltty" : "ocupado";
    else if (drv.empty()) problema = "sin_modulo";
    else if (!tty.empty()) {
      fs::path node = dev_root() / tty;
      if (path_exists(node) && ::access(node.c_str(), R_OK | W_OK) != 0) problema = "permiso";
    }
    out.push_back(fila(*p, tty.empty() ? "" : "/dev/" + tty, ok, problema));
  }
  return out;
}

std::string usuario() {
  std::string u = env("SUDO_USER");
  if (u.empty()) u = env("USER");
  if (u.empty()) u = env("LOGNAME");
  return u;
}

std::string script_linux(const std::set<std::string>& mods, bool brltty) {
  std::string s = "set +e\n";
  for (auto& m : mods) s += "modprobe " + m + "\n";
  if (brltty)   // brltty toma los CH340 como si fueran lineas braille
    s += "systemctl stop brltty-udev.service brltty.service 2>/dev/null; "
         "systemctl mask brltty-udev.service 2>/dev/null; ln -sf /dev/null /etc/udev/rules.d/85-brltty.rules\n";
  s += "cat > /etc/udev/rules.d/60-nexus-office-serie.rules <<'EOF'\n"
       "# Nexus-Office: puertos serie de las placas ESP y el Pico W para el usuario sin sudo\n";
  std::set<int> vids;
  for (auto& p : PUENTES) vids.insert(p.vid);
  for (int v : vids) s += "SUBSYSTEM==\"tty\", ATTRS{idVendor}==\"" + hex4(v) + "\", MODE=\"0666\"\n";
  s += "EOF\n";
  std::string u = usuario();
  if (!u.empty() && u != "root")
    s += "G=$(getent group dialout >/dev/null && echo dialout || echo uucp); usermod -aG \"$G\" " + u + "\n";
  s += "udevadm control --reload-rules 2>/dev/null; udevadm trigger --subsystem-match=tty 2>/dev/null\n";
  return s;
}
#endif

// ---------------------------------------------------------------- Windows
#ifdef _WIN32
std::string narrow(const std::wstring& w) { return S(fs::path(w)); }

std::wstring reg_str(HKEY k, const wchar_t* name) {
  wchar_t buf[512];
  DWORD sz = sizeof buf, type = 0;
  if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS || type != REG_SZ) return L"";
  return std::wstring(buf, wcsnlen(buf, sz / sizeof(wchar_t)));
}

json windows_dispositivos() {
  json out = json::array();
  HKEY usb;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usb) != ERROR_SUCCESS)
    return out;
  wchar_t name[256];
  for (DWORD i = 0;; ++i) {
    DWORD n = 256;
    if (RegEnumKeyExW(usb, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    unsigned vid = 0, pid = 0;
    if (swscanf(name, L"VID_%4x&PID_%4x", &vid, &pid) != 2) continue;
    const Puente* p = puente((int)vid, (int)pid);
    if (!p || (wcsstr(name, L"&MI_") && !wcsstr(name, L"&MI_00"))) continue;   // solo la primera interfaz
    HKEY dev;
    if (RegOpenKeyExW(usb, name, 0, KEY_READ, &dev) != ERROR_SUCCESS) continue;
    wchar_t inst[256];
    for (DWORD j = 0;; ++j) {
      DWORD m = 256;
      if (RegEnumKeyExW(dev, j, inst, &m, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
      std::wstring id = std::wstring(L"USB\\") + name + L"\\" + inst;
      DEVINST dn;
      if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;   // no enchufado
      ULONG st = 0, prob = 0;
      CM_Get_DevNode_Status(&st, &prob, dn, 0);
      HKEY ik;
      std::wstring service, port;
      if (RegOpenKeyExW(dev, inst, 0, KEY_READ, &ik) == ERROR_SUCCESS) {
        service = reg_str(ik, L"Service");
        HKEY par;
        if (RegOpenKeyExW(ik, L"Device Parameters", 0, KEY_READ, &par) == ERROR_SUCCESS) {
          port = reg_str(par, L"PortName");
          RegCloseKey(par);
        }
        RegCloseKey(ik);
      }
      bool ok = !service.empty() && prob == 0;
      std::string problema = ok ? "" : prob == CM_PROB_FAILED_INSTALL || prob == CM_PROB_NOT_CONFIGURED || service.empty()
                                           ? "sin_driver"
                                           : "codigo_" + std::to_string(prob);
      out.push_back(fila(*p, narrow(port), ok, problema));
    }
    RegCloseKey(dev);
  }
  RegCloseKey(usb);
  return out;
}

// pnputil con elevacion (una ventana de UAC si no se es ya administrador); devuelve el codigo de salida
int elevado(const std::wstring& exe, const std::wstring& args, std::string& salida) {
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
  sei.lpVerb = L"runas";
  sei.lpFile = exe.c_str();
  sei.lpParameters = args.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) {
    salida = "no se concedio permiso de administrador";
    return -1;
  }
  WaitForSingleObject(sei.hProcess, 300000);
  DWORD code = 1;
  GetExitCodeProcess(sei.hProcess, &code);
  CloseHandle(sei.hProcess);
  return (int)code;
}
#endif

// ---------------------------------------------------------------- macOS
#ifdef __APPLE__
json macos_dispositivos() {
  json out = json::array();
  auto r = proc::run({"/usr/sbin/ioreg", "-p", "IOUSB", "-l", "-w0"}, 15, [] {
    proc::Options o;
    o.capture = true;
    return o;
  }());
  static const Rx VID(R"("idVendor" = (\d+))"), PID(R"("idProduct" = (\d+))");
  std::vector<std::pair<int, int>> vistos;
  int vid = -1;
  for (auto& line : splitlines(r.out)) {
    if (line.find("+-o ") != std::string::npos) vid = -1;
    if (auto m = VID.search(line)) vid = std::atoi(m->str(1).c_str());
    if (auto m = PID.search(line); m && vid >= 0) {
      vistos.emplace_back(vid, std::atoi(m->str(1).c_str()));
      vid = -1;
    }
  }
  std::vector<std::string> cu;
  std::error_code ec;
  for (auto& e : fs::directory_iterator("/dev", ec)) {
    std::string n = S(e.path().filename());
    if (starts_with(n, "cu.") && (n.find("usb") != std::string::npos || n.find("SLAB") != std::string::npos ||
                                  n.find("wch") != std::string::npos))
      cu.push_back("/dev/" + n);
  }
  for (auto& [v, pi] : vistos) {
    const Puente* p = puente(v, pi);
    if (!p) continue;
    std::string drv = p->driver, puerto;
    for (auto& c : cu) {
      bool modem = c.find("usbmodem") != std::string::npos;
      if ((drv == "cdc") == modem) {
        puerto = c;
        break;
      }
    }
    out.push_back(fila(*p, puerto, !puerto.empty(), puerto.empty() ? "sin_driver" : ""));
  }
  return out;
}
#endif

std::vector<fs::path> paquetes_de(const std::string& driver) {
  std::vector<fs::path> out;
  fs::path d = carpeta();
  if (d.empty()) return out;
  std::error_code ec;
  for (auto& dir : {d / driver, d / (driver == "ch343" ? "ch34x" : driver)})
    if (fs::is_directory(dir, ec)) {
      out.push_back(dir);
      break;
    }
  return out;
}

}  // namespace

const std::vector<Puente>& puentes() { return PUENTES; }

const Puente* puente(int vid, int pid) {
  for (auto& p : PUENTES)
    if (p.vid == vid && p.pid == pid) return &p;
  return nullptr;
}

fs::path carpeta() {
  fs::path fw = firmware::carpeta();
  if (fw.empty()) return {};
  fs::path d = fw / "drivers" / sistema();
  return is_dir(d) ? d : fs::path();
}

json dispositivos() {
  std::lock_guard<std::mutex> lk(mu);
#if defined(_WIN32)
  return windows_dispositivos();
#elif defined(__APPLE__)
  return macos_dispositivos();
#else
  return linux_dispositivos();
#endif
}

json estado() {
  json devs = dispositivos();
  std::set<std::string> faltan;
  json avisos = json::array();
  for (auto& d : devs) {
    std::string pr = jstr(d, "problema"), drv = jstr(d, "driver");
    if (pr.empty()) continue;
    faltan.insert(pr == "permiso" || pr == "brltty" || pr == "ocupado" ? "permisos" : drv);
    if (pr == "brltty") avisos.push_back("brltty se queda con el " + jstr(d, "chip") + ": hay que apartarlo");
    if (pr == "permiso") avisos.push_back("tu usuario no puede abrir " + jstr(d, "puerto"));
  }
  json paq = json::object();
  for (auto& p : PUENTES)
    if (std::string(p.driver) != "cdc") {
      auto v = paquetes_de(p.driver);
      paq[p.driver] = v.empty() ? "" : S(v[0]);
    }
  return {{"sistema", sistema()}, {"dispositivos", devs}, {"faltan", json(std::vector<std::string>(faltan.begin(), faltan.end()))},
          {"paquetes", paq},      {"avisos", avisos}};
}

json instalar(std::vector<std::string> drivers, bool simular) {
  if (drivers.empty()) {
    json st = estado();
    for (auto& f : st["faltan"]) drivers.push_back(f.get<std::string>());
  }
  json hecho = json::array(), pendiente = json::array(), comandos = json::array();
  std::string so = sistema();
  if (drivers.empty())
    return {{"ok", true}, {"hecho", hecho}, {"pendiente", pendiente}, {"comandos", comandos},
            {"mensaje", "no falta ningun driver"}};
  if (so == "android")
    return {{"ok", false}, {"hecho", hecho}, {"pendiente", drivers}, {"comandos", comandos},
            {"mensaje", "en Android los puertos USB-serie los maneja cada app: graba las placas desde un ordenador"}};
#if !defined(_WIN32) && !defined(__APPLE__)
  std::set<std::string> mods;
  bool brl = false;
  for (auto& d : drivers) {
    if (d == "permisos") brl = brl || brltty_instalado();
    else if (auto m = detail::modulo_linux(d); !m.empty()) mods.insert(m);
  }
  std::string script = script_linux(mods, brl);
  std::vector<std::string> cmd;
  if (::geteuid() == 0) cmd = {"sh", "-c", script};
  else if (!proc::which("pkexec").empty() && (!env("DISPLAY").empty() || !env("WAYLAND_DISPLAY").empty()))
    cmd = {"pkexec", "sh", "-c", script};
  else if (!proc::which("sudo").empty() && proc::run({"sudo", "-n", "true"}, 10).ok())
    cmd = {"sudo", "-n", "sh", "-c", script};
  comandos.push_back("sudo sh -c '" + replace_all(script, "'", "'\\''") + "'");
  if (simular || cmd.empty()) {
    for (auto& d : drivers) pendiente.push_back(d);
    return {{"ok", simular}, {"hecho", hecho}, {"pendiente", pendiente}, {"comandos", comandos},
            {"mensaje", simular ? "esto es lo que se haria" : "hace falta ser administrador: ejecuta el comando con sudo"}};
  }
  auto r = proc::run(cmd, 180, [] {
    proc::Options o;
    o.capture = true;
    return o;
  }());
  if (!r.ok()) {
    for (auto& d : drivers) pendiente.push_back(d);
    return {{"ok", false}, {"hecho", hecho}, {"pendiente", pendiente}, {"comandos", comandos},
            {"mensaje", "no se pudo instalar: " + trim(r.err.empty() ? r.out : r.err)}};
  }
  for (auto& d : drivers) hecho.push_back(d);
  L.info("Drivers USB-serie listos en Linux: ", join(drivers, ", "));
  return {{"ok", true}, {"hecho", hecho}, {"pendiente", pendiente}, {"comandos", comandos},
          {"mensaje", "listo: desenchufa y vuelve a enchufar la placa"}};
#else
  std::string msg;
  for (auto& d : drivers) {
    if (d == "cdc" || d == "permisos") {
      hecho.push_back(d);
      continue;
    }
    auto paq = paquetes_de(d);
    if (paq.empty()) {
      pendiente.push_back(d);
      auto w = WEB.find(d);
      msg += "No viene el driver " + d + " en el paquete" + (w == WEB.end() ? "" : ": descargalo de " + w->second) + ". ";
      continue;
    }
#ifdef _WIN32
    std::wstring args = L"/add-driver \"" + (paq[0] / L"*.inf").wstring() + L"\" /subdirs /install";
    comandos.push_back("pnputil " + narrow(args));
    if (simular) {
      pendiente.push_back(d);
      continue;
    }
    std::string out;
    int code = elevado(L"pnputil.exe", args, out);
    if (code == 0 || code == 3010 || code == 259) {   // 3010: hace falta reiniciar; 259: ya estaba
      hecho.push_back(d);
      if (code == 3010) msg += "Windows pide reiniciar para terminar con " + d + ". ";
    } else {
      pendiente.push_back(d);
      msg += "pnputil fallo con " + d + " (" + std::to_string(code) + ") " + trim(out) + ". ";
    }
#else
    std::vector<fs::path> pkgs = glob(paq[0], "*.pkg");
    if (pkgs.empty()) pkgs = glob(paq[0], "*.dmg");
    if (pkgs.empty()) {
      pendiente.push_back(d);
      continue;
    }
    comandos.push_back("open \"" + S(pkgs[0]) + "\"");
    if (!simular) proc::run({"open", S(pkgs[0])}, 30);
    pendiente.push_back(d);   // el instalador de macOS lo termina el usuario (y aprueba la extension en Ajustes)
    msg += "Abierto el instalador de " + d + ": siguelo y aprueba la extension en Ajustes > Privacidad y seguridad. ";
#endif
  }
  bool ok = pendiente.empty();
  if (msg.empty()) msg = ok ? "listo: desenchufa y vuelve a enchufar la placa" : "esto es lo que se haria";
  return {{"ok", ok || simular}, {"hecho", hecho}, {"pendiente", pendiente}, {"comandos", comandos}, {"mensaje", trim(msg)}};
#endif
}

namespace detail {
void set_sysfs(const fs::path& root) {
  std::lock_guard<std::mutex> lk(mu);
  sysfs_root = root.empty() ? fs::path("/sys") : root;
}
std::string modulo_linux(const std::string& driver) {
  if (driver == "ch34x") return "ch341";
  if (driver == "ch343" || driver == "cdc") return "cdc_acm";   // el CH9102/CH343 lo maneja cdc_acm en Linux
  if (driver == "cp210x") return "cp210x";
  if (driver == "ftdi") return "ftdi_sio";
  return "";
}
}  // namespace detail

}  // namespace nx::devices::drivers
