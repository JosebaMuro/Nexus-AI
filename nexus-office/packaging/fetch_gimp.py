"""GIMP portable para los paquetes offline de Nexus-Office (bundled/runtimes/gimp).

Descarga la version estable mas reciente de GIMP 3 de download.gimp.org (los paquetes oficiales), la deja lista
para usarse sin instalar y sin ventana, y la prueba con el motor de Nexus-Office (nexus/media/gimp.py):

    Linux x86_64 / aarch64   AppImage oficial extraido (unsquashfs o --appimage-extract) + lanzador
                             nexus-gimp-console que ejecuta gimp-console con el entorno del AppImage
    Windows x64 / ARM64      instalador oficial en modo silencioso a una carpeta (necesita Windows)
    macOS arm64 / x86_64     GIMP.app copiado del DMG oficial (necesita macOS: hdiutil)

    python packaging/fetch_gimp.py --target linux-x86_64 [--out dist-gimp] [--smoke] [--version 3.0.4]

Deja .cache/desktop-runtimes/<target>/gimp/ (lo que copia build.py) y, con --out, gimp-<target>.tar.gz (o .zip en
Windows) para publicarlo. build.py tambien sabe descargar ese archivo de la Release «gimp-runtimes» del repositorio
cuando la carpeta no esta (lo construye .github/workflows/gimp-runtimes.yml en cada sistema).

GIMP es software libre (GPL v3): el paquete lleva sus licencias y el LEEME indica donde esta su codigo fuente
(https://download.gimp.org/gimp/).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = "https://download.gimp.org/gimp/"
VERSIONS_JSON = "https://www.gimp.org/gimp_versions.json"
RELEASE = os.environ.get("NEXUS_GIMP_RELEASE", "https://github.com/JosebaMuro/Nexus-AI/releases/download/gimp-runtimes/")
DESKTOP_RT = ROOT / ".cache" / "desktop-runtimes"
CACHE = ROOT / ".cache" / "gimp-downloads"
_V = r"(\d+\.\d+\.\d+)(?:-(\d+))?"
ASSETS = {   # carpeta en download.gimp.org/gimp/vX.Y/ y patron del fichero
    "windows-x64": ("windows", rf"gimp-{_V}-setup(?:-\d+)?\.exe"),
    "windows-arm64": ("windows", rf"gimp-{_V}-setup(?:-\d+)?\.exe"),
    "macos-arm64": ("macos", rf"gimp-{_V}-arm64\.dmg"),
    "macos-x86_64": ("macos", rf"gimp-{_V}-x86_64\.dmg"),
    "linux-x86_64": ("linux", rf"GIMP-{_V}-x86_64\.AppImage"),
    "linux-aarch64": ("linux", rf"GIMP-{_V}-aarch64\.AppImage"),
}


def log(msg: str) -> None:
    print(f"[gimp] {msg}", flush=True)


def _get(url: str, timeout: int = 60) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "nexus-office-packaging"})
    with urllib.request.urlopen(req, timeout=timeout) as r:  # noqa: S310
        return r.read()


def _links(url: str) -> list[str]:
    return re.findall(r'href="([^"?/][^"]*)"', _get(url).decode("utf-8", "replace"))


def _vkey(v: str, rev: str | None = None) -> tuple:
    return (*[int(x) for x in v.split(".")], int(rev or 0))


# ------------------------------------------------------------------ descarga
def find_asset(target: str, version: str = "") -> tuple[str, str, str]:
    """(url, nombre, version) del paquete oficial mas reciente de GIMP 3 (o de ``version``) para el destino."""
    sub, pat = ASSETS[target]
    found_series = (re.fullmatch(r"v(\d+)\.(\d+)/?", h) for h in _links(BASE))
    series = sorted({(int(m.group(1)), int(m.group(2))) for m in found_series if m}, reverse=True)
    if version:
        series = [tuple(int(x) for x in version.split(".")[:2])]
    for major, minor in series:
        if major < 3:
            continue
        folder = f"{BASE}v{major}.{minor}/{sub}/"
        try:
            names = _links(folder)
        except OSError:
            continue
        found = []
        for n in names:
            m = re.fullmatch(pat, n.rsplit("/", 1)[-1], re.I)
            if m and (not version or m.group(1) == version):
                found.append((_vkey(m.group(1), m.group(2)), n.rsplit("/", 1)[-1], m.group(1)))
        if found:
            _, name, ver = max(found)
            return folder + name, name, ver
    raise RuntimeError(f"no hay un paquete de GIMP 3 para {target} en {BASE}")


def _checksums() -> dict[str, str]:
    """{fichero: sha256} de gimp_versions.json (la pagina de descargas de gimp.org)."""
    out: dict[str, str] = {}
    try:
        data = json.loads(_get(VERSIONS_JSON))
    except (OSError, ValueError) as e:
        log(f"sin {VERSIONS_JSON}: {e}")
        return out

    def walk(o):
        if isinstance(o, dict):
            name = o.get("filename") or o.get("file")
            if isinstance(name, str) and isinstance(o.get("sha256"), str):
                out[name.rsplit("/", 1)[-1]] = o["sha256"].lower()
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(data)
    return out


def download(target: str, version: str = "") -> tuple[Path, str]:
    url, name, ver = find_asset(target, version)
    CACHE.mkdir(parents=True, exist_ok=True)
    dest = CACHE / name
    if not dest.exists():
        log(f"descargando {url}")
        tmp = dest.with_suffix(dest.suffix + ".part")
        req = urllib.request.Request(url, headers={"User-Agent": "nexus-office-packaging"})
        with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:  # noqa: S310
            shutil.copyfileobj(r, f, 1 << 20)
        tmp.replace(dest)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    expected = _checksums().get(name)
    if expected and expected != digest:
        dest.unlink()
        raise RuntimeError(f"SHA256 de {name} no coincide con gimp.org ({digest} != {expected})")
    log(f"{name}: {dest.stat().st_size >> 20} MB, sha256 {digest} "
        f"({'verificado con gimp.org' if expected else 'gimp.org no publica la suma de este fichero'})")
    return dest, ver


# ------------------------------------------------------------------ preparar
def _squashfs_offset(appimage: Path) -> int:
    """El sistema de ficheros de un AppImage tipo 2 empieza donde acaba el ELF del runtime."""
    with open(appimage, "rb") as f:
        head = f.read(64)
    if head[:4] != b"\x7fELF":
        raise RuntimeError("no es un AppImage")
    if head[4] == 2:   # ELF de 64 bits
        shoff, = struct.unpack_from("<Q", head, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", head, 0x3A)
    else:
        shoff, = struct.unpack_from("<I", head, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", head, 0x2E)
    return shoff + shentsize * shnum


LAUNCHER_FALLBACK = """#!/bin/sh
# Lanzador de Nexus-Office: gimp-console del AppImage oficial de GIMP, sin ventana.
HERE="$(dirname "$(readlink -f "$0")")"
export APPDIR="$HERE"
TRIPLET="$(ls "$HERE/usr/lib" | grep -m1 -- '-linux-gnu$')"
export LD_LIBRARY_PATH="$HERE/usr/lib:$HERE/usr/lib/$TRIPLET${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GI_TYPELIB_PATH="$HERE/usr/lib/$TRIPLET/girepository-1.0:$HERE/usr/lib/girepository-1.0"
export GIMP3_DATADIR="$HERE/usr/share/gimp/3.0"
export GIMP3_SYSCONFDIR="$HERE/etc/gimp/3.0"
export GIMP3_PLUGINDIR="$(ls -d "$HERE"/usr/lib/*/gimp/3.0 "$HERE"/usr/lib/gimp/3.0 2>/dev/null | head -n1)"
export GIMP3_LOCALEDIR="$HERE/usr/share/locale"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/usr/bin/@CONSOLE@" "$@"
"""


def _linux_launcher(root: Path) -> Path:
    """nexus-gimp-console: el AppRun oficial cambiando el binario con interfaz por gimp-console (el de interfaz no
    arranca sin pantalla). Si AppRun no es un script, un entorno equivalente escrito a mano."""
    consoles = sorted((root / "usr" / "bin").glob("gimp-console*"), key=lambda p: -len(p.name))
    if not consoles:
        raise RuntimeError("el AppImage no trae gimp-console")
    console = consoles[0].name
    apprun = root / "AppRun"
    text = ""
    if apprun.exists() and not apprun.is_symlink():
        raw = apprun.read_bytes()
        if not raw.startswith(b"\x7fELF"):
            text = raw.decode("utf-8", "replace")
            log("AppRun del AppImage de GIMP:\n" + text)
    if text and re.search(r"gimp-\d", text):
        new = re.sub(r"\bgimp-(\d+(?:\.\d+)?)\b(?!-)", r"gimp-console-\1", text)
        script = new.replace('"$APPDIR/usr/bin/gimp"', f'"$APPDIR/usr/bin/{console}"')
    else:
        script = LAUNCHER_FALLBACK.replace("@CONSOLE@", console)
    out = root / "nexus-gimp-console"
    out.write_text(script, encoding="utf-8")
    out.chmod(0o755)
    return out


def prepare_linux(appimage: Path, dest: Path) -> None:
    shutil.rmtree(dest, ignore_errors=True)
    if shutil.which("unsquashfs"):
        off = _squashfs_offset(appimage)
        subprocess.run(["unsquashfs", "-q", "-n", "-d", str(dest), "-o", str(off), str(appimage)], check=True)
    else:
        appimage.chmod(0o755)
        with tempfile.TemporaryDirectory() as t:
            subprocess.run([str(appimage), "--appimage-extract"], cwd=t, check=True, stdout=subprocess.DEVNULL)
            shutil.move(str(Path(t) / "squashfs-root"), dest)
    _linux_launcher(dest)


def prepare_windows(setup: Path, dest: Path) -> None:
    if os.name != "nt":
        raise RuntimeError("el instalador de GIMP para Windows se prepara en Windows (ver gimp-runtimes.yml)")
    shutil.rmtree(dest, ignore_errors=True)
    subprocess.run([str(setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/CURRENTUSER",
                    "/NOICONS", f"/DIR={dest}", f"/LOG={dest.parent / 'gimp-setup.log'}"], check=True)
    for _ in range(120):   # el instalador puede devolver el control antes de terminar de copiar
        if list(dest.glob("bin/gimp-console*.exe")):
            break
        time.sleep(1)
    for f in dest.glob("unins*"):   # el desinstalador no sirve en una copia portable
        f.unlink()


def prepare_macos(dmg: Path, dest: Path) -> None:
    if sys.platform != "darwin":
        raise RuntimeError("el DMG de GIMP se prepara en macOS (ver gimp-runtimes.yml)")
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    mnt = Path(tempfile.mkdtemp())
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mnt), str(dmg)], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        app = next(mnt.glob("GIMP*.app"))
        subprocess.run(["ditto", str(app), str(dest / "GIMP.app")], check=True)
    finally:
        subprocess.run(["hdiutil", "detach", str(mnt)], check=False, stdout=subprocess.DEVNULL)
    subprocess.run(["xattr", "-dr", "com.apple.quarantine", str(dest)], check=False)


def prepare(target: str, version: str = "") -> Path:
    pkg, ver = download(target, version)
    dest = DESKTOP_RT / target / "gimp"
    dest.parent.mkdir(parents=True, exist_ok=True)
    {"linux": prepare_linux, "windows": prepare_windows, "macos": prepare_macos}[target.split("-")[0]](pkg, dest)
    (dest / "NEXUS-GIMP.txt").write_text(
        f"GIMP {ver} oficial ({pkg.name}) para Nexus-Office {target}.\nGIMP es software libre (GNU GPL v3). "
        "Codigo fuente: https://download.gimp.org/gimp/ . Licencias: ver los ficheros de licencia de esta carpeta.\n",
        encoding="utf-8")
    size = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file() and not f.is_symlink())
    log(f"GIMP {ver} listo en {dest} ({size >> 20} MB)")
    return dest


# ------------------------------------------------------------------ prueba y archivo
def smoke(bundle: Path) -> dict:
    """Edita una imagen con el GIMP de ``bundle`` usando el motor de Nexus-Office (todas las operaciones)."""
    sys.path.insert(0, str(ROOT))
    os.environ["NEXUS_DATA_DIR"] = tempfile.mkdtemp(prefix="nexus-gimp-")
    from PIL import Image, ImageDraw

    from nexus.media import gimp
    gimp._bundled_dir = lambda: bundle   # noqa: SLF001
    info = gimp.find(None, refresh=True)
    cands = gimp.candidates(None)
    log(f"candidatos: {cands}")
    if not info or info["origen"] != "incluido":
        for cmd, _ in cands:
            try:
                r = subprocess.run([*cmd, "--version"], capture_output=True, text=True, timeout=120)
                log(f"{cmd} --version -> {r.returncode}\n{r.stdout[-1500:]}\n{r.stderr[-3000:]}")
            except Exception as e:  # noqa: BLE001
                log(f"{cmd}: {e}")
        raise RuntimeError(f"el GIMP incluido no arranca: {info}")
    work = Path(os.environ["NEXUS_DATA_DIR"])
    src = work / "foto.jpg"
    im = Image.new("RGB", (800, 600), (150, 110, 80))
    ImageDraw.Draw(im).ellipse((200, 120, 600, 480), fill=(230, 190, 60))
    im.save(src)
    eng = gimp.GimpEngine(None)
    ops = [{"op": "redimensionar", "ancho": 640}, {"op": "balance_blancos"}, {"op": "niveles_auto"},
           {"op": "saturacion", "cantidad": 15}, {"op": "enfocar"}, {"op": "reducir_ruido"}, {"op": "vineta"},
           {"op": "texto", "texto": "Año 2026 · Nexus-Office"}, {"op": "borde", "grosor": 10}, {"op": "ppp", "valor": 300}]
    t0 = time.time()
    res = eng.edit(src, ops, work / "out.jpg", engine="gimp", timeout=600)
    with Image.open(work / "out.jpg") as o:
        assert o.size == (660, 500), o.size
    xcf = eng.edit(src, [{"op": "rotar", "grados": 90}], work / "out.xcf", engine="gimp")
    assert (work / "out.xcf").read_bytes()[:8] == b"gimp xcf"
    log(f"prueba correcta: {info['version']} {res['motor']} en {time.time() - t0:.1f} s; {xcf['motor']}")
    return info


def archive(target: str, bundle: Path, out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    if target.startswith("windows"):
        dst = out / f"gimp-{target}.zip"
        with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for f in sorted(bundle.rglob("*")):
                if f.is_file():
                    z.write(f, f.relative_to(bundle))
    else:
        dst = out / f"gimp-{target}.tar.gz"
        with tarfile.open(dst, "w:gz", compresslevel=6) as t:
            t.add(bundle, arcname=".")
    log(f"-> {dst.name} ({dst.stat().st_size >> 20} MB)")
    return dst


def bundle_dir(target: str, desktop_rt: Path = DESKTOP_RT, download_ok: bool = True) -> Path | None:
    """Carpeta con el GIMP del destino para build.py: la preparada, un gimp-<target>.* suelto en
    .cache/desktop-runtimes/<target>/, o el de la Release «gimp-runtimes». None si no hay forma de tenerlo."""
    d = desktop_rt / target / "gimp"
    if d.is_dir() and any(d.iterdir()):
        return d
    ext = "zip" if target.startswith("windows") else "tar.gz"
    arc = desktop_rt / target / f"gimp-{target}.{ext}"
    if not arc.exists() and download_ok:
        try:
            log(f"descargando {RELEASE}gimp-{target}.{ext}")
            arc.parent.mkdir(parents=True, exist_ok=True)
            arc.write_bytes(_get(f"{RELEASE}gimp-{target}.{ext}", timeout=600))
        except OSError as e:
            log(f"sin GIMP para {target}: {e}")
            arc.unlink(missing_ok=True)
            return None
    if not arc.exists():
        return None
    d.mkdir(parents=True, exist_ok=True)
    if ext == "zip":
        with zipfile.ZipFile(arc) as z:
            z.extractall(d)
    else:
        with tarfile.open(arc) as t:
            t.extractall(d, filter="tar") if sys.version_info >= (3, 12) else t.extractall(d)  # noqa: S202
    return d


def host_target() -> str:
    m = platform.machine().lower()
    arm = m in ("arm64", "aarch64")
    if os.name == "nt":
        return "windows-arm64" if arm else "windows-x64"
    if sys.platform == "darwin":
        return "macos-arm64" if arm else "macos-x86_64"
    return "linux-aarch64" if arm else "linux-x86_64"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", default=host_target(), choices=sorted(ASSETS))
    ap.add_argument("--version", default="", help="version exacta de GIMP (por defecto la estable mas reciente)")
    ap.add_argument("--out", default="", help="carpeta donde dejar gimp-<target>.tar.gz/.zip")
    ap.add_argument("--smoke", action="store_true", help="probar la edicion con el GIMP preparado")
    a = ap.parse_args()
    bundle = prepare(a.target, a.version)
    if a.smoke:
        smoke(bundle)
    if a.out:
        archive(a.target, bundle, Path(a.out))


if __name__ == "__main__":
    main()
