// Nexus-Office como servicio del sistema (service.py).
#include "core/service.h"

#include <cstdio>
#include <stdexcept>

#include "core/config.h"
#include "util/fs.h"
#include "util/http.h"
#include "util/proc.h"
#include "util/text.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nx::service {

namespace {

const std::string NAME = "nexus-office";            // systemd / termux
const std::string LABEL = "ai.nexus.office.server";  // launchd
const std::string TASK = "Nexus-Office";            // Programador de tareas de Windows
const std::string MARK = "# nexus-office-servicio"; // linea de crontab

void say(const std::string& msg) {
  std::printf("[Nexus-Office] %s\n", msg.c_str());
  std::fflush(stdout);
}

std::string q(const std::string& s) {
  return s.find_first_of(" \t'\"") != std::string::npos ? "\"" + s + "\"" : s;
}

std::string joinq(const std::vector<std::string>& v, size_t from = 0) {
  std::vector<std::string> out;
  for (size_t i = from; i < v.size(); ++i) out.push_back(q(v[i]));
  return join(out, " ");
}

std::string xml_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

proc::Result sh(const std::vector<std::string>& args, bool check = true, const std::string& input = "") {
  auto r = proc::run(args, 120, {.capture = true, .input = input});
  if (check && !r.ok())
    throw std::runtime_error(args[0] + " fallo: " + trim(r.err.empty() ? r.out : r.err));
  return r;
}

std::string user_name() {
#ifdef _WIN32
  return env("USERNAME", "usuario");
#else
  if (auto* pw = getpwuid(geteuid())) return pw->pw_name;
  return env("USER", "usuario");
#endif
}

// usuario con el que corre el servicio del sistema: el que hizo sudo o el dueno de la carpeta
std::string run_user() {
  std::string su = env("SUDO_USER");
  if (!su.empty() && su != "root") return su;
#ifndef _WIN32
  struct stat st {};
  if (stat(S(paths::root()).c_str(), &st) == 0)
    if (auto* pw = getpwuid(st.st_uid)) return pw->pw_name;
#endif
  return user_name();
}

fs::path home_of(const std::string& user) {
#ifndef _WIN32
  if (auto* pw = getpwnam(user.c_str())) return pw->pw_dir;
#endif
  (void)user;
  return home_dir();
}

// NEXUS_DATA_DIR que vera el servicio ("" = la carpeta data/ portable)
std::string data_dir_for(const std::string& user) {
  std::string d = env("NEXUS_DATA_DIR");
#ifndef _WIN32
  if (!d.empty() && is_admin() && user != "root" && starts_with(d, "/root"))
    d = S(home_of(user) / ".local/share/nexus-office");   // sudo conserva HOME=/root: datos del usuario real
#endif
  return d;
}

fs::path workdir() { return bundle().empty() ? paths::root() : home_dir(); }

std::string systemd_unit(const std::vector<std::string>& cmd, bool system, const std::string& user = "",
                         const std::string& data = "") {
  std::string envs = data.empty() ? "" : "Environment=\"NEXUS_DATA_DIR=" + data + "\"\n";
  std::string who = system && !user.empty() ? "User=" + user + "\n" : "";
  return "[Unit]\nDescription=Nexus-Office (oficina de IA local)\nAfter=network-online.target\n"
         "Wants=network-online.target\n\n[Service]\nType=simple\n" +
         who + "WorkingDirectory=" + S(workdir()) + "\n" + envs + "ExecStart=" + joinq(cmd) +
         "\nRestart=on-failure\nRestartSec=5\nTimeoutStopSec=20\n\n[Install]\nWantedBy=" +
         (system ? "multi-user.target" : "default.target") + "\n";
}

std::string launchd_plist(const std::vector<std::string>& cmd, const std::string& user = "",
                          const std::string& data = "") {
  std::string args;
  for (auto& c : cmd) args += "<string>" + xml_escape(c) + "</string>";
  std::string logs = xml_escape(S(paths::logs_dir() / "servicio.log"));
  std::string envs = data.empty() ? "" : "<key>NEXUS_DATA_DIR</key><string>" + xml_escape(data) + "</string>";
  std::string who = user.empty() ? "" : "<key>UserName</key><string>" + xml_escape(user) + "</string>";
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\"><dict>\n<key>Label</key><string>" +
         LABEL + "</string>\n<key>ProgramArguments</key><array>" + args +
         "</array>\n<key>WorkingDirectory</key><string>" + xml_escape(S(workdir())) +
         "</string>\n<key>EnvironmentVariables</key><dict>" + envs + "</dict>\n" + who +
         "<key>RunAtLoad</key><true/>\n<key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>\n"
         "<key>ThrottleInterval</key><integer>10</integer>\n<key>StandardOutPath</key><string>" +
         logs + "</string>\n<key>StandardErrorPath</key><string>" + logs + "</string>\n</dict></plist>\n";
}

std::string windows_task_xml(const std::vector<std::string>& cmd, bool system) {
  std::string trigger, principal;
  if (system) {
    trigger = "<BootTrigger><Enabled>true</Enabled><Delay>PT30S</Delay></BootTrigger>";
    principal = "<Principal id=\"Author\"><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal>";
  } else {
    std::string dom = env("USERDOMAIN");
    std::string user = dom.empty() ? user_name() : dom + "\\" + user_name();
    trigger = "<LogonTrigger><Enabled>true</Enabled><UserId>" + xml_escape(user) + "</UserId></LogonTrigger>";
    principal = "<Principal id=\"Author\"><UserId>" + xml_escape(user) +
                "</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal>";
  }
  std::string root = xml_escape(S(paths::root()));
  return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
         "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
         "  <RegistrationInfo><Description>Nexus-Office: oficina de IA local (" +
         root + ")</Description></RegistrationInfo>\n  <Triggers>" + trigger + "</Triggers>\n  <Principals>" +
         principal +
         "</Principals>\n  <Settings>\n"
         "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
         "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
         "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
         "    <AllowHardTerminate>true</AllowHardTerminate>\n"
         "    <StartWhenAvailable>true</StartWhenAvailable>\n"
         "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
         "    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\n"
         "    <AllowStartOnDemand>true</AllowStartOnDemand>\n    <Enabled>true</Enabled>\n    <Hidden>false</Hidden>\n"
         "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
         "    <Priority>6</Priority>\n"
         "    <RestartOnFailure><Interval>PT1M</Interval><Count>999</Count></RestartOnFailure>\n  </Settings>\n"
         "  <Actions Context=\"Author\">\n    <Exec><Command>" +
         xml_escape(cmd[0]) + "</Command><Arguments>" + xml_escape(joinq(cmd, 1)) + "</Arguments><WorkingDirectory>" +
         root + "</WorkingDirectory></Exec>\n  </Actions>\n</Task>\n";
}

std::string utf16le_bom(const std::string& utf8) {
  std::string out = "\xFF\xFE";
  for (char32_t c : u32(utf8)) {
    auto put = [&](uint16_t u) {
      out += (char)(u & 0xFF);
      out += (char)(u >> 8);
    };
    if (c < 0x10000) {
      put((uint16_t)c);
    } else {
      c -= 0x10000;
      put((uint16_t)(0xD800 + (c >> 10)));
      put((uint16_t)(0xDC00 + (c & 0x3FF)));
    }
  }
  return out;
}

void make_exec(const fs::path& p) {
  std::error_code ec;
  fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                         fs::perms::others_exec, ec);
}

fs::path termux_prefix() { return P(env("PREFIX", "/data/data/com.termux/files/usr")); }

std::string getuid_str() {
#ifdef _WIN32
  return "0";
#else
  return std::to_string(getuid());
#endif
}

void launchd_unload(const fs::path& dest, bool system) {
  if (!path_exists(dest)) return;
  std::string domain = system ? "system" : "gui/" + getuid_str();
  if (!sh({"launchctl", "bootout", domain + "/" + LABEL}, false).ok())
    sh({"launchctl", "unload", "-w", S(dest)}, false);
}

}  // namespace

std::string kind() {
  if (!env("TERMUX_VERSION").empty() || is_dir("/data/data/com.termux")) return "termux";
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "launchd";
#else
  return is_dir("/run/systemd/system") && !proc::which("systemctl").empty() ? "systemd" : "cron";
#endif
}

bool is_admin() {
#ifdef _WIN32
  return IsUserAnAdmin() != FALSE;
#else
  return geteuid() == 0;
#endif
}

std::string bundle() {
  if (!env("APPIMAGE").empty()) return "appimage";
  if (!env("FLATPAK_ID").empty()) return "flatpak";
  return "";
}

std::vector<std::string> command(const std::string& host, int port) {
  std::vector<std::string> tail = {"run", "--no-browser", "--host", host, "--port", std::to_string(port)};
  std::vector<std::string> cmd;
  if (bundle() == "appimage")   // el AppImage se monta en una ruta temporal: se arranca el fichero
    cmd = {env("APPIMAGE")};
  else if (bundle() == "flatpak")
    cmd = {"/usr/bin/flatpak", "run", env("FLATPAK_ID")};
  else {
    fs::path exe = paths::exe_path();
#ifdef _WIN32
    // en Windows el servicio usa la variante sin ventana de consola (nexus-office-w.exe) si esta al lado
    fs::path w = exe.parent_path() / "nexus-office-w.exe";
    if (fs::exists(w)) exe = w;
#endif
    cmd = {S(exe)};
  }
  cmd.insert(cmd.end(), tail.begin(), tail.end());
  return cmd;
}

std::string install(const std::string& host, int port) {
  std::string k = kind();
  auto cmd = command(host, port);
  if (bundle() == "flatpak") {   // desde el sandbox no se puede llamar a systemctl: se deja la unidad lista
    fs::path dest = home_dir() / ".config/systemd/user" / (NAME + ".service");
    mkdirs(dest.parent_path());
    write_file(dest, systemd_unit(cmd, false));
    say("Activalo en una terminal del sistema con: systemctl --user enable --now nexus-office  "
        "(y loginctl enable-linger para que arranque sin iniciar sesion)");
    return "unidad systemd de usuario (Flatpak): " + S(dest);
  }
  if (k == "systemd") {
    bool system = is_admin();
    std::string user = system ? run_user() : "";
    std::string unit = systemd_unit(cmd, system, user, system ? data_dir_for(user) : env("NEXUS_DATA_DIR"));
    fs::path dest = system ? fs::path("/etc/systemd/system") / (NAME + ".service")
                           : home_dir() / ".config/systemd/user" / (NAME + ".service");
    std::vector<std::string> ctl = system ? std::vector<std::string>{"systemctl"}
                                          : std::vector<std::string>{"systemctl", "--user"};
    mkdirs(dest.parent_path());
    if (!write_file(dest, unit)) throw std::runtime_error("no se pudo escribir " + S(dest));
    auto c1 = ctl;
    c1.push_back("daemon-reload");
    sh(c1);
    auto c2 = ctl;
    c2.insert(c2.end(), {"enable", "--now", NAME + ".service"});
    sh(c2);
    if (!system && !sh({"loginctl", "enable-linger", user_name()}, false).ok())
      say("Aviso: sin 'loginctl enable-linger' Nexus-Office arrancara al iniciar sesion, no al encender. "
          "Para que arranque con el equipo: sudo ./nexus-office servicio instalar");
    return std::string("systemd ") + (system ? "del sistema (usuario " + user + ")" : "de usuario") + ": " + S(dest);
  }
  if (k == "cron") {
    if (proc::which("crontab").empty())
      throw std::runtime_error("este sistema no tiene systemd ni cron: arranca Nexus-Office con ./nexus-office run --no-browser");
    std::string line = "cd " + q(S(workdir())) + " && " + joinq(cmd) + " >> " + q(S(paths::logs_dir() / "servicio.log")) +
                       " 2>&1 " + MARK;
    std::string cur = sh({"crontab", "-l"}, false).out;
    std::vector<std::string> lines;
    for (auto& ln : splitlines(cur))
      if (!contains(ln, MARK)) lines.push_back(ln);
    lines.push_back("@reboot " + line);
    sh({"crontab", "-"}, true, join(lines, "\n") + "\n");
    proc::Process p;
    p.start(cmd, {.cwd = workdir(), .detached = true});
    return "cron @reboot (sin systemd)";
  }
  if (k == "launchd") {
    bool system = is_admin();
    std::string user = system ? run_user() : "";
    fs::path dest = (system ? fs::path("/Library/LaunchDaemons") : home_dir() / "Library/LaunchAgents") / (LABEL + ".plist");
    mkdirs(dest.parent_path());
    launchd_unload(dest, system);
    write_file(dest, launchd_plist(cmd, user, system ? data_dir_for(user) : env("NEXUS_DATA_DIR")));
    if (system) {
      std::error_code ec;
      fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read, ec);
    }
    std::string domain = system ? "system" : "gui/" + getuid_str();
    if (!sh({"launchctl", "bootstrap", domain, S(dest)}, false).ok()) sh({"launchctl", "load", "-w", S(dest)}, false);
    return std::string("launchd ") + (system ? "LaunchDaemon (arranca con el equipo)" : "LaunchAgent (al iniciar sesion)") +
           ": " + S(dest);
  }
  if (k == "windows") {
    bool system = is_admin();
    fs::path tmp = paths::sub({"cache"}) / "nexus-office-tarea.xml";
    write_file(tmp, utf16le_bom(windows_task_xml(cmd, system)));
    auto r = sh({"schtasks", "/Create", "/TN", TASK, "/XML", S(tmp), "/F"}, false);
    remove_file(tmp);
    if (!r.ok()) throw std::runtime_error(trim(r.out + r.err).empty() ? "schtasks fallo" : trim(r.out + r.err));
    sh({"schtasks", "/Run", "/TN", TASK}, false);
    if (system && host != "127.0.0.1" && host != "localhost") {
      sh({"netsh", "advfirewall", "firewall", "delete", "rule", "name=" + TASK}, false);
      sh({"netsh", "advfirewall", "firewall", "add", "rule", "name=" + TASK, "dir=in", "action=allow", "protocol=TCP",
          "localport=" + std::to_string(port)},
         false);
    }
    return system ? "tarea programada al iniciar el equipo (SYSTEM)"
                  : "tarea programada al iniciar sesion (ejecuta como administrador para que arranque con el equipo)";
  }
  // termux
  fs::path svdir = termux_prefix() / "var/service" / NAME;
  mkdirs(svdir / "log");
  write_file(svdir / "run", "#!/data/data/com.termux/files/usr/bin/sh\nexec 2>&1\ncd " + q(S(paths::root())) + "\nexec " +
                                joinq(cmd) + "\n");
  write_file(svdir / "log/run",
             "#!/data/data/com.termux/files/usr/bin/sh\nexec svlogd -tt " + q(S(paths::logs_dir())) + "\n");
  make_exec(svdir / "run");
  make_exec(svdir / "log/run");
  fs::path boot = home_dir() / ".termux/boot";
  mkdirs(boot);
  write_file(boot / (NAME + ".sh"), "#!/data/data/com.termux/files/usr/bin/sh\ntermux-wake-lock\n"
                                    ". $PREFIX/etc/profile.d/start-services.sh 2>/dev/null\nsv up " + NAME + "\n");
  make_exec(boot / (NAME + ".sh"));
  if (!proc::which("sv-enable").empty()) {
    sh({"sv-enable", NAME}, false);
    sh({"sv", "up", NAME}, false);
    return "termux-services (+ Termux:Boot para arrancar al encender el movil)";
  }
  say("Falta termux-services: pkg install termux-services y reinicia Termux; luego: sv-enable nexus-office");
  return "termux-services preparado (pendiente: pkg install termux-services)";
}

std::vector<std::string> uninstall() {
  std::string k = kind();
  std::vector<std::string> done;
  if (bundle() == "flatpak") {
    fs::path dest = home_dir() / ".config/systemd/user" / (NAME + ".service");
    if (path_exists(dest)) {
      remove_file(dest);
      done.push_back(S(dest));
      say("Desactivalo tambien en el sistema: systemctl --user disable --now nexus-office");
    }
    return done;
  }
  if (k == "systemd") {
    std::vector<std::pair<std::vector<std::string>, fs::path>> units = {
        {{"systemctl"}, fs::path("/etc/systemd/system") / (NAME + ".service")},
        {{"systemctl", "--user"}, home_dir() / ".config/systemd/user" / (NAME + ".service")}};
    for (auto& [ctl, dest] : units) {
      if (!path_exists(dest)) continue;
      if (ctl.size() == 1 && !is_admin())
        throw std::runtime_error("el servicio del sistema (" + S(dest) + ") se quita con sudo");
      auto c = ctl;
      c.insert(c.end(), {"disable", "--now", NAME + ".service"});
      sh(c, false);
      remove_file(dest);
      auto r = ctl;
      r.push_back("daemon-reload");
      sh(r, false);
      done.push_back(S(dest));
    }
  } else if (k == "cron" && !proc::which("crontab").empty()) {
    std::string cur = sh({"crontab", "-l"}, false).out;
    if (contains(cur, MARK)) {
      std::vector<std::string> lines;
      for (auto& ln : splitlines(cur))
        if (!contains(ln, MARK)) lines.push_back(ln);
      sh({"crontab", "-"}, true, join(lines, "\n") + "\n");
      done.push_back("crontab @reboot");
    }
  } else if (k == "launchd") {
    std::vector<std::pair<fs::path, bool>> items = {{fs::path("/Library/LaunchDaemons") / (LABEL + ".plist"), true},
                                                    {home_dir() / "Library/LaunchAgents" / (LABEL + ".plist"), false}};
    for (auto& [dest, system] : items) {
      if (!path_exists(dest)) continue;
      launchd_unload(dest, system);
      remove_file(dest);
      done.push_back(S(dest));
    }
  } else if (k == "windows") {
    if (sh({"schtasks", "/Query", "/TN", TASK}, false).ok()) {
      sh({"schtasks", "/End", "/TN", TASK}, false);
      if (!sh({"schtasks", "/Delete", "/TN", TASK, "/F"}, false).ok())
        throw std::runtime_error("no se pudo borrar la tarea (si se creo como administrador, quitala como administrador)");
      done.push_back("tarea '" + TASK + "'");
    }
    sh({"netsh", "advfirewall", "firewall", "delete", "rule", "name=" + TASK}, false);
  } else {
    if (!proc::which("sv-disable").empty()) {
      sh({"sv", "down", NAME}, false);
      sh({"sv-disable", NAME}, false);
    }
    for (auto& p : {termux_prefix() / "var/service" / NAME, home_dir() / ".termux/boot" / (NAME + ".sh")}) {
      if (!path_exists(p)) continue;
      rm_rf(p);
      done.push_back(S(p));
    }
  }
  return done;
}

std::string registered() {
  std::string k = kind();
  if (k == "systemd" || bundle() == "flatpak") {
    for (auto& p : {fs::path("/etc/systemd/system") / (NAME + ".service"), home_dir() / ".config/systemd/user" / (NAME + ".service")})
      if (path_exists(p)) return S(p);
  } else if (k == "cron") {
    if (!proc::which("crontab").empty() && contains(sh({"crontab", "-l"}, false).out, MARK)) return "crontab @reboot";
  } else if (k == "launchd") {
    for (auto& p : {fs::path("/Library/LaunchDaemons") / (LABEL + ".plist"), home_dir() / "Library/LaunchAgents" / (LABEL + ".plist")})
      if (path_exists(p)) return S(p);
  } else if (k == "windows") {
    if (sh({"schtasks", "/Query", "/TN", TASK}, false).ok()) return "tarea '" + TASK + "'";
  } else {
    fs::path p = termux_prefix() / "var/service" / NAME;
    if (path_exists(p)) return S(p);
  }
  return "";
}

bool running(int port, double timeout) {
  auto r = http::get("http://127.0.0.1:" + std::to_string(port) + "/api/ping", {}, timeout);
  return r.status == 200;
}

int main(const std::vector<std::string>& args) {
  const char* usage =
      "uso: nexus-office servicio {instalar,quitar,estado} [--red] [--host HOST] [--puerto P]\n"
      "  --red      escucha en la red local (0.0.0.0) para entrar desde moviles y otros PC\n";
  std::string action, host_arg;
  bool red = false;
  int port_arg = 0;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--red") red = true;
    else if (a == "--host" && i + 1 < args.size()) host_arg = args[++i];
    else if ((a == "--puerto" || a == "--port") && i + 1 < args.size()) port_arg = std::atoi(args[++i].c_str());
    else if (a == "-h" || a == "--help") {
      std::printf("%s", usage);
      return 0;
    } else if (action.empty()) action = a;
  }
  if (action != "instalar" && action != "quitar" && action != "estado") {
    std::fprintf(stderr, "%s", usage);
    return 2;
  }
  Config& cfg = get_config();
  int port = port_arg ? port_arg : (int)cfg.num("server.port", 8775);
  std::string host = red ? "0.0.0.0" : (!host_arg.empty() ? host_arg : cfg.str("server.host", "127.0.0.1"));
  try {
    if (action == "instalar") {
      say("Servicio instalado: " + install(host, port));
      say("Nexus-Office arrancara solo; abre http://127.0.0.1:" + std::to_string(port) +
          (host == "0.0.0.0" ? " (o http://<ip-del-equipo>:" + std::to_string(port) + " desde la red)" : ""));
    } else if (action == "quitar") {
      auto done = uninstall();
      say(done.empty() ? "No habia servicio instalado" : "Servicio quitado: " + join(done, ", "));
    } else {
      std::string reg = registered();
      say("Tipo: " + kind() + " | registrado: " + (reg.empty() ? "no" : reg) + " | en marcha: " +
          (running(port) ? "si" : "no") + " (puerto " + std::to_string(port) + ")");
    }
  } catch (const std::exception& e) {
    say(std::string("Error: ") + e.what());
    return 1;
  }
  return 0;
}

}  // namespace nx::service
