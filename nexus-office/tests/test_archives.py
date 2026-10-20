"""Comprimidos RAR, 7z y tar(.gz): extraccion segura, limites e indexacion."""
import io
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from nexus import paths
from nexus.rag import archive

DATA = Path(__file__).parent / "data" / "rar"
TOOLS = {"7z": shutil.which("7zz") or shutil.which("7z"), "unrar": shutil.which("unrar"),
         "bsdtar": shutil.which("bsdtar"), "unar": shutil.which("unar")}


def make_tar(path: Path, entries: dict[str, bytes], mode: str = "w:gz") -> Path:
    with tarfile.open(path, mode) as t:
        for name, data in entries.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    return path


def test_detect_kind_by_signature(tmp_path):
    assert archive.detect_kind(DATA / "rar3-subdirs.rar") == "rar"
    t = make_tar(tmp_path / "x.tar.gz", {"a.txt": b"hola"})
    assert archive.detect_kind(t) == "tar"
    fake = tmp_path / "no-es.rar"
    fake.write_bytes(b"texto normal")
    assert archive.detect_kind(fake) == "rar"          # por extension: luego falla al descomprimir con un error claro


def test_tar_gz_ingest_and_symlinks(services, tmp_path):
    t = tmp_path / "guia-volcanes.tar.gz"
    with tarfile.open(t, "w:gz") as tf:
        for name, data in {"guia-1.0/README.md": "# Guía\n\nEl Teide mide 3718 metros.".encode(),
                           "guia-1.0/src/medir.py": b"def medir():\n    return 3718\n"}.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))
        link = tarfile.TarInfo("guia-1.0/enlace")
        link.type, link.linkname = tarfile.SYMTYPE, "/etc/passwd"
        tf.addfile(link)
        evil = tarfile.TarInfo("../../fuera.txt")
        evil.size = 1
        tf.addfile(evil, io.BytesIO(b"x"))
    doc = services.rag.ingest_file(t, source="guia-volcanes.tar.gz")
    meta = doc["meta"]
    assert doc["kind"] == "tar" and meta["format"] == "tar" and meta["sandbox"] == "proyectos/guia-volcanes"
    folder = paths.sandbox_dir() / meta["sandbox"]
    assert (folder / "src" / "medir.py").exists() and not (folder / "enlace").exists()
    assert not (tmp_path / "fuera.txt").exists() and meta["files"] == 2 and meta["indexed"] == 2
    assert any("enlace simbolico" in s for s in meta["skipped_detail"])
    hits = services.rag.search("cuanto mide el Teide", k=2)
    assert hits and hits[0]["doc_id"] == doc["id"]
    services.rag.delete(doc["id"])
    assert not folder.exists()


@pytest.mark.parametrize("tool", ["7z", "unrar", "bsdtar", "unar"])
def test_rar_with_each_tool(tool, tmp_path, monkeypatch):
    if not TOOLS[tool]:
        pytest.skip(f"{tool} no instalado")
    monkeypatch.setattr(archive, "find_extractor", lambda kind: (tool, TOOLS[tool]))
    dest = tmp_path / "dest"
    dest.mkdir()
    res = archive.extract_any(DATA / "rar3-subdirs.rar", dest, "rar")
    names = sorted(str(f) for f in res["files"])
    # una unica carpeta raiz (sub/) se aplana; nombres con espacios y no ASCII se conservan
    assert "dir1/file1.txt" in names and "with space/long fn.txt" in names and len(names) == 4
    assert not list(tmp_path.glob(".cuarentena-*")) and not list(dest.parent.glob(".cuarentena-*"))


def test_rar_symlink_traversal_is_blocked(tmp_path):
    if not archive.find_extractor("rar")[0]:
        pytest.skip("sin herramienta RAR")
    dest = tmp_path / "proy" / "dest"
    dest.mkdir(parents=True)
    try:
        res = archive.extract_any(DATA / "rar5-evil-symlink-traversal.rar", dest, "rar")
    except ValueError:
        res = {"files": []}
    assert not (tmp_path / "proy" / "pwned.txt").exists() and not (dest / "up").is_symlink()
    assert all(not (dest / f).is_symlink() for f in res["files"])


def test_rar_encrypted_reports_error(services, tmp_path):
    if not archive.find_extractor("rar")[0]:
        pytest.skip("sin herramienta RAR")
    src = tmp_path / "cifrado.rar"
    shutil.copy(DATA / "rar5-psw-blake.rar", src)
    with pytest.raises(ValueError, match="no se pudo descomprimir|no contiene texto"):
        services.rag.ingest_file(src, source="cifrado.rar")
    assert not list(paths.sandbox_dir().glob("proyectos/*"))


def test_rar_ingest_indexes(services, tmp_path):
    if not archive.find_extractor("rar")[0]:
        pytest.skip("sin herramienta RAR")
    src = tmp_path / "solido.rar"
    shutil.copy(DATA / "rar5-solid.rar", src)
    doc = services.rag.ingest_file(src, source="solido.rar")
    assert doc["kind"] == "rar" and doc["meta"]["files"] == 2 and doc["meta"]["tool"]


def test_7z_roundtrip(services, tmp_path):
    if not TOOLS["7z"]:
        pytest.skip("7z no instalado")
    src = tmp_path / "proyecto"
    (src / "docs").mkdir(parents=True)
    (src / "docs" / "manual.md").write_text("Manual del medidor de volcanes: el Teide mide 3718 metros.", encoding="utf-8")
    (src / "app.py").write_text("print('hola')\n", encoding="utf-8")
    out = tmp_path / "proyecto.7z"
    subprocess.run([TOOLS["7z"], "a", "-bd", str(out), str(src)], check=True, capture_output=True)
    doc = services.rag.ingest_file(out, source="proyecto.7z")
    assert doc["kind"] == "7z" and doc["meta"]["files"] == 2 and doc["meta"]["indexed"] == 2
    assert (paths.sandbox_dir() / doc["meta"]["sandbox"] / "docs" / "manual.md").exists()


def test_missing_tool_explains_how_to_install(tmp_path, monkeypatch):
    monkeypatch.setattr(archive, "find_extractor", lambda kind: ("", ""))
    with pytest.raises(ValueError, match="7-Zip"):
        archive.extract_any(DATA / "rar3-subdirs.rar", tmp_path, "rar")


def test_external_limits(tmp_path, monkeypatch):
    if not archive.find_extractor("rar")[0]:
        pytest.skip("sin herramienta RAR")
    monkeypatch.setattr(archive, "MAX_FILES", 2)
    dest = tmp_path / "dest"
    dest.mkdir()
    res = archive.extract_any(DATA / "rar3-subdirs.rar", dest, "rar")
    assert len(res["files"]) == 2 and any(m == "limite de ficheros" for _, m in res["skipped"])


def test_big_project_is_split_in_ordered_parts(services, monkeypatch, tmp_path):
    """Un proyecto que no cabe en un documento se reparte en partes correlativas (antes se cortaba)."""
    import zipfile as zf

    from nexus.rag import archive
    monkeypatch.setattr(archive, "MAX_INDEX_CHARS", 3000)
    monkeypatch.setattr(archive, "READ_GROUP", 4)
    src = tmp_path / "grande.zip"
    with zf.ZipFile(src, "w") as z:
        for i in range(12):
            z.writestr(f"src/mod{i:02d}.py", f"# modulo {i}\n" + f"def funcion_{i}():\n    return 'valor{i}fin'\n" * 40)
    doc = services.rag.ingest_file(src, source="grande.zip")
    ids = doc["meta"]["part_ids"]
    assert len(ids) >= 3 and doc["meta"]["parts"] == len(ids)
    parts = [services.rag.get(i) for i in ids]
    assert [p["meta"]["part"] for p in parts] == list(range(1, len(ids) + 1))
    assert parts[0]["meta"]["file_from"] == 1 and parts[-1]["meta"]["file_to"] == 12
    assert all(parts[k]["meta"]["file_from"] == parts[k - 1]["meta"]["file_to"] + 1 for k in range(1, len(parts)))
    hit = services.rag.search("valor11fin funcion_11", k=1)
    assert hit and hit[0]["doc_id"] == ids[-1]
    services.rag.delete(ids[0])          # borrar la primera parte quita el proyecto entero
    assert all(services.rag.get(i) is None for i in ids)
