// RAG que lee paginas como un navegador (tests/test_navegador.py): deteccion de webs con JavaScript, lectura con
// Chromium y proxy de salida.
#include <httplib.h>

#include <cstdio>
#include <thread>

#include "check.h"
#include "core/config.h"
#include "rag/browser.h"
#include "util/net.h"
#include "util/text.h"

using namespace nx;

namespace {

const std::string SPA = R"(<!doctype html><html><head><title>Tienda</title></head><body><div id="root"></div>
<noscript>You need to enable JavaScript to run this app.</noscript>
<script>document.getElementById('root').innerHTML = '<h1>Catalogo de bicicletas</h1>' +
  Array.from({length: 40}, (_, i) => '<p>Bicicleta electrica modelo ' + i + ' con bateria de larga duracion</p>').join('');</script>
<script src="/vacio.js"></script></body></html>)";

std::string pagina_normal() {
  std::string p;
  for (int i = 0; i < 100; ++i) p += "texto de verdad ";
  return "<html><body><h1>Hola</h1><p>" + p + "</p></body></html>";
}

struct Sitio {
  httplib::Server srv;
  std::thread th;
  int port = 0;
  Sitio() {
    srv.Get("/", [](const httplib::Request&, httplib::Response& r) { r.set_content(SPA, "text/html; charset=utf-8"); });
    srv.Get("/vacio.js", [](const httplib::Request&, httplib::Response& r) { r.set_content("", "text/javascript"); });
    port = srv.bind_to_any_port("127.0.0.1");
    th = std::thread([this] { srv.listen_after_bind(); });
  }
  ~Sitio() {
    srv.stop();
    th.join();
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/"; }
};

}  // namespace

TEST("navegador: detecta paginas con JavaScript") {
  CHECK(rag::browser::needs_browser(SPA));
  CHECK(!rag::browser::needs_browser(pagina_normal()));
  CHECK(!rag::browser::needs_browser("solo texto plano"));
  CHECK(rag::browser::needs_browser(R"(<html><body><app-root></app-root><script src="a.js"></script></body></html>)"));
  json i = rag::browser::info();
  CHECK(i["modo"] == "auto" && i["disponible"] == !jstr(i, "navegador").empty());
}

TEST("navegador: el proxy bloquea direcciones privadas") {
  net::GuardProxy p;
  CHECK(p.port() > 0);
  for (const char* target : {"CONNECT 127.0.0.1:443 HTTP/1.1\r\nHost: x\r\n\r\n",
                             "GET http://192.168.1.1/ HTTP/1.1\r\nHost: x\r\n\r\n", "CONNECT localhost:8775 HTTP/1.1\r\n\r\n",
                             "CONNECT 239.255.255.250:1900 HTTP/1.1\r\n\r\n"}) {
    auto r = net::tcp_exchange("127.0.0.1", p.port(), target, 5);
    CHECK(r && starts_with(*r, "HTTP/1.1 403"));
  }
  auto b = p.blocked();
  std::set<std::string> got(b.begin(), b.end());
  for (const char* h : {"127.0.0.1", "192.168.1.1", "localhost", "239.255.255.250"}) CHECK(got.count(h));
  CHECK(!net::public_host("10.0.0.1") && !net::public_host("::1") && !net::public_host("ff02::1"));
  p.close();
}

TEST("navegador: lee el DOM tras el JavaScript y el proxy corta la red local") {
  if (rag::browser::find().empty()) return;   // no hay Chrome/Edge/Chromium en este equipo
  Sitio sitio;
  std::string exe = rag::browser::find(), html;
  try {
    html = rag::browser::render(sitio.url(), "", rag::browser::BUDGET_MS, true);
  } catch (const std::runtime_error& e) {
    // un navegador que no responde en este equipo (macOS en una maquina virtual sin pantalla) se aparta y las
    // paginas se siguen leyendo con la descarga normal
    std::printf("    %s no funciona aqui: %s\n", exe.c_str(), e.what());
    CHECK(contains(e.what(), "tardado demasiado") && rag::browser::find() != exe);
    if (rag::browser::find().empty()) {
      CHECK(rag::browser::maybe_render(sitio.url(), SPA, "siempre") == std::make_pair(SPA, false));
      CHECK(rag::browser::info()["disponible"] == false);
    }
    return;
  }
  CHECK(contains(html, "Catalogo de bicicletas") && contains(html, "modelo 39"));
  auto& cfg = get_config();
  json old = cfg.get("rag.allow_private_urls", false);
  cfg.set("rag.allow_private_urls", true, false);
  auto [dom, usado] = rag::browser::maybe_render(sitio.url(), SPA, "auto");
  CHECK(usado && contains(dom, "modelo 39"));
  auto [same, usado2] = rag::browser::maybe_render(sitio.url(), pagina_normal(), "auto");
  CHECK(!usado2 && same == pagina_normal());
  CHECK(rag::browser::maybe_render(sitio.url(), SPA, "nunca") == std::make_pair(SPA, false));
  cfg.set("rag.allow_private_urls", old, false);
  // sin permiso para URLs privadas, el navegador sale por el proxy y este corta 127.0.0.1
  std::string bloqueada;
  try {
    bloqueada = rag::browser::render(sitio.url(), "", rag::browser::BUDGET_MS, false);
  } catch (const std::runtime_error&) {
  }
  CHECK(!contains(bloqueada, "Catalogo de bicicletas"));
}
