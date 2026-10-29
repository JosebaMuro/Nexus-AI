// Manual-de-usuario.pdf y Documentacion-tecnica.pdf a partir de los .md de docs/, con portada, indice y pie
// «Nexus-Office <version> · pagina/total». El Markdown se pasa a HTML aqui (markdown.cpp) y Chromium sin interfaz lo
// imprime en PDF por el protocolo DevTools (Page.printToPDF), como hace Playwright.
//
//   nexus-construir pdf [carpeta_salida]          por defecto dist/; navegador: NEXUS_CHROMIUM o el del equipo
#include <chrono>
#include <thread>

#include <httplib.h>

#include "comun.h"
#include "rag/browser.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace nc {

using namespace nx;

namespace {

struct Libro {
  std::string nombre, titulo, subtitulo;
  std::vector<std::string> docs;
};

const std::vector<Libro> LIBROS = {
    {"Manual-de-usuario", "Manual de usuario", "Edición C++ · oficina de IA local para pymes",
     {"USUARIO.md", "CPP.md", "OFICINA.md", "BOLSA.md", "SERVICIO.md", "AVATAR.md", "SALON.md", "ACELERADOR.md",
      "GRAFOS.md", "RAG.md", "DISPOSITIVOS.md"}},
    {"Documentacion-tecnica", "Documentación técnica", "Edición C++: cómo está hecha, administración y API",
     {"CPP.md", "ACELERADOR.md", "ADMINISTRADOR.md", "SERVICIO.md", "API.md", "HERRAMIENTAS.md", "MEMORIA.md",
      "GRAFOS.md", "RAG.md", "RED_COOPERATIVA.md", "VPN.md", "ACCESO_REMOTO.md", "DISPOSITIVOS.md"}},
};

const char* CSS = R"(
@page { size: A4; margin: 18mm 16mm 20mm 16mm; }
body { font-family: 'DejaVu Sans', 'Noto Sans', Arial, sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1d2330; }
h1 { font-size: 20pt; color: #0b4f6c; border-bottom: 2px solid #0b4f6c; padding-bottom: 4px; page-break-before: always; }
h2 { font-size: 14pt; color: #0b4f6c; margin-top: 1.4em; } h3 { font-size: 11.5pt; color: #24384a; }
h2, h3 { page-break-after: avoid; }
table { border-collapse: collapse; width: 100%; margin: .6em 0; font-size: 9pt; page-break-inside: auto; }
th, td { border: 1px solid #c7d0da; padding: 3px 6px; vertical-align: top; text-align: left; }
th { background: #e8eef4; } tr { page-break-inside: avoid; }
code { font-family: 'DejaVu Sans Mono', monospace; font-size: 8.8pt; background: #f1f4f7; padding: 0 2px; }
pre { background: #f1f4f7; padding: 6px 8px; white-space: pre-wrap; word-break: break-word; font-size: 8.5pt; }
pre code { background: none; padding: 0; }
a { color: #0b4f6c; text-decoration: none; }
blockquote { border-left: 3px solid #c7d0da; margin-left: 0; padding-left: 10px; color: #3b4656; }
.cover { height: 250mm; display: flex; flex-direction: column; justify-content: center; text-align: center; }
.cover .v { font-size: 30pt; font-weight: bold; color: #0b4f6c; } .cover .t { font-size: 22pt; margin-top: 8mm; }
.cover .s { font-size: 13pt; color: #556; margin-top: 4mm; }
.toc { page-break-before: always; } .toc ul { list-style: none; padding-left: 1.2em; } .toc > ul { padding-left: 0; }
.toc li { margin: 2px 0; } .toc .l1 { font-weight: bold; margin-top: 6px; }
)";

std::string slug(const std::string& text, std::set<std::string>& used) {
  static const Rx bad(R"([^\w\- ])");
  std::string s = replace_all(trim(bad.sub(lower(text), "")), " ", "-");
  if (s.empty()) s = "s";
  std::string base = s;
  for (int n = 2; used.count(s); ++n) s = base + "-" + std::to_string(n);
  used.insert(s);
  return s;
}

std::string libro_html(const Libro& b) {
  static const Rx head(R"(<h([1-6])>(.*?)</h\1>)", Rx::S);
  static const Rx tags(R"(<[^>]+>)");
  static const Rx local(R"(href="(?!https?:|#)[^"]*")");
  std::set<std::string> used;
  std::vector<std::tuple<int, std::string, std::string>> toc;
  std::string body;
  for (auto& d : b.docs) {
    fs::path f = raiz() / "docs" / d;
    if (!is_file(f)) throw Fallo("falta " + S(f));
    std::string part = markdown_html(read_text(f));
    part = head.sub(part, [&](const Rx::Match& m) {
      int level = std::stoi(m.str(1));
      std::string inner = m.str(2);
      std::string plain = html_unescape(tags.sub(inner, ""));
      std::string sid = slug(plain, used);
      if (level <= 3) toc.emplace_back(level, plain, sid);
      return cat("<h", level, " id=\"", sid, "\">", inner, "</h", level, ">");
    });
    body += local.sub(part, "href=\"#\"");   // enlaces a otros .md: sin destino en el PDF
  }
  std::string items;
  for (auto& [lvl, t, sid] : toc)
    items += cat("<li class=\"l", lvl, "\" style=\"margin-left:", fnum((lvl - 1) * 1.2, 1), "em\"><a href=\"#", sid, "\">",
                 html_escape(t), "</a></li>");
  return cat("<!doctype html><html lang='es'><head><meta charset='utf-8'><title>", b.titulo, "</title><style>", CSS,
             "</style></head><body><div class='cover'><div class='v'>Nexus-Office ", version(), "</div><div class='t'>",
             b.titulo, "</div><div class='s'>", b.subtitulo, "</div></div><div class='toc'><h2>", b.titulo,
             "</h2><ul>", items, "</ul></div>", body, "</body></html>");
}

std::string file_url(const fs::path& p) {
  std::string s = fs::absolute(p).generic_string();
  if (!starts_with(s, "/")) s = "/" + s;   // C:/...
  return "file://" + replace_all(url_encode(s), "%2F", "/");
}

// Chromium sin interfaz con el protocolo DevTools: navega a cada HTML e imprime el PDF
class Cromo {
 public:
  explicit Cromo(const std::string& exe) {
    prof_ = fs::temp_directory_path() / ("nexus-pdf-" + token_hex(6));
    fs::create_directories(prof_);
    std::vector<std::string> cmd = {exe, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                                    "--disable-extensions", "--use-mock-keychain", "--password-store=basic",
                                    "--allow-file-access-from-files", "--remote-debugging-port=0",
                                    "--user-data-dir=" + S(prof_)};
#if defined(__linux__)
    if (::geteuid() == 0) cmd.push_back("--no-sandbox");
#endif
    cmd.push_back("about:blank");
    proc::Options opt;
    opt.output_file = prof_ / "chromium.log";
    if (!p_.start(cmd, opt)) throw Fallo("no se pudo abrir el navegador " + exe);
    fs::path port_file = prof_ / "DevToolsActivePort";
    for (int i = 0; i < 300 && !is_file(port_file); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string port;
    for (int i = 0; i < 50 && port.empty(); ++i) {
      auto lines = splitlines(read_text(port_file));
      if (!lines.empty()) port = trim(lines[0]);
      if (port.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (port.empty()) throw Fallo("el navegador no abre el puerto de DevTools: " + read_text(prof_ / "chromium.log"));
    base_ = "127.0.0.1:" + port;
    std::string ws;
    for (int i = 0; i < 50 && ws.empty(); ++i) {
      auto r = http::get("http://" + base_ + "/json/list", {}, 5);
      json list = r.json_body();
      if (list.is_array())
        for (auto& t : list)
          if (jstr(t, "type") == "page") ws = jstr(t, "webSocketDebuggerUrl");
      if (ws.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (ws.empty()) throw Fallo("el navegador no ofrece ninguna pagina por DevTools");
    ws_ = std::make_unique<httplib::ws::WebSocketClient>(ws);
    ws_->set_connection_timeout(10);
    ws_->set_read_timeout(120);
    if (!ws_->connect()) throw Fallo("no puedo conectar con DevTools en " + ws);
    llamar("Page.enable", json::object());
  }

  ~Cromo() {
    ws_.reset();
    p_.terminate();
    if (!p_.wait(10)) p_.kill();
    std::error_code ec;
    fs::remove_all(prof_, ec);
  }

  // envia un mensaje y espera su respuesta; evento != "" espera ademas ese evento
  json llamar(const std::string& method, const json& params, const std::string& evento = "") {
    int id = ++id_;
    json msg = {{"id", id}, {"method", method}, {"params", params}};
    if (!ws_->send(msg.dump())) throw Fallo("DevTools: no se pudo enviar " + method);
    json result;
    bool got = false, ev = evento.empty();
    auto limit = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!(got && ev)) {
      if (std::chrono::steady_clock::now() > limit) throw Fallo("DevTools: sin respuesta a " + method);
      std::string in;
      auto r = ws_->read(in);
      if (r == httplib::ws::Fail) throw Fallo("DevTools: conexion cerrada esperando " + method);
      if (r != httplib::ws::Text) continue;
      json j = jparse(in);
      if (!j.is_object()) continue;
      if (j.contains("id") && j["id"] == id) {
        if (j.contains("error")) throw Fallo("DevTools " + method + ": " + j["error"].dump());
        result = j.value("result", json::object());
        got = true;
      } else if (!evento.empty() && jstr(j, "method") == evento) {
        ev = true;
      }
    }
    return result;
  }

  void imprimir(const fs::path& html, const fs::path& pdf, const std::string& footer) {
    llamar("Page.navigate", {{"url", file_url(html)}}, "Page.loadEventFired");
    const double mm = 1 / 25.4;
    json r = llamar("Page.printToPDF", {{"paperWidth", 210 * mm}, {"paperHeight", 297 * mm}, {"printBackground", true},
                                        {"displayHeaderFooter", true}, {"headerTemplate", "<span></span>"},
                                        {"footerTemplate", footer}, {"marginTop", 18 * mm}, {"marginBottom", 20 * mm},
                                        {"marginLeft", 16 * mm}, {"marginRight", 16 * mm}, {"preferCSSPageSize", false}});
    std::string data = b64decode(jstr(r, "data"));
    if (data.size() < 5 || data.compare(0, 5, "%PDF-") != 0) throw Fallo("el navegador no ha devuelto un PDF");
    escribir(pdf, data);
  }

 private:
  proc::Process p_;
  fs::path prof_;
  std::string base_;
  std::unique_ptr<httplib::ws::WebSocketClient> ws_;
  int id_ = 0;
};

}  // namespace

int pdf(const std::vector<std::string>& argv) {
  Args a = leer_args(argv, {}, {});
  fs::path out = a.pos.empty() ? raiz() / "dist" : P(a.pos[0]);
  fs::create_directories(out);
  std::string exe = env("NEXUS_CHROMIUM");
  if (exe.empty()) {
    if (env("NEXUS_DATA_DIR").empty()) set_env("NEXUS_DATA_DIR", S(fs::temp_directory_path() / "nexus-construir"));
    exe = rag::browser::find();
  }
  if (exe.empty()) throw Fallo("no hay Chromium, Chrome ni Edge: indicalo en NEXUS_CHROMIUM");
  std::string footer = cat("<div style='font-size:8px;width:100%;text-align:right;padding-right:16mm;color:#667'>Nexus-Office ",
                           version(), " · <span class='pageNumber'></span>/<span class='totalPages'></span></div>");
  fs::path work = raiz() / "build-paquetes" / "pdf";
  fs::create_directories(work);
  Cromo c(exe);
  for (auto& b : LIBROS) {
    fs::path html = work / (b.nombre + ".html");
    escribir(html, libro_html(b));
    fs::path dest = out / (b.nombre + ".pdf");
    c.imprimir(html, dest, footer);
    log("docs", "-> " + S(dest) + " (" + fnum((double)fsize(dest) / 1e3, 0) + " KB)");
  }
  return 0;
}

}  // namespace nc
