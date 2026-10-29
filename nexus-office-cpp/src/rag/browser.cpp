#include "rag/browser.h"

#include <condition_variable>
#include <memory>
#include <set>
#include <map>
#include <mutex>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "core/config.h"
#include "util/fs.h"
#include "util/log.h"
#include "util/net.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/sysinfo.h"
#include "util/text.h"

namespace nx::rag::browser {

namespace {

auto L = log::get("navegador");
constexpr double TIMEOUT_S = 40;
const char* const NAMES[] = {"chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "chrome",
                             "microsoft-edge", "microsoft-edge-stable", "msedge", "brave-browser", "brave"};

std::mutex found_mu;
std::map<std::string, std::string> found;
std::set<std::string> broken;   // navegadores que no llegan a leer nada en este equipo (p. ej. un Mac sin pantalla)

// como mucho dos navegadores a la vez (un rastreo pide muchas paginas en paralelo); un perfil por hueco
std::mutex slot_mu;
std::condition_variable slot_cv;
std::vector<int> free_slots = {1, 0};

bool executable(const fs::path& p) {
  if (!is_file(p)) return false;
#ifdef _WIN32
  return true;
#else
  return ::access(p.c_str(), X_OK) == 0;
#endif
}

std::vector<std::string> candidates() {
  std::vector<std::string> out;
#ifdef _WIN32
  for (const char* var : {"PROGRAMFILES(X86)", "PROGRAMFILES", "LOCALAPPDATA"}) {
    std::string base = env(var);
    if (base.empty()) continue;
    for (const char* rel : {"\\Microsoft\\Edge\\Application\\msedge.exe", "\\Google\\Chrome\\Application\\chrome.exe",
                            "\\BraveSoftware\\Brave-Browser\\Application\\brave.exe", "\\Chromium\\Application\\chrome.exe"})
      out.push_back(base + rel);
  }
#elif defined(__APPLE__)
  for (const char* p : {"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
                        "/Applications/Chromium.app/Contents/MacOS/Chromium",
                        "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"})
    out.push_back(p);
#endif
  for (const char* n : NAMES)
    if (auto w = proc::which(n); !w.empty()) out.push_back(w);
  // Chromium de Playwright, si esta instalado (equipos de desarrollo y CI)
  std::vector<fs::path> roots;
  if (auto p = env("PLAYWRIGHT_BROWSERS_PATH"); !p.empty()) roots.push_back(P(p));
  roots.push_back("/opt/pw-browsers");
  roots.push_back(home_dir() / ".cache" / "ms-playwright");
#ifdef _WIN32
  if (auto p = env("LOCALAPPDATA"); !p.empty()) roots.push_back(P(p) / "ms-playwright");
#endif
  auto add = [&](const fs::path& root, const char* dir, const char* sub, std::initializer_list<const char*> names) {
    auto ds = glob(root, dir);
    for (auto it = ds.rbegin(); it != ds.rend(); ++it) {
      auto ss = glob(*it, sub);
      for (auto jt = ss.rbegin(); jt != ss.rend(); ++jt)
        for (const char* n : names)
          if (is_file(*jt / n)) out.push_back(S(*jt / n));
    }
  };
  for (auto& r : roots) {
    if (!is_dir(r)) continue;
    add(r, "chromium_headless_shell-*", "chrome-headless-shell-*", {"chrome-headless-shell", "chrome-headless-shell.exe"});
    add(r, "chromium-*", "chrome-*", {"chrome", "chrome.exe"});
  }
  return out;
}

std::string browser_path() {
  try {
    return get_config().str("rag.browser_path", "");
  } catch (...) {
    return "";
  }
}

bool allow_private_cfg() {
  try {
    return truthy(get_config().get("rag.allow_private_urls", false));
  } catch (...) {
    return false;
  }
}

int words(const std::string& raw) {
  static const Rx BLOCKS(R"((?is)<(script|style|noscript|svg|template|head)\b.*?</\1>)"), TAGS("<[^>]+>");
  std::string body = BLOCKS.sub(raw, " ");
  return (int)split_ws(TAGS.sub(body, " ")).size();
}

// Perfil propio de Nexus (nunca el del usuario), uno por hueco. Se reutiliza porque Chrome tarda mucho (o se queda
// colgado) la primera vez que prepara un perfil nuevo; antes de cada lectura se borran cookies, almacenamiento e
// historial, asi que cada pagina se abre como en una ventana privada.
int take_slot() {
  std::unique_lock<std::mutex> lk(slot_mu);
  slot_cv.wait(lk, [] { return !free_slots.empty(); });
  int s = free_slots.back();
  free_slots.pop_back();
  return s;
}

void release_slot(int s) {
  {
    std::lock_guard<std::mutex> lk(slot_mu);
    free_slots.push_back(s);
  }
  slot_cv.notify_one();
}

fs::path profile(int slot) {
  fs::path prof = paths::sub({"navegador", "perfil" + std::to_string(slot)});
  for (const char* name : {"Cookies", "Cookies-journal", "History", "History-journal", "Local Storage", "Session Storage",
                           "IndexedDB", "Service Worker", "Cache", "Code Cache", "Sessions"})
    rm_rf(prof / "Default" / name);
  return prof;
}

std::string render_now(const std::string& url, const std::string& exe, int budget_ms, bool allow_private) {
  std::unique_ptr<net::GuardProxy> proxy;
  if (!allow_private) proxy = std::make_unique<net::GuardProxy>(TIMEOUT_S);
  int slot = take_slot();
  struct Release {
    int s;
    ~Release() { release_slot(s); }
  } rel{slot};
  fs::path prof = profile(slot);
  bool shell = contains(S(P(exe).filename()), "headless-shell") || contains(exe, "headless_shell");
  std::vector<std::string> cmd = {exe};
  if (!shell) cmd.push_back("--headless=new");
  for (const char* f : {"--disable-gpu", "--no-first-run", "--no-default-browser-check", "--mute-audio",
                        "--disable-extensions", "--disable-background-networking", "--disable-component-update",
                        "--disable-sync", "--hide-scrollbars", "--blink-settings=imagesEnabled=false",
                        // sin llavero del sistema: en macOS Chrome puede quedarse esperando al Llavero sin interfaz
                        "--use-mock-keychain", "--password-store=basic",
                        // sin buscar dispositivos de la red local (macOS pide permiso y Chrome se queda esperando) y
                        // sin frenar la pagina por estar en segundo plano
                        "--disable-features=MediaRouter,DialMediaRouteProvider,Translate,OptimizationHints",
                        "--no-service-autorun", "--disable-background-timer-throttling",
                        "--disable-backgrounding-occluded-windows", "--disable-renderer-backgrounding"})
    cmd.push_back(f);
  cmd.push_back("--user-data-dir=" + S(prof));
  size_t budget_arg = cmd.size();
  cmd.push_back("--virtual-time-budget=" + std::to_string(budget_ms));
  cmd.push_back("--window-size=1280,2000");
  cmd.push_back("--lang=es-ES");
  cmd.push_back("--dump-dom");
  if (proxy) {
    // todo pasa por el proxy, incluso localhost (<-loopback quita la excepcion por defecto de Chromium)
    cmd.push_back("--proxy-server=http://127.0.0.1:" + std::to_string(proxy->port()));
    cmd.push_back("--proxy-bypass-list=<-loopback>");
  }
#if defined(__linux__)
  if (::geteuid() == 0) cmd.push_back("--no-sandbox");   // Chromium no arranca como root con sandbox (contenedores, proot)
#endif
  cmd.push_back(url);
  proc::Result r;
  proc::Options opt;
  opt.capture = true;
  for (int intento = 0; intento < 2; ++intento) {   // Chrome a veces se queda colgado al arrancar: un reintento
    r = proc::run(cmd, (intento ? TIMEOUT_S : 20) + budget_ms / 1000.0, opt);
    if (!r.started) throw std::runtime_error("no se pudo abrir el navegador");
    if (!r.timed_out) break;
    // el tiempo virtual depende de los fotogramas de la pantalla: en equipos sin pantalla (macOS en una maquina
    // virtual, CVDisplayLink falla) no avanza nunca. El reintento espera en tiempo real.
    cmd[budget_arg] = "--timeout=" + std::to_string(budget_ms);
    if (intento) {
      std::string err = trim(r.err);
      if (err.size() > 300) err = err.substr(err.size() - 300);
      {   // no funciona aqui: no se vuelve a intentar con el (cada pagina esperaria un minuto para nada)
        std::lock_guard<std::mutex> lk(found_mu);
        broken.insert(exe);
        found.clear();
      }
      L.warning("El navegador " + exe + " no responde en este equipo; se leen las paginas sin el");
      throw std::runtime_error("el navegador ha tardado demasiado" + (err.empty() ? "" : " (" + err + ")"));
    }
    L.info("El navegador no respondio a tiempo; se reintenta (" + exe + ")");
  }
  if (proxy) {
    auto blocked = proxy->blocked();
    proxy->close();
    if (!blocked.empty()) {
      std::set<std::string> uniq(blocked.begin(), blocked.end());
      std::vector<std::string> some(uniq.begin(), uniq.end());
      if (some.size() > 5) some.resize(5);
      L.info("Navegador: bloqueadas " + std::to_string(blocked.size()) + " conexiones a la red local (" +
             join(some, ", ") + ")");
    }
  }
  std::string html = valid_utf8(r.out);
  if (html.find('<') == std::string::npos) {
    std::string err = trim(r.err);
    if (err.size() > 200) err = err.substr(err.size() - 200);
    throw std::runtime_error("el navegador no devolvio la pagina (" + err + ")");
  }
  return html;
}

}  // namespace

std::string find(const std::string& preferred) {
  if (sys::is_android()) return "";
  std::lock_guard<std::mutex> lk(found_mu);
  if (auto it = found.find(preferred); it != found.end()) return it->second;
  std::vector<std::string> cs;
  if (!preferred.empty()) cs.push_back(preferred);
  for (auto& c : candidates()) cs.push_back(c);
  std::string exe;
  for (auto& c : cs)
    if (!c.empty() && !broken.count(c) && executable(P(c))) {
      exe = c;
      break;
    }
  found[preferred] = exe;
  if (!exe.empty()) L.info("Navegador para leer paginas con JavaScript: " + exe);
  return exe;
}

bool needs_browser(const std::string& raw) {
  if (raw.empty() || raw.substr(0, 2000).find('<') == std::string::npos) return false;
  static const Rx SCRIPT(R"((?i)<script\b)"),
      SPA(R"(<div[^>]+id=["'](root|app|__next|__nuxt|svelte|main-app|ember\w*)["'][^>]*>\s*</div>)"
          R"(|<app-root|ng-version=|data-reactroot|window\.__NUXT__|__NEXT_DATA__)", Rx::I),
      NEEDS_JS(R"((enable|activa|habilita|turn on)\s+(javascript|js)|requires? javascript|necesita javascript)"
               R"(|javascript (is )?(required|disabled))", Rx::I),
      NOSCRIPT(R"((?is)<noscript\b[^>]*>(.*?)</noscript>)");
  int w = words(raw);
  int scripts = (int)SCRIPT.finditer(raw).size();
  if (w < 60 && scripts >= 2) return true;
  if (SPA.test(raw) && w < 300) return true;
  std::string noscript = join(NOSCRIPT.findall(raw), " ");
  return NEEDS_JS.test(noscript) && w < 400;
}

std::string render(const std::string& url, std::string exe, int budget_ms, std::optional<bool> allow_private) {
  if (exe.empty()) exe = find();
  if (exe.empty()) throw std::runtime_error("no hay ningun navegador Chrome, Edge o Chromium en este equipo");
  return render_now(url, exe, budget_ms, allow_private ? *allow_private : allow_private_cfg());
}

std::string mode() {
  std::string m;
  try {
    m = get_config().str("rag.browser", "auto");
  } catch (...) {
  }
  return m == "auto" || m == "siempre" || m == "nunca" ? m : "auto";
}

std::pair<std::string, bool> maybe_render(const std::string& url, const std::string& raw, std::string how) {
  if (how.empty()) how = mode();
  if (how == "nunca" || (how == "auto" && !needs_browser(raw))) return {raw, false};
  std::string exe = find(browser_path());
  if (exe.empty()) return {raw, false};
  std::string dom;
  try {
    dom = render(url, exe);
  } catch (const std::runtime_error& e) {
    L.info("No se pudo leer " + url + " con el navegador: " + e.what());
    return {raw, false};
  }
  if (how == "auto" && words(dom) <= words(raw)) return {raw, false};
  return {dom, true};
}

json info() {
  std::string exe = find(browser_path());
  return {{"modo", mode()}, {"navegador", exe}, {"disponible", !exe.empty()}};
}

}  // namespace nx::rag::browser
