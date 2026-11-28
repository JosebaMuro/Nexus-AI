"""FreeLLMAPI como endpoint LLM: motor «freellmapi», ajustes, que nunca sea respaldo automatico, la API y la incluida."""
from __future__ import annotations

import json
import os
import sqlite3
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

from nexus.freellmapi import FreeLLMAPI

CLAVE = "freellmapi-prueba123"


class _Falsa(BaseHTTPRequestHandler):
    """Imita el /v1 de FreeLLMAPI: pide la clave unica y responde en streaming como OpenAI."""
    vistos: list = []

    def log_message(self, *a):
        pass

    def _auth(self) -> bool:
        if self.headers.get("Authorization") != f"Bearer {CLAVE}":
            self.send_response(401)
            self.end_headers()
            return False
        return True

    def do_GET(self):
        if not self._auth():
            return
        body = json.dumps({"data": [{"id": "auto"}, {"id": "groq/llama-3.3-70b"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if not self._auth():
            return
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        _Falsa.vistos.append(req)
        if req.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for t in ("Hola ", "desde FreeLLMAPI"):
                self.wfile.write(f"data: {json.dumps({'choices': [{'delta': {'content': t}}]})}\n\n".encode())
            self.wfile.write(b"data: [DONE]\n\n")
            return
        body = json.dumps({"model": "groq/llama-3.3-70b", "choices": [{"message": {"content": "OK"}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)


@pytest.fixture()
def falsa():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), _Falsa)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    _Falsa.vistos = []
    yield f"http://127.0.0.1:{srv.server_address[1]}/v1"
    srv.shutdown()


def test_config_y_secretos():
    from nexus.config import DEFAULTS, SECRET_KEYS
    assert DEFAULTS["freellmapi"]["url"].endswith(":3001/v1") and DEFAULTS["freellmapi"]["modelo"] == "auto"
    assert {"freellmapi.clave", "freellmapi.clave_cifrado"} <= SECRET_KEYS


def test_motor_freellmapi_responde_y_no_es_respaldo(services, falsa):
    s = services
    assert "freellmapi" in s.llm.backends
    s.config.set("freellmapi.url", falsa)
    s.llm.actualizar_freellmapi()
    assert not s.llm.is_available("freellmapi")          # sin clave: FreeLLMAPI responde 401
    s.config.set("freellmapi.clave", CLAVE)
    s.llm.actualizar_freellmapi()
    assert s.llm.is_available("freellmapi")
    # sin elegirlo no responde nunca: los modelos locales siguen siendo los de siempre
    assert s.llm.select()[0] != "freellmapi"
    # pedido por un agente, si
    assert s.llm.select("freellmapi:auto") == ("freellmapi", "auto")
    texto = "".join(s.llm.stream([{"role": "user", "content": "hola"}], "freellmapi:auto"))
    assert "desde FreeLLMAPI" in texto
    assert _Falsa.vistos[-1]["model"] == "auto"
    # como modelo principal
    s.config.set("llm.default_model", "freellmapi:auto")
    assert s.llm.select() == ("freellmapi", "auto")
    s.config.set("llm.default_model", "")


def test_api_ajustes_probar_y_usar(services, falsa):
    from fastapi.testclient import TestClient
    from nexus.server import create_app
    services.freellmapi.raiz = Path("/no-esta-freellmapi")   # como un paquete sin la incluida (Android, codigo)
    with TestClient(create_app(services)) as c:
        h = {"X-Admin-Token": c.post("/api/admin/setup", json={"pin": "1234"}).json()["token"]}
        assert c.post("/api/freellmapi/ajustes", json={"url": falsa}).status_code == 403
        e = c.post("/api/freellmapi/ajustes", json={"url": falsa, "clave": CLAVE, "modelo": "auto"}, headers=h).json()
        assert e["clave_puesta"] and e["responde"] and e["url"] == falsa and not e["instalada"]
        assert c.post("/api/freellmapi/ajustes", json={"url": "ftp://x"}, headers=h).status_code == 400
        assert "groq/llama-3.3-70b" in c.get("/api/freellmapi/modelos").json()["modelos"]
        r = c.post("/api/freellmapi/probar", json={}, headers=h).json()
        assert r["respuesta"] == "OK" and r["modelo"] == "groq/llama-3.3-70b"
        u = c.post("/api/freellmapi/usar", json={"principal": True}, headers=h).json()
        assert u["estado"]["es_principal"] and services.config.get("llm.default_model") == "freellmapi:auto"
        c.post("/api/freellmapi/ajustes", json={"modelo": "groq/llama-3.3-70b"}, headers=h)
        assert services.config.get("llm.default_model") == "freellmapi:groq/llama-3.3-70b"
        c.post("/api/freellmapi/usar", json={"principal": False}, headers=h)
        assert services.config.get("llm.default_model") == ""
        # la clave nunca se devuelve
        assert CLAVE not in json.dumps(c.get("/api/freellmapi").json())
        # sin la incluida, arrancar explica que hacer
        r = c.post("/api/freellmapi/arrancar", json={}, headers=h)
        assert r.status_code == 400 and "URL" in r.json()["detail"]


def test_copia_la_clave_unica_de_su_base_de_datos(services, tmp_path):
    fl = FreeLLMAPI(services.config, raiz=tmp_path / "no-esta")
    db = fl.datos() / "freellmapi.db"
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT)")
    con.execute("INSERT INTO settings VALUES ('unified_api_key', ?)", (CLAVE,))
    con.commit()
    con.close()
    avisos = []
    fl.al_cambiar = lambda: avisos.append(1)
    assert fl.sincronizar_clave() and services.config.get("freellmapi.clave") == CLAVE and avisos
    assert not fl.sincronizar_clave()   # ya estaba


INCLUIDA = Path(__file__).resolve().parent.parent / "bundled" / "runtimes" / "freellmapi"


@pytest.mark.skipif(not (INCLUIDA / "app").exists() or os.environ.get("NEXUS_SIN_FREELLMAPI") == "1",
                    reason="sin FreeLLMAPI incluida (solo en los paquetes de escritorio)")
def test_incluida_arranca(services):
    services.config.set("freellmapi.url", "http://127.0.0.1:31996/v1")
    fl = FreeLLMAPI(services.config)
    import time
    fl.arrancar()
    try:
        for _ in range(60):
            time.sleep(1)
            if fl.estado()["responde"] and fl.clave():
                break
        assert fl.estado()["responde"] and fl.clave().startswith("freellmapi-")
        assert "auto" in fl.modelos() or fl.modelos()
    finally:
        fl.parar()
