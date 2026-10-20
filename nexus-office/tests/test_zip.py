import io
import stat
import zipfile
from pathlib import Path

from nexus import paths
from nexus.rag import archive


def make_zip(path: Path, entries: dict[str, bytes], root: str = "") -> Path:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(root + name, data)
    return path


PROJECT = {
    "README.md": "# Calculadora volcánica\n\nCalcula la altura de los volcanes de Canarias.".encode(),
    "src/volcan.py": b"def altura_teide():\n    return 3718  # metros sobre el nivel del mar\n",
    "docs/guia.txt": b"Para instalar ejecuta pip install calculadora-volcanica.",
    "logo.png": b"\x89PNG\r\n\x1a\n\x00\x00binario",
    ".env": b"API_KEY=secreto",
    "node_modules/lib/index.js": b"module.exports = 1",
    ".git/config": b"[core]",
}


def test_zip_ingest_extracts_and_indexes(services, tmp_path):
    z = make_zip(tmp_path / "calculadora.zip", PROJECT, root="calculadora-main/")
    doc = services.rag.ingest_file(z, source="calculadora.zip")
    meta = doc["meta"]
    assert doc["kind"] == "zip" and meta["sandbox"] == "proyectos/calculadora"
    folder = paths.sandbox_dir() / meta["sandbox"]
    # la carpeta raiz unica se aplana y se omiten node_modules y .git
    assert (folder / "src" / "volcan.py").exists() and (folder / "logo.png").exists()
    assert not (folder / "node_modules").exists() and not (folder / ".git").exists()
    assert meta["files"] == 5 and meta["indexed"] == 3   # README, volcan.py, guia.txt (ni png ni .env)
    hits = services.rag.search("altura del Teide en metros", k=3)
    assert hits and hits[0]["doc_id"] == doc["id"] and "proyectos/calculadora/src/volcan.py" in hits[0]["text"]
    assert not any("API_KEY" in h["text"] for h in services.rag.search("API_KEY secreto", k=10, min_score=0))
    # los agentes lo exploran con las herramientas del sandbox
    from nexus.tools.builtin.sandbox_tools import file_list, file_read
    listed = file_list({"path": meta["sandbox"]}, None)
    assert "proyectos/calculadora/src/volcan.py" in listed["files"]
    assert "3718" in file_read({"path": "proyectos/calculadora/src/volcan.py"}, None)["content"]
    # borrar de la biblioteca elimina tambien la copia descomprimida
    services.rag.delete(doc["id"])
    assert not folder.exists() and not Path(doc["path"]).exists()


def test_zip_second_import_gets_new_folder(services, tmp_path):
    z = make_zip(tmp_path / "docs.zip", {"a.md": b"Primera version de la documentacion del proyecto."})
    d1 = services.rag.ingest_file(z, source="docs.zip")
    d2 = services.rag.ingest_file(z, source="docs.zip")
    assert d1["meta"]["sandbox"] == "proyectos/docs" and d2["meta"]["sandbox"] == "proyectos/docs_2"


def test_zip_slip_and_symlinks_are_rejected(tmp_path):
    buf = tmp_path / "malo.zip"
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("../fuera.txt", b"x")
        z.writestr("/abs.txt", b"x")
        z.writestr("C:/win.txt", b"x")
        z.writestr("ok/bien.txt", b"bien")
        z.writestr("raiz.txt", b"raiz")
        link = zipfile.ZipInfo("ok/enlace")
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        z.writestr(link, "/etc/passwd")
    dest = tmp_path / "dest"
    dest.mkdir()
    res = archive.safe_extract(buf, dest)
    assert [str(f) for f in res["files"]] == ["ok/bien.txt", "raiz.txt"]
    assert not (tmp_path / "fuera.txt").exists() and not (dest / "ok" / "enlace").exists()
    assert {m for _, m in res["skipped"]} == {"ruta no permitida", "enlace simbolico"}


def test_zip_bomb_limits(tmp_path, monkeypatch):
    monkeypatch.setattr(archive, "MAX_FILE", 1000)
    z = make_zip(tmp_path / "bomba.zip", {"grande.txt": b"0" * 5000, "chico.txt": b"hola"})
    dest = tmp_path / "dest"
    dest.mkdir()
    res = archive.safe_extract(z, dest)
    assert [str(f) for f in res["files"]] == ["chico.txt"] and not (dest / "grande.txt").exists()


def test_invalid_zip_rejected(services, tmp_path):
    bad = tmp_path / "roto.zip"
    bad.write_bytes(b"esto no es un zip")
    try:
        services.rag.ingest_file(bad)
    except ValueError as e:
        assert "ZIP" in str(e)
    else:
        raise AssertionError("debia fallar")
    assert not list((paths.sandbox_dir()).glob("proyectos/*"))


def test_upload_endpoint_accepts_zip(services, tmp_path):
    from fastapi.testclient import TestClient
    from nexus.server.app import create_app
    app = create_app(services)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("manual.md", "Manual de uso de la plataforma de despachos.")
    with TestClient(app) as c:
        r = c.post("/api/rag/upload", files={"file": ("manual.zip", buf.getvalue(), "application/zip")})
        assert r.status_code == 200, r.text
        assert r.json()["kind"] == "zip" and r.json()["meta"]["sandbox"] == "proyectos/manual"
        assert c.get(f"/api/rag/{r.json()['id']}/file").content[:2] == b"PK"
