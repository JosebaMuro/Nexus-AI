"""Biblioteca de avatares 3D gratuitos: catalogos, descarga (con pasarelas IPFS de respaldo), uso y modo sin conexion."""
import json
import os

import httpx
import pytest
from fastapi.testclient import TestClient

GLB = b"glTF" + b"\x02\x00\x00\x00" + b"\x00" * 100

PROJECTS = [{"id": "p1", "name": "100Avatars R1", "creator_id": "Polygonal-Mind", "license": "CC0", "is_public": True,
             "avatar_data_file": "avatars/p1.json"},
            {"id": "oculto", "name": "Oculto", "is_public": False, "avatar_data_file": "avatars/x.json"}]
AVS = [{"id": "a1", "name": "Devil", "description": "Avatar 031: Devil", "format": "VRM", "is_public": True,
        "model_file_url": "https://dweb.link/ipfs/QmX/devil.vrm", "thumbnail_url": "https://arweave.net/thumb"},
       {"id": "a2", "name": "Robot", "description": "", "format": "VRM", "is_public": True,
        "model_file_url": "https://arweave.net/robot", "thumbnail_url": ""},
       {"id": "a3", "name": "Borrador", "is_draft": True, "model_file_url": "https://arweave.net/x"}]


def transport(caido=False):
    def handler(req: httpx.Request):
        u = str(req.url)
        if caido:
            raise httpx.ConnectError("sin red")
        if u.endswith("projects.json"):
            return httpx.Response(200, json=PROJECTS)
        if u.endswith("avatars/p1.json"):
            return httpx.Response(200, json=AVS)
        if u.startswith("https://dweb.link/"):
            return httpx.Response(504)                       # pasarela caida: se prueba la siguiente
        if u.startswith("https://ipfs.io/ipfs/QmX/devil.vrm"):
            return httpx.Response(200, content=GLB)
        if u == "https://arweave.net/thumb":
            return httpx.Response(200, content=b"\x89PNG....")
        if u == "https://arweave.net/robot":
            return httpx.Response(200, content=b"<html>no</html>")
        if "raw.githubusercontent.com" in u and u.endswith(".glb"):
            return httpx.Response(200, content=GLB, headers={"content-length": str(len(GLB))})
        return httpx.Response(404)
    return httpx.MockTransport(handler)


@pytest.fixture
def av(services):
    from nexus.media import avatares
    avatares.reset()
    a = avatares.get(services.config)
    a.http = httpx.Client(transport=transport(), follow_redirects=True)
    yield a
    avatares.reset()


def test_catalogos_y_descarga(av):
    r = av.buscar("osa")
    assert r["total"] == 2 and {i["nombre"] for i in r["resultados"]} == {"Devil", "Robot"}
    assert r["resultados"][0]["licencia"] == "CC0" and r["resultados"][0]["id"].startswith("osa:")
    assert av.buscar("osa", "devil")["total"] == 1
    m = av.buscar("muestras", "robot")
    assert m["total"] >= 1 and all(i["id"].startswith("muestras:") for i in m["resultados"])
    it = av._descargar("osa:a1")
    assert it["archivo"].endswith(".vrm") and it["miniatura_local"] and it["licencia"] == "CC0"
    assert av.buscar("osa", "devil")["resultados"][0]["descargado"]
    assert av._descargar("osa:a1")["id"] == it["id"]          # no se descarga dos veces
    with pytest.raises(ValueError):                           # no es un glTF
        av._descargar("osa:a2")
    with pytest.raises(LookupError):
        av._descargar("osa:a3")                               # borrador: no esta en el catalogo
    with pytest.raises(LookupError):
        av._descargar("https://evil.example/x.glb")           # solo URLs del catalogo
    with pytest.raises(PermissionError):
        av._item("sketchfab:abc")                             # Sketchfab sin clave
    # sin conexion: el indice guardado sigue sirviendo y lo descargado tambien
    av.http = httpx.Client(transport=transport(caido=True))
    av._osa = None
    assert av.buscar("osa")["total"] == 2
    assert [i["id"] for i in av.biblioteca()] == [it["id"]]
    assert av.borrar(it["id"]) and av.biblioteca() == []


def test_api_descargar_y_usar(services, av):
    from nexus.server.app import create_app
    with TestClient(create_app(services)) as c:
        d = c.get("/api/avatares").json()
        assert set(d["fuentes"]) == {"osa", "muestras", "sketchfab"} and d["biblioteca"] == []
        assert c.get("/api/avatares/buscar", params={"fuente": "osa"}).json()["total"] == 2
        j = c.post("/api/avatares/descargar", json={"id": "muestras:xbot"}).json()
        import time
        for _ in range(100):
            s = c.get(f"/api/avatares/trabajo/{j['id']}").json()
            if s["estado"] != "en_curso":
                break
            time.sleep(0.05)
        assert s["estado"] == "hecho", s
        aid = s["avatar"]["id"]
        assert c.get(s["avatar"]["url"]).content == GLB
        assert c.post(f"/api/avatares/{aid}/usar").json()["ok"]
        cfg = c.get("/api/config").json()
        assert cfg["user"]["avatar_style"] == "glb" and cfg["user"]["avatar_model"] == s["avatar"]["url"]
        assert c.put("/api/config", json={"avatares.sketchfab_token": "abc"}).status_code == 200
        assert c.get("/api/avatares").json()["sketchfab_clave"]
        assert c.delete(f"/api/avatares/{aid}").json()["ok"]
        assert c.delete(f"/api/avatares/{aid}").status_code == 404


@pytest.mark.skipif(os.environ.get("NEXUS_ONLINE_TESTS") != "1", reason="necesita Internet (NEXUS_ONLINE_TESTS=1)")
def test_catalogos_reales(services):
    from nexus.media import avatares
    avatares.reset()
    a = avatares.get(services.config)
    r = a.buscar("osa", por_pagina=5)
    print("osa:", r["total"], [i["nombre"] for i in r["resultados"]])
    assert r["total"] > 100
    it = a._descargar(r["resultados"][0]["id"])
    print("descargado:", json.dumps(it, ensure_ascii=False))
    assert it["bytes"] > 10000
    m = a._descargar("muestras:fox")
    assert m["bytes"] > 10000
    s = a.buscar("sketchfab", "robot", por_pagina=3)
    print("sketchfab:", s["total"], [i["nombre"] for i in s["resultados"]])
