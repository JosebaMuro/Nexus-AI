"""RAG que lee paginas como un navegador: deteccion de webs con JavaScript, lectura con Chromium y proxy de salida."""
import http.server
import socket
import threading

import pytest

from nexus.rag import browser

SPA = """<!doctype html><html><head><title>Tienda</title></head><body><div id="root"></div>
<noscript>You need to enable JavaScript to run this app.</noscript>
<script>document.getElementById('root').innerHTML = '<h1>Catalogo de bicicletas</h1>' +
  Array.from({length: 40}, (_, i) => '<p>Bicicleta electrica modelo ' + i + ' con bateria de larga duracion</p>').join('');</script>
<script src="/vacio.js"></script></body></html>"""
NORMAL = "<html><body><h1>Hola</h1>" + "<p>" + "texto de verdad " * 100 + "</p></body></html>"


def test_detecta_paginas_con_javascript():
    assert browser.needs_browser(SPA)
    assert not browser.needs_browser(NORMAL)
    assert not browser.needs_browser("solo texto plano")
    assert browser.needs_browser('<html><body><app-root></app-root><script src="a.js"></script></body></html>')


@pytest.fixture()
def sitio():
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = (SPA if self.path == "/" else "").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8" if self.path == "/" else "text/javascript")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_address[1]}/"
    srv.shutdown()


needs_chromium = pytest.mark.skipif(not browser.find(), reason="no hay Chrome/Edge/Chromium en este equipo")


@needs_chromium
def test_lee_el_dom_tras_el_javascript(sitio):
    html = browser.render(sitio, allow_private=True)
    assert "Catalogo de bicicletas" in html and "modelo 39" in html
    from nexus.config import get_config
    cfg = get_config()
    old = cfg.get("rag.allow_private_urls", False)
    cfg.set("rag.allow_private_urls", True, save=False)
    try:
        dom, usado = browser.maybe_render(sitio, SPA, "auto")
        assert usado and "modelo 39" in dom
        same, usado = browser.maybe_render(sitio, NORMAL, "auto")
        assert not usado and same == NORMAL
        assert browser.maybe_render(sitio, SPA, "nunca") == (SPA, False)
    finally:
        cfg.set("rag.allow_private_urls", old, save=False)


@needs_chromium
def test_el_proxy_no_deja_llegar_a_la_red_local(sitio):
    # sin permiso para URLs privadas, el navegador sale por el proxy y este corta 127.0.0.1
    try:
        html = browser.render(sitio, allow_private=False)
    except RuntimeError:
        html = ""
    assert "Catalogo de bicicletas" not in html


def test_proxy_bloquea_direcciones_privadas():
    p = browser._GuardProxy()
    try:
        for target in (b"CONNECT 127.0.0.1:443 HTTP/1.1\r\nHost: x\r\n\r\n", b"GET http://192.168.1.1/ HTTP/1.1\r\nHost: x\r\n\r\n",
                       b"CONNECT localhost:8775 HTTP/1.1\r\n\r\n"):
            s = socket.create_connection(("127.0.0.1", p.port), timeout=5)
            s.sendall(target)
            assert s.recv(100).startswith(b"HTTP/1.1 403")
            s.close()
        assert {"127.0.0.1", "192.168.1.1", "localhost"} <= set(p.blocked)
    finally:
        p.close()
