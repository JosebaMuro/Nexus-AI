"""Importacion de archivos comprimidos (ZIP, RAR, 7z, tar.gz...) con proyectos o documentacion.

El archivo se descomprime de forma segura en data/sandbox/proyectos/<nombre>/ (donde los agentes lo
exploran con file_list/file_read) y sus documentos y ficheros de codigo se indexan en la
biblioteca RAG como un unico documento, un fichero por pagina, con la ruta en cada fragmento.

- ZIP y tar(.gz/.bz2/.xz): con la biblioteca estandar de Python, en cualquier sistema.
- RAR y 7z: con la herramienta del sistema que haya (7-Zip `7z`/`7zz`, `unrar`, `bsdtar` -el `tar` de
  Windows 10/11 y macOS- o `unar`). Se descomprime en una carpeta de cuarentena y solo se aceptan
  ficheros normales dentro de ella, con los mismos limites que el ZIP.
- La lectura de los documentos para indexarlos se reparte entre todos los nucleos (nexus.parallel).
"""
from __future__ import annotations

import os
import json
import re
import secrets
import shutil
import stat
import subprocess
import tarfile
import time
import zipfile
from pathlib import Path, PurePosixPath

from .. import log, parallel, paths
from .readers import archive_suffix, read_document

L = log.get("rag")

# limites contra bombas zip y proyectos desmesurados
MAX_FILES = 20000                   # entradas extraidas
MAX_TOTAL = 1024 * 1024 * 1024      # bytes descomprimidos en total
MAX_FILE = 50 * 1024 * 1024         # bytes por fichero
MAX_RATIO = 200                     # compresion maxima sospechosa (ficheros > 1 MB)
MAX_INDEX_FILE = 512 * 1024         # bytes por fichero indexado en el RAG
MAX_INDEX_FILES = 20000
MAX_INDEX_CHARS = 6_000_000         # caracteres por parte: lo que no cabe va a la parte siguiente
READ_GROUP = 200                    # ficheros leidos a la vez (memoria acotada aunque el proyecto sea enorme)

DOC_EXT = {".pdf", ".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx", ".epub", ".rtf", ".html", ".htm", ".xhtml",
           ".md", ".markdown", ".txt", ".rst", ".adoc", ".org"}
RICH_EXT = {".pdf", ".docx", ".odt", ".ods", ".odp", ".pptx", ".xlsx", ".epub", ".rtf", ".html", ".htm", ".xhtml"}
CODE_EXT = {".py", ".pyi", ".ipynb", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".vue", ".svelte", ".java", ".kt",
            ".kts", ".scala", ".groovy", ".gradle", ".c", ".h", ".cc", ".cpp", ".hpp", ".cs", ".go", ".rs", ".rb",
            ".php", ".swift", ".m", ".mm", ".dart", ".lua", ".r", ".jl", ".pl", ".sh", ".bash", ".zsh", ".ps1",
            ".bat", ".cmd", ".sql", ".graphql", ".proto", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg",
            ".conf", ".properties", ".xml", ".csv", ".tsv", ".css", ".scss", ".sass", ".less", ".tex", ".el",
            ".clj", ".ex", ".exs", ".erl", ".hs", ".ml", ".fs", ".vb", ".asm", ".cmake", ".tf", ".nix"}
PLAIN_NAMES = {"dockerfile", "makefile", "cmakelists.txt", "readme", "license", "licence", "changelog", "authors",
               "contributing", "procfile", "gemfile", "rakefile", "vagrantfile", "jenkinsfile", ".gitignore",
               ".dockerignore", ".editorconfig"}
SKIP_DIRS = {".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv", "venv", ".tox", ".mypy_cache",
             ".pytest_cache", ".ruff_cache", ".idea", ".vscode", ".gradle", ".next", ".nuxt", "__MACOSX", "dist",
             "build", "target", "obj", ".dart_tool", "Pods", "DerivedData"}
SKIP_FILES = {".ds_store", "thumbs.db", "desktop.ini"}
SECRET_RE = re.compile(r"(^|/)(\.env(\..*)?|.*\.(pem|key|p12|pfx|keystore|jks)|id_(rsa|ed25519|ecdsa|dsa))$", re.I)


def _slug(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", Path(name).stem).strip("._-")
    return (s or "proyecto")[:60]


def _clean_name(raw: str) -> PurePosixPath | None:
    """Ruta relativa segura dentro del ZIP, o None si es peligrosa (zip slip, absoluta, unidad de Windows)."""
    name = raw.replace("\\", "/")
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        return None
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts):
        return None
    return PurePosixPath(*parts)


NOISE_RE = re.compile(r"\.min\.(js|css)$|\.map$|(^|/)(package-lock\.json|yarn\.lock|pnpm-lock\.yaml|poetry\.lock|"
                      r"Cargo\.lock|composer\.lock|Gemfile\.lock)$|(^|/)(vendor|third_party)/", re.I)


def _indexable(rel: PurePosixPath) -> bool:
    if SECRET_RE.search(str(rel)):
        return False   # no metemos claves ni .env en la biblioteca (siguen en el sandbox)
    if NOISE_RE.search(str(rel)):
        return False   # minificados, mapas, lockfiles y codigo de terceros: ruido para la busqueda
    return rel.suffix.lower() in DOC_EXT | CODE_EXT or rel.name.lower() in PLAIN_NAMES


def safe_extract(zip_path: Path, dest: Path) -> dict:
    """Descomprime con limites. Devuelve {'files': [rutas relativas], 'skipped': [(ruta, motivo)]}."""
    files, skipped, total = [], [], 0
    with zipfile.ZipFile(zip_path) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        cleaned = [(_clean_name(i.filename), i) for i in infos]
        # un ZIP con una unica carpeta raiz no anida proyectos/x/x
        tops = {c.parts[0] for c, _ in cleaned if c is not None and len(c.parts) > 1}
        strip = len(tops) == 1 and all(c is None or len(c.parts) > 1 for c, _ in cleaned)
        for rel, info in cleaned:
            if rel is None:
                skipped.append((info.filename, "ruta no permitida"))
                continue
            if strip:
                rel = PurePosixPath(*rel.parts[1:])
            if any(p in SKIP_DIRS for p in rel.parts[:-1]) or rel.name.lower() in SKIP_FILES:
                continue
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode):
                skipped.append((str(rel), "enlace simbolico"))
                continue
            if info.flag_bits & 0x1:
                skipped.append((str(rel), "cifrado"))
                continue
            if info.file_size > MAX_FILE:
                skipped.append((str(rel), "demasiado grande"))
                continue
            if info.file_size > 1024 * 1024 and info.file_size > MAX_RATIO * max(info.compress_size, 1):
                skipped.append((str(rel), "compresion sospechosa"))
                continue
            if len(files) >= MAX_FILES:
                skipped.append((str(rel), "limite de ficheros"))
                continue
            if total + info.file_size > MAX_TOTAL:
                skipped.append((str(rel), "limite de tamano total"))
                continue
            out = (dest / Path(*rel.parts)).resolve()
            if not out.is_relative_to(dest.resolve()):
                skipped.append((str(rel), "ruta no permitida"))
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            written = 0
            with z.open(info) as src, open(out, "wb") as dst:   # no nos fiamos del tamano declarado
                while chunk := src.read(1024 * 1024):
                    written += len(chunk)
                    if written > MAX_FILE or total + written > MAX_TOTAL:
                        break
                    dst.write(chunk)
            if written > MAX_FILE or total + written > MAX_TOTAL:
                out.unlink(missing_ok=True)
                skipped.append((str(rel), "tamano real excesivo"))
                continue
            total += written
            files.append(rel)
    return {"files": sorted(files), "skipped": skipped, "bytes": total}


def _accept(rel: PurePosixPath | None, raw: str, size: int, state: dict, skipped: list) -> bool:
    """Reglas comunes de todos los formatos (antes de escribir nada)."""
    if rel is None:
        skipped.append((raw, "ruta no permitida"))
        return False
    if any(p in SKIP_DIRS for p in rel.parts[:-1]) or rel.name.lower() in SKIP_FILES:
        return False
    if size > MAX_FILE:
        skipped.append((str(rel), "demasiado grande"))
        return False
    if state["count"] >= MAX_FILES:
        skipped.append((str(rel), "limite de ficheros"))
        return False
    if state["total"] + size > MAX_TOTAL:
        skipped.append((str(rel), "limite de tamano total"))
        return False
    return True


def _strip_root(rels: list[PurePosixPath | None]) -> bool:
    """True si todo cuelga de una unica carpeta raiz (proyecto-main/...): no anidamos proyectos/x/x."""
    tops = {c.parts[0] for c in rels if c is not None and len(c.parts) > 1}
    return len(tops) == 1 and all(c is None or len(c.parts) > 1 for c in rels)


def _copy_stream(src, out: Path, state: dict) -> int | None:
    """Copia limitada (no nos fiamos del tamano declarado). Devuelve los bytes o None si se pasa."""
    out.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with open(out, "wb") as dst:
        while chunk := src.read(1024 * 1024):
            written += len(chunk)
            if written > MAX_FILE or state["total"] + written > MAX_TOTAL:
                break
            dst.write(chunk)
    if written > MAX_FILE or state["total"] + written > MAX_TOTAL:
        out.unlink(missing_ok=True)
        return None
    return written


def safe_extract_tar(path: Path, dest: Path) -> dict:
    """tar, tar.gz, tar.bz2, tar.xz con los mismos limites; solo ficheros normales."""
    files, skipped, state = [], [], {"count": 0, "total": 0}
    with tarfile.open(path) as t:
        members = t.getmembers()
        cleaned = [(_clean_name(m.name), m) for m in members if not m.isdir()]
        strip = _strip_root([c for c, _ in cleaned])
        root = dest.resolve()
        for rel, m in cleaned:
            if rel is not None and strip:
                rel = PurePosixPath(*rel.parts[1:])
            if m.issym() or m.islnk():
                skipped.append((str(rel or m.name), "enlace simbolico"))
                continue
            if not m.isfile():
                skipped.append((str(rel or m.name), "no es un fichero normal"))
                continue
            if not _accept(rel, m.name, m.size, state, skipped):
                continue
            out = (dest / Path(*rel.parts)).resolve()
            if not out.is_relative_to(root):
                skipped.append((str(rel), "ruta no permitida"))
                continue
            src = t.extractfile(m)
            written = _copy_stream(src, out, state) if src else None
            if written is None:
                skipped.append((str(rel), "tamano real excesivo"))
                continue
            state["total"] += written
            state["count"] += 1
            files.append(rel)
    return {"files": sorted(files), "skipped": skipped, "bytes": state["total"]}


# ------------------------------------------------------------------------------------- RAR / 7z (externo)
def _which(*names: str) -> str:
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    if os.name == "nt":
        for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", "")):
            for exe in ("7-Zip\\7z.exe", "WinRAR\\UnRAR.exe"):
                p = Path(base) / exe if base else None
                if p and p.exists():
                    return str(p)
    return ""


def _is_bsdtar(exe: str) -> bool:
    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=10, stdin=subprocess.DEVNULL)
        return "bsdtar" in (out.stdout + out.stderr).lower() or "libarchive" in (out.stdout + out.stderr).lower()
    except Exception:  # noqa: BLE001
        return False


_RAR_CODEC: dict[str, bool] = {}


def _7z_reads_rar(exe: str) -> bool:
    """El 7-Zip de Debian/Ubuntu lista el formato RAR pero no trae sus codecs: falla con 'Unsupported Method'."""
    if exe not in _RAR_CODEC:
        try:
            out = subprocess.run([exe, "i"], capture_output=True, text=True, timeout=15, errors="replace").stdout
        except (OSError, subprocess.SubprocessError):
            out = ""
        codecs = out.split("Codecs:", 1)[1] if "Codecs:" in out else ""
        _RAR_CODEC[exe] = not out or bool(re.search(r"\bRar[1-5]\b", codecs)) or "Codecs:" not in out
    return _RAR_CODEC[exe]


def find_extractor(kind: str) -> tuple[str, str]:
    """(herramienta, ruta) para 'rar' o '7z', o ('', '') si no hay ninguna.
    Para RAR va primero unrar: el paquete 7zip de Debian/Ubuntu no trae el codec RAR."""
    order = ["unrar", "7z", "bsdtar", "unar"] if kind == "rar" else ["7z", "bsdtar", "unar"]
    for tool in order:
        if tool == "unrar":
            exe = _which("unrar", "UnRAR.exe")
            if exe and "unrar" in Path(exe).name.lower():
                return "unrar", exe
        elif tool == "7z":
            exe = _which("7zz", "7z") or (_which("7za") if kind == "7z" else "")
            if exe and (kind != "rar" or _7z_reads_rar(exe)):
                return "7z", exe
        elif tool == "bsdtar":
            for cand in ("bsdtar", "tar"):
                exe = _which(cand)
                if exe and _is_bsdtar(exe):
                    return "bsdtar", exe
        elif tool == "unar":
            exe = _which("unar")
            if exe:
                return "unar", exe
    return "", ""


INSTALL_HINT = {
    "rar": "Instala una herramienta para RAR: Windows: 7-Zip (7-zip.org) o WinRAR; Linux/WSL: sudo apt install unrar "
           "(el paquete 7zip de Debian/Ubuntu no lee RAR); macOS: brew install sevenzip o unar; Termux: pkg install unrar. "
           "En Windows 10/11 y macOS tambien vale el 'tar' del sistema.",
    "7z": "Instala 7-Zip (Windows: 7-zip.org; Linux/WSL: sudo apt install 7zip; macOS: brew install sevenzip; Termux: pkg install p7zip).",
}


def _listing(tool: str, exe: str, path: Path) -> dict:
    """{'count', 'bytes', 'encrypted': set(rutas)} segun el listado de la herramienta (antes de extraer)."""
    info = {"count": 0, "bytes": 0, "encrypted": set()}
    try:
        if tool == "7z":
            out = subprocess.run([exe, "l", "-slt", "-pNEXUS_SIN_CLAVE", str(path)], capture_output=True, text=True,
                                 timeout=120, stdin=subprocess.DEVNULL, errors="replace").stdout
            for b in out.split("\n\n"):
                name, size = re.search(r"^Path = (.+)$", b, re.M), re.search(r"^Size = (\d+)", b, re.M)
                if not name or not re.search(r"^Folder = -", b, re.M):
                    continue
                info["count"] += 1
                info["bytes"] += int(size.group(1)) if size else 0
                if re.search(r"^Encrypted = \+", b, re.M):
                    info["encrypted"].add(name.group(1).strip().replace("\\", "/"))
        elif tool == "unrar":
            out = subprocess.run([exe, "lt", "-p-", str(path)], capture_output=True, text=True, timeout=120,
                                 stdin=subprocess.DEVNULL, errors="replace").stdout
            for b in re.split(r"\n\s*\n", out):
                name, size = re.search(r"^\s*Name: (.+)$", b, re.M), re.search(r"^\s*Size: (\d+)", b, re.M)
                if not name or re.search(r"^\s*Type: Directory", b, re.M):
                    continue
                info["count"] += 1
                info["bytes"] += int(size.group(1)) if size else 0
                if re.search(r"^\s*Flags:.*encrypted", b, re.M):
                    info["encrypted"].add(name.group(1).strip().replace("\\", "/"))
        elif tool == "bsdtar":
            out = subprocess.run([exe, "-tvf", str(path)], capture_output=True, text=True, timeout=120,
                                 stdin=subprocess.DEVNULL, errors="replace").stdout
            sizes = [int(p[4]) for p in (ln.split() for ln in out.splitlines()) if len(p) > 4 and p[4].isdigit()]
            info["count"], info["bytes"] = len(sizes), sum(sizes)
    except Exception:  # noqa: BLE001
        pass
    return info


def safe_extract_external(path: Path, dest: Path, kind: str) -> dict:
    """RAR/7z con la herramienta del sistema, en cuarentena; despues se validan y mueven los ficheros."""
    tool, exe = find_extractor(kind)
    if not tool:
        raise ValueError(f"no hay ninguna herramienta para descomprimir {kind.upper()}. {INSTALL_HINT[kind]}")
    listed = _listing(tool, exe, path)
    if listed["bytes"] > MAX_TOTAL * 4:
        raise ValueError(f"el archivo descomprimido ocuparia {listed['bytes'] / 1e6:.0f} MB (limite {MAX_TOTAL / 1e6:.0f} MB)")
    tmp = dest.parent / f".cuarentena-{secrets.token_hex(4)}"
    tmp.mkdir(parents=True)
    try:
        cmd = {"7z": [exe, "x", "-y", "-bd", "-pNEXUS_SIN_CLAVE", f"-o{tmp}", str(path)],
               "unrar": [exe, "x", "-y", "-p-", "-idq", str(path), str(tmp) + os.sep],
               "bsdtar": [exe, "-xf", str(path), "-C", str(tmp)],
               "unar": [exe, "-q", "-f", "-D", "-p", "NEXUS_SIN_CLAVE", "-o", str(tmp), str(path)]}[tool]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900, stdin=subprocess.DEVNULL, errors="replace")
        problems = (proc.stderr or proc.stdout or "").strip().splitlines()[-1:] if proc.returncode else []
        # ficheros cifrados o que fallaron al extraer: la herramienta deja ficheros vacios o a medias
        log_ = f"{proc.stdout or ''}\n{proc.stderr or ''}"
        bad = set(listed["encrypted"]) | {m.strip().replace("\\", "/") for m in
                                          re.findall(r"^ERROR: [^:\n]+ : (.+)$", log_, re.M)}
        bad |= {m.strip().replace("\\", "/") for m in re.findall(r"^\s+(.+?)\s+\([^)]*\)\.\.\. Failed!", log_, re.M)}
        if tool == "bsdtar":   # "<fichero>: <error>" (cifrado o dañado: deja el fichero a medias)
            bad |= {m.strip() for m in re.findall(r"^(?!bsdtar: )([^:\n]+): ", proc.stderr or "", re.M)}
        files, skipped, state = [], [], {"count": 0, "total": 0}
        found: list[tuple[PurePosixPath | None, Path]] = []
        root = tmp.resolve()
        for dirpath, dirnames, filenames in os.walk(tmp, followlinks=False):
            for name in filenames + [d for d in dirnames if Path(dirpath, d).is_symlink()]:
                p = Path(dirpath, name)
                rel = PurePosixPath(*p.relative_to(tmp).parts)
                if p.is_symlink():
                    skipped.append((str(rel), "enlace simbolico"))
                    continue
                if not p.is_file() or not p.resolve().is_relative_to(root):
                    skipped.append((str(rel), "ruta no permitida"))
                    continue
                if str(rel) in bad:
                    skipped.append((str(rel), "cifrado o dañado"))
                    continue
                found.append((_clean_name(str(rel)), p))
        found.sort(key=lambda x: str(x[0]))
        strip = _strip_root([r for r, _ in found])
        droot = dest.resolve()
        for rel, p in found:
            if rel is not None and strip:
                rel = PurePosixPath(*rel.parts[1:])
            size = p.stat().st_size
            if not _accept(rel, str(p), size, state, skipped):
                continue
            out = (dest / Path(*rel.parts)).resolve()
            if not out.is_relative_to(droot):
                skipped.append((str(rel), "ruta no permitida"))
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            os.replace(p, out)
            state["total"] += size
            state["count"] += 1
            files.append(rel)
        if problems:
            skipped.append(("(archivo)", f"{tool}: {problems[0][:150]} (¿ficheros cifrados o dañados?)"))
        if not files and proc.returncode:
            raise ValueError(f"no se pudo descomprimir el {kind.upper()}: {problems[0][:200] if problems else proc.returncode}")
        return {"files": sorted(files), "skipped": skipped, "bytes": state["total"], "tool": tool}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def detect_kind(path: Path, name: str = "") -> str:
    """'zip' | 'rar' | '7z' | 'tar' | '' por la firma del fichero (y si no, por la extension)."""
    with open(path, "rb") as f:
        head = f.read(8)
    if head.startswith(b"Rar!\x1a\x07"):
        return "rar"
    if head.startswith(b"7z\xbc\xaf\x27\x1c"):
        return "7z"
    if zipfile.is_zipfile(path):
        return "zip"
    try:
        if tarfile.is_tarfile(path):
            return "tar"
    except Exception:  # noqa: BLE001
        pass
    suf = archive_suffix(name or path.name)
    return {".rar": "rar", ".7z": "7z"}.get(suf, "")


def extract_any(path: Path, dest: Path, kind: str) -> dict:
    if kind == "zip":
        return safe_extract(path, dest)
    if kind == "tar":
        return safe_extract_tar(path, dest)
    return safe_extract_external(path, dest, kind)


def tree_text(files: list[PurePosixPath], limit: int = 400) -> str:
    lines, seen = [], set()
    for f in files:
        for depth in range(1, len(f.parts)):
            d = f.parts[:depth]
            if d not in seen:
                seen.add(d)
                lines.append("  " * (depth - 1) + d[-1] + "/")
        lines.append("  " * (len(f.parts) - 1) + f.name)
        if len(lines) >= limit:
            lines.append(f"… ({len(files)} ficheros en total)")
            break
    return "\n".join(lines)


def _read_for_index(path: str) -> tuple[list[str], str]:
    """(paginas, error). De nivel de modulo para leer varios ficheros a la vez en distintos nucleos."""
    p = Path(path)
    try:
        if p.suffix.lower() in RICH_EXT:
            return read_document(p)[1], ""
        data = p.read_bytes()[:MAX_INDEX_FILE]
        if b"\x00" in data[:4096]:
            return [], ""   # binario con extension de texto
        return [data.decode("utf-8", errors="replace")], ""
    except Exception as e:  # noqa: BLE001
        return [], f"{type(e).__name__}: {e}"[:160]


LABEL = {"zip": "ZIP", "rar": "RAR", "7z": "7z", "tar": "tar"}


def ingest_archive(rag, src: Path, title: str = "", source: str = "", meta: dict | None = None, agent: str = "") -> dict:
    src = Path(src)
    kind = detect_kind(src, source or src.name)
    if not kind:
        suf = archive_suffix(source or src.name)
        raise ValueError(f"el fichero no es un {LABEL.get(suf.lstrip('.'), 'ZIP')} valido" if suf not in (".rar", ".7z")
                         else f"el fichero no es un {suf[1:].upper()} valido")
    stored = rag.files / f"{int(time.time())}_{re.sub(r'[^A-Za-z0-9._-]+', '_', src.name)[-80:]}"
    if stored != src:
        shutil.copy2(src, stored)
    base = paths.sub("sandbox", "proyectos")
    slug = _slug(re.sub(r"\.tar$", "", Path(source or src.name).stem) + ".x")
    dest, n = base / slug, 2
    while dest.exists():
        dest, n = base / f"{slug}_{n}", n + 1
    dest.mkdir(parents=True)
    try:
        res = extract_any(stored, dest, kind)
    except Exception:
        shutil.rmtree(dest, ignore_errors=True)
        stored.unlink(missing_ok=True)
        raise
    files = res["files"]
    rel_dest = dest.relative_to(paths.sandbox_dir()).as_posix()
    title = title or re.sub(r"\.tar$", "", Path(source or src.name).stem)
    tree = tree_text(files)

    pages = [f"Proyecto «{title}» importado desde {source or src.name}: {len(files)} ficheros. "
             f"Esta en el sandbox en {rel_dest}/ (usa file_list/file_read con esa ruta).\n\n"
             f"Estructura:\n{tree}"]
    labels = [f"[{title} · estructura]"]
    # lectura en paralelo (PDF, Office, EPUB... en todos los nucleos) por grupos, aplicada en orden. Si el proyecto
    # no cabe en un documento, se reparte en partes correlativas: «Proyecto · parte 2 (ficheros 1201-2400)», etc.
    cands = [rel for rel in files if _indexable(rel)][:MAX_INDEX_FILES]
    indexed, errors, parts_out = [], [], []
    chars, first_file = 0, 1

    def flush(final: bool = False) -> None:
        nonlocal pages, labels, chars, first_file
        if not pages and not final:
            return
        n = len(parts_out) + 1
        last_file = len(indexed)
        info = {**(meta or {}), "archive": source or src.name, "format": kind, "sandbox": rel_dest,
                "files": len(files), "indexed": len(indexed), "skipped": len(res["skipped"]), "bytes": res["bytes"],
                "skipped_detail": [f"{p}: {m}" for p, m in res["skipped"][:50]] + errors[:20]}
        if res.get("tool"):
            info["tool"] = res["tool"]
        part_title = title
        if n > 1 or not final:
            part_title = f"{title} · parte {n} (ficheros {first_file}-{last_file})"
            info.update(part=n, file_from=first_file, file_to=last_file)
        if n > 1:   # solo la primera parte es duena del sandbox (al borrarla se borran las demas)
            info.pop("sandbox", None)
            info["archive_root"] = parts_out[0]["id"]
        if pages:
            parts_out.append(rag.ingest_pages(part_title, pages, source or src.name, kind,
                                              str(stored) if n == 1 else "", info, labels=labels, agent=agent))
        pages, labels, chars, first_file = [], [], 0, last_file + 1

    try:
        for g in range(0, len(cands), READ_GROUP):
            group = cands[g:g + READ_GROUP]
            paths_ = [str(dest / Path(*rel.parts)) for rel in group]
            heavy = sum(1 for r in group if r.suffix.lower() in RICH_EXT)
            results = parallel.map_cpu(_read_for_index, paths_, min_items=2) if heavy >= 2 else [_read_for_index(x) for x in paths_]
            for rel, (texts, err) in zip(group, results):
                if err:
                    L.info("%s: no se pudo leer %s: %s", LABEL[kind], rel, err)
                    errors.append(f"{rel}: {err}")
                    continue
                texts = [p for p in texts if p.strip()]
                if not texts:
                    continue
                size = sum(len(p) for p in texts)
                if chars and chars + size > MAX_INDEX_CHARS:
                    flush()
                for i, p in enumerate(texts):
                    pages.append(p)
                    labels.append(f"[{rel_dest}/{rel}" + (f" p.{i + 1}" if len(texts) > 1 else "") + "]")
                    chars += len(p)
                indexed.append(str(rel))
        flush(final=True)
    except Exception:
        for d in parts_out:
            rag.delete(d["id"])
        shutil.rmtree(dest, ignore_errors=True)
        stored.unlink(missing_ok=True)
        raise
    doc = parts_out[0]
    if len(parts_out) > 1:
        ids = [d["id"] for d in parts_out]
        meta0 = {**doc["meta"], "parts": len(ids), "part_ids": ids}
        rag.db.execute("UPDATE rag_docs SET meta=? WHERE id=?", (json.dumps(meta0, ensure_ascii=False), doc["id"]))
        doc = rag.get(doc["id"])
    L.info("%s '%s': %d ficheros en %s, %d indexados, %d omitidos", LABEL[kind], title, len(files), rel_dest,
           len(indexed), len(res["skipped"]))
    return doc


ingest_zip = ingest_archive   # nombre anterior
