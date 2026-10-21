"""Constructor de los instaladores de Nexus-Office. Todos son 100 % portables: llevan Python, las dependencias
y llama-server/sd-cli dentro, y se instalan sin internet (solo el modelo de IA se descarga, si hay red).

    python packaging/build.py                                  # todas las plataformas
    python packaging/build.py --targets windows-x64 linux-aarch64
    python packaging/build.py --apk dist-apk/*.apk             # anade las APK ya compiladas

Genera en dist/<grupo>/ (necesita .cache/desktop-runtimes: build_python_runtimes.py, build_desktop_runtimes.sh y
build_gpu_runtimes.py; y makensis para el .exe, que funciona tambien en Linux: apt install nsis):
  windows/  Nexus-Office-Setup-<ver>-windows-x64.exe / -arm64.exe   instalador grafico (NSIS)
  linux/    Nexus-Office-<ver>-linux-<x86_64|aarch64>.run            instalador autoextraible (Raspberry Pi: aarch64)
            Nexus-Office-<ver>-linux-<arch>.AppImage / .flatpak      (build_linux_bundles.py)
            nexus-office_<ver>_<amd64|arm64>.deb                    Debian/Ubuntu/Raspberry Pi OS (64 bits)
  macos/    Nexus-Office-<ver>-macos-<arm64|x86_64>.tar.gz            abre "Nexus-Office.command"
  android/  las APK de --apk
  Nexus-Office-<ver>-codigo-fuente.zip, SHA256SUMS.txt, LEEME.txt
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = Path(os.environ.get("NEXUS_DIST") or ROOT / "dist")
BUILD = ROOT / "build"
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "packaging"))
from nexus import __version__ as VERSION  # noqa: E402

COMMON = ["nexus", "vendor", "tests", "requirements.txt", "README.md", "LICENSE", "CHANGELOG.md", "docs"]
UNIX = ["install.sh", "start.sh"]
TARGETS = {
    "windows-x64": {"files": ["install.ps1", "install.bat", "start.bat"], "fmt": "zip", "pbs": "x86_64-pc-windows-msvc",
                    "pip": ["win_amd64"]},
    "windows-arm64": {"files": ["install.ps1", "install.bat", "start.bat"], "fmt": "zip", "pbs": "aarch64-pc-windows-msvc",
                      "pip": ["win_arm64"]},
    "linux-x86_64": {"files": UNIX + ["Dockerfile", ".dockerignore"], "fmt": "tar.gz", "pbs": "x86_64-unknown-linux-gnu",
                     "pip": ["manylinux_2_28_x86_64", "manylinux_2_17_x86_64", "manylinux2014_x86_64", "linux_x86_64"],
                     "deb": "amd64", "note": "PC Linux, Steam Deck (SteamOS), servidores"},
    "linux-aarch64": {"files": UNIX + ["Dockerfile", ".dockerignore"], "fmt": "tar.gz", "pbs": "aarch64-unknown-linux-gnu",
                      "pip": ["manylinux_2_28_aarch64", "manylinux_2_17_aarch64", "manylinux2014_aarch64", "linux_aarch64"],
                      "deb": "arm64", "note": "Raspberry Pi 4/5 (64 bits), NVIDIA Jetson, portatiles y consolas portatiles ARM"},
    "macos-arm64": {"files": UNIX, "fmt": "tar.gz", "pbs": "aarch64-apple-darwin",
                    "pip": ["macosx_11_0_arm64", "macosx_12_0_arm64", "macosx_13_0_arm64", "macosx_14_0_arm64", "macosx_10_9_universal2"],
                    "mac": True},
    "macos-x86_64": {"files": UNIX, "fmt": "tar.gz", "pbs": "x86_64-apple-darwin",
                     "pip": ["macosx_10_9_x86_64", "macosx_10_12_x86_64", "macosx_10_13_x86_64", "macosx_10_9_universal2"],
                     "mac": True},
}
GROUP = {"windows-x64": "windows", "windows-arm64": "windows", "linux-x86_64": "linux", "linux-aarch64": "linux",
         "macos-arm64": "macos", "macos-x86_64": "macos"}
EXCLUDE = re.compile(r"(__pycache__|\.pyc$|\.pytest_cache)|^(\.gradle|\.cache|build|local\.properties|keystore\.properties)$|\.jks$")


def log(msg: str) -> None:
    print(f"[build] {msg}", flush=True)


def stage(target: str) -> Path:
    t = TARGETS[target]
    dest = BUILD / target / "Nexus-Office"
    shutil.rmtree(dest.parent, ignore_errors=True)
    dest.mkdir(parents=True)
    for name in COMMON + t["files"]:
        src = ROOT / name
        if not src.exists():
            continue
        if src.is_dir():
            shutil.copytree(src, dest / name, ignore=lambda d, names: [n for n in names if EXCLUDE.search(n)])
        else:
            shutil.copy2(src, dest / name)
    if t.get("mac"):
        _mac_launchers(dest)
    (dest / "PLATAFORMA.txt").write_text(f"Nexus-Office {VERSION} - {target}\n{t.get('note', '')}\n", encoding="utf-8")
    return dest


def _mac_launchers(dest: Path) -> None:
    """Lanzador de doble clic (.command) y un bundle .app minimo que abre el Terminal."""
    cmd = dest / "Nexus-Office.command"
    cmd.write_text('#!/bin/bash\ncd "$(dirname "$0")"\n[ -x venv/bin/python3 ] || ./install.sh\nexec ./start.sh\n', encoding="utf-8")
    app = dest / "Nexus-Office.app" / "Contents"
    (app / "MacOS").mkdir(parents=True)
    (app / "Info.plist").write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Nexus-Office</string><key>CFBundleIdentifier</key><string>ai.nexus.office.portable</string>
<key>CFBundleVersion</key><string>{VERSION}</string><key>CFBundleExecutable</key><string>nexus</string>
<key>CFBundlePackageType</key><string>APPL</string><key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
""", encoding="utf-8")
    (app / "MacOS" / "nexus").write_text('#!/bin/bash\nDIR="$(cd "$(dirname "$0")/../../.." && pwd)"\n'
                                         'open -a Terminal "$DIR/Nexus-Office.command"\n', encoding="utf-8")


# ------------------------------------------------------------------ offline: runtime + wheels + llama-server + sd-cli
DESKTOP_RT = ROOT / ".cache" / "desktop-runtimes"     # packaging/build_python_runtimes.py y build_desktop_runtimes.sh
WHEELS_ALL = ROOT / ".cache" / "wheelhouse-all"       # python -m nexus wheels --target all (si ya se descargo)
# Windows 11 ARM ejecuta el CPython x64 emulado (conda-forge no tiene 3.11 para win-arm64): runtime y wheels de x64;
# llama-server y sd-cli si son nativos ARM64 (son procesos aparte).
PY_TARGET = {"windows-arm64": "windows-x64"}


def _python_runtime(target: str) -> Path:
    """CPython portable: el armado con conda-forge si esta en .cache, si no python-build-standalone (GitHub)."""
    t = TARGETS[target]
    local = sorted((DESKTOP_RT / target).glob("cpython-3.11.*-install_only*.tar.gz"))
    if local:
        return local[-1]
    rel = json.load(urllib.request.urlopen(urllib.request.Request(
        "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest", headers={"User-Agent": "nexus"})))
    pat = re.compile(rf"^cpython-3\.11\.\d+\+\d+-{re.escape(t['pbs'])}-install_only_stripped\.tar\.gz$")
    asset = next((a for a in rel["assets"] if pat.match(a["name"])), None)
    if not asset:
        raise RuntimeError(f"runtime no encontrado para {t['pbs']}")
    cache = ROOT / ".cache" / asset["name"]
    if not cache.exists():
        log(f"descargando {asset['name']} ({asset['size'] >> 20} MB)")
        urllib.request.urlretrieve(asset["browser_download_url"], cache)
    return cache


def offline(target: str, dest: Path) -> None:
    if "pbs" not in TARGETS[target]:
        log(f"{target}: sin modo offline (usa los paquetes del sistema)")
        return
    py_target = PY_TARGET.get(target, target)
    rt = dest / "runtime"
    rt.mkdir()
    src = _python_runtime(py_target)
    shutil.copy2(src, rt / src.name)
    log(f"offline: runtime {src.name}")
    # base + extras de voz/NPU, solo binarias (nexus/wheelhouse.py): el paquete offline instala sin compilar
    from nexus import wheelhouse
    wh = dest / "wheelhouse"
    cache = WHEELS_ALL if wheelhouse.check(py_target, WHEELS_ALL)[0] else ROOT / ".cache" / "wheelhouse"
    if cache != WHEELS_ALL:
        log(f"descargando wheels para {py_target}...")
        missing = wheelhouse.download(py_target, cache, log=log)
        if missing:
            log(f"sin wheel en {py_target}: {', '.join(missing)}")
    ok, msg = wheelhouse.check(py_target, cache)
    if not ok:
        raise RuntimeError(f"wheelhouse incompleto para {py_target}: {msg}")
    wh.mkdir()
    keep = set()                                       # solo las de esta plataforma (y las universales)
    for _, plats in wheelhouse._variants(py_target):
        keep |= {f for versions in wheelhouse._index(cache, plats).values() for _, f in versions}
    for f in sorted(keep):
        shutil.copy2(f, wh / f.name)
    wheelhouse.write_manifest(wh, [py_target])
    log(f"offline: {len(wheelhouse.wheels(wh))} wheels")
    # llama-server y sd-cli precompilados (todas las variantes: CPU, Vulkan, CUDA, Metal): sin descargar runtimes
    for rt, exe in (("llama", "llama-server"), ("sd", "sd-cli")):
        src = DESKTOP_RT / target / rt
        if src.is_dir() and any(src.iterdir()):
            shutil.copytree(src, dest / "bundled" / "runtimes" / rt)
            log(f"offline: {exe} {', '.join(sorted(p.name for p in src.iterdir()))}")
        else:
            log(f"offline: sin {exe} precompilado para {target} (se descargara desde Sistema > Runtimes)")
    # motor «llama.cpp integrado» del selector de inicio: wheel de llama-cpp-python de la plataforma del Python
    # (Windows ARM usa el Python x64) mas sus dependencias puras; se instala sin red al elegirlo
    lp = DESKTOP_RT / py_target / "llama-python"
    if lp.is_dir() and any(lp.glob("llama_cpp_python-*.whl")):
        shutil.copytree(lp, dest / "bundled" / "runtimes" / "llama-python")
        _llama_python_deps(dest / "bundled" / "runtimes" / "llama-python", py_target)
        log(f"offline: llama-cpp-python {', '.join(p.name for p in lp.glob('*.whl'))}")
    else:
        log(f"offline: sin llama-cpp-python para {py_target} (el selector solo ofrecera llama-server)")
    # GIMP portable (packaging/fetch_gimp.py): los agentes editan imagenes con GIMP sin instalar nada
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import fetch_gimp
    g = None if os.environ.get("NEXUS_SIN_GIMP") else fetch_gimp.bundle_dir(target, DESKTOP_RT)
    if g:
        shutil.copytree(g, dest / "bundled" / "runtimes" / "gimp", symlinks=True)
        log(f"offline: GIMP de {g}")
    else:
        log(f"offline: sin GIMP para {target} (los agentes editan las imagenes con Pillow)")


# ------------------------------------------------------------------ archivos
def _out(target: str, name: str) -> Path:
    d = DIST / GROUP[target]
    d.mkdir(parents=True, exist_ok=True)
    return d / name


def archive(target: str, dest: Path, suffix: str = "") -> Path:
    t = TARGETS[target]
    DIST.mkdir(exist_ok=True)
    out = _out(target, f"Nexus-Office-{VERSION}-{target}{suffix}.{t['fmt']}")
    base = dest.parent
    if t["fmt"] == "zip":
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(dest.rglob("*")):
                if f.is_file():
                    z.write(f, f.relative_to(base))
    else:
        def mode(ti: tarfile.TarInfo) -> tarfile.TarInfo:
            name = ti.name
            if "/bundled/runtimes/gimp/" in name:   # GIMP lleva sus propios ejecutables y plugins: permisos tal cual
                ti.mode = (ti.mode & 0o755) | 0o644 if not ti.issym() else ti.mode
            elif ti.isdir() or name.endswith((".sh", ".command")) or "/MacOS/" in name or (
                    "/bundled/runtimes/" in name and name.rsplit("/", 1)[-1] in ("llama-server", "sd-cli")):
                ti.mode = 0o755
            else:
                ti.mode = 0o644
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            return ti
        with tarfile.open(out, "w:gz", compresslevel=9) as tar:
            tar.add(dest, arcname="Nexus-Office", filter=mode)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


def _ar(members: list[tuple[str, bytes]]) -> bytes:
    out = b"!<arch>\n"
    for name, data in members:
        hdr = f"{name:<16}{int(time.time()):<12}{0:<6}{0:<6}{'100644':<8}{len(data):<10}`\n".encode()
        out += hdr + data + (b"\n" if len(data) % 2 else b"")
    return out


def deb(target: str, dest: Path) -> Path:
    """Paquete .deb en Python puro: instala en /opt/nexus-office, lanzador /usr/bin/nexus-office y entrada de menu.
    Los datos de cada usuario van a ~/.local/share/nexus-office (la carpeta /opt queda de solo lectura)."""
    arch = TARGETS[target]["deb"]
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz", format=tarfile.GNU_FORMAT) as tar:
        def add_bytes(path: str, content: bytes, mode_: int = 0o644):
            ti = tarfile.TarInfo(path)
            ti.size, ti.mode, ti.mtime = len(content), mode_, int(time.time())
            tar.addfile(ti, io.BytesIO(content))

        def filt(ti):
            ti.uid = ti.gid = 0
            if "/bundled/runtimes/gimp/" in ti.name:   # GIMP: permisos de sus ejecutables y plugins tal cual
                ti.mode = (ti.mode & 0o755) | 0o644
            else:
                ti.mode = 0o755 if ti.isdir() or ti.name.endswith(".sh") or ti.name.rsplit("/", 1)[-1] in ("llama-server", "sd-cli") \
                    else 0o644
            return ti
        tar.add(dest, arcname="./opt/nexus-office", filter=filt)
        add_bytes("./usr/bin/nexus-office", b"""#!/bin/sh
# Lanzador de Nexus-Office: programa en /opt/nexus-office (portable, sin internet), datos por usuario.
#   nexus-office                       arranca Nexus-Office y abre el navegador
#   nexus-office servicio instalar     arranque automatico (con sudo: con el equipo; --red: accesible desde moviles)
#   nexus-office modelos [--descargar] modelo de IA elegido para este equipo | nexus-office oficina <perfil>
if [ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ]; then
  export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-$(getent passwd "$SUDO_USER" | cut -d: -f6)/.local/share/nexus-office}"
else
  export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-$HOME/.local/share/nexus-office}"
fi
mkdir -p "$NEXUS_DATA_DIR"
PY=/opt/nexus-office/venv/bin/python3
if [ ! -x "$PY" ]; then
  echo "Preparando Nexus-Office (una vez, requiere sudo)"; sudo /opt/nexus-office/install.sh --offline --no-clean --modelos=no
fi
cd /opt/nexus-office
case "$1" in
  servicio|modelos|oficina|detect|pull|chat) exec "$PY" -m nexus "$@";;
esac
if [ ! -f "$NEXUS_DATA_DIR/.modelos-evaluados" ]; then
  echo "Primera ejecucion: evaluando el hardware y eligiendo el modelo de IA..."
  "$PY" -m nexus modelos --descargar && touch "$NEXUS_DATA_DIR/.modelos-evaluados"
fi
exec "$PY" -m nexus run "$@"
""", 0o755)
        add_bytes("./usr/share/applications/nexus-office.desktop", b"""[Desktop Entry]
Type=Application
Name=Nexus-Office
Comment=Agente de IA multimodal portable
Exec=nexus-office
Icon=/opt/nexus-office/nexus/web/icon.svg
Terminal=true
Categories=Utility;Education;Science;
""")
    size_kb = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file()) // 1024
    control = f"""Package: nexus-office
Version: {VERSION}
Section: utils
Priority: optional
Architecture: {arch}
Depends: libc6 (>= 2.28)
Recommends: libvulkan1, libgomp1, ca-certificates
Installed-Size: {size_kb}
Maintainer: Nexus-Office <nexus@localhost>
Description: Oficina de IA local para pymes: agentes por gremio, memoria y documentos
 Incluye Python, dependencias y motor de IA: se instala sin internet. Detecta el hardware
 (CPU, GPU NVIDIA/AMD/Intel, NPU) y elige el modelo de IA adecuado. Puede funcionar como servicio.
""".encode()
    postinst = (b"#!/bin/sh\nset -e\nNEXUS_DATA_DIR=/var/lib/nexus-office /opt/nexus-office/install.sh --offline --no-clean --modelos=no "
                b"|| echo 'Se completara en la primera ejecucion de nexus-office'\nexit 0\n")
    prerm = (b"#!/bin/sh\n[ -x /opt/nexus-office/venv/bin/python3 ] && (cd /opt/nexus-office && venv/bin/python3 -m nexus servicio quitar) "
             b"|| true\nrm -rf /opt/nexus-office/venv /opt/nexus-office/.cache /var/lib/nexus-office\n"
             b"find /opt/nexus-office -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null\nexit 0\n")
    ctrl = io.BytesIO()
    with tarfile.open(fileobj=ctrl, mode="w:gz", format=tarfile.GNU_FORMAT) as tar:
        for name, content, m in (("./control", control, 0o644), ("./postinst", postinst, 0o755), ("./prerm", prerm, 0o755)):
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(content), m, int(time.time())
            tar.addfile(ti, io.BytesIO(content))
    out = _out(target, f"nexus-office_{VERSION}_{arch}.deb")
    out.write_bytes(_ar([("debian-binary", b"2.0\n"), ("control.tar.gz", ctrl.getvalue()), ("data.tar.gz", data.getvalue())]))
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


RUN_HEADER = """#!/bin/sh
# Nexus-Office {version} - instalador autoextraible para Linux ({arch}).
# Uso: sh {name} [carpeta_destino] [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto|todos|no]
# Todo va incluido (Python, dependencias, motor de IA): no necesita internet salvo para descargar el modelo.
set -e
DEFAULT="$HOME/.local/share/nexus-office"
[ "$(id -u)" = 0 ] && DEFAULT=/opt/nexus-office        # con sudo: para todo el equipo (el servicio corre como $SUDO_USER)
DEST="${{1:-$DEFAULT}}"
case "$DEST" in --*) DEST="$DEFAULT";; *) [ $# -gt 0 ] && shift;; esac
echo "[Nexus-Office] Instalando en $DEST"
mkdir -p "$DEST"
LINE=$(awk '/^__NEXUS_ARCHIVE__$/{{print NR + 1; exit 0}}' "$0")
tail -n +"$LINE" "$0" | tar -xz -C "$DEST" --strip-components=1
cd "$DEST"
chmod +x install.sh start.sh 2>/dev/null || true
./install.sh "$@"
if [ "$(id -u)" = 0 ] && [ -n "${{SUDO_USER:-}}" ]; then chown -R "$SUDO_USER" "$DEST"; fi
echo "[Nexus-Office] Listo. Arranca con: $DEST/start.sh"
exit 0
__NEXUS_ARCHIVE__
"""


def run_installer(target: str, tarball: Path, suffix: str = "") -> Path:
    """Binario de instalacion para Linux: script sh + tar.gz incrustado (estilo makeself)."""
    arch = "x86_64" if "x86_64" in target else "aarch64"
    name = f"Nexus-Office-{VERSION}-linux-{arch}{suffix}.run"
    out = _out(target, name)
    header = RUN_HEADER.format(version=VERSION, arch=arch, name=name).encode()
    out.write_bytes(header + tarball.read_bytes())
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


# data/ solo en la raiz (los datos del usuario): tests/data son ficheros de prueba y van en el zip
PROJECT_SKIP = re.compile(r"^data(/|$)|(^|/)(venv|wheelhouse|runtime|bundled|\.cache|\.gradle|build|dist[^/]*|__pycache__|\.pytest_cache|\.git)(/|$)"
                          r"|\.pyc$|\.jks$|(^|/)(keystore|local)\.properties$|\.install\.log$")
SECRET_FILES = re.compile(r"\.(jks|keystore|pass|pem|key|p12|log)$|(^|/)(config\.ya?ml|\.env)$", re.I)


def source_zip() -> Path:
    """Zip del proyecto limpio: codigo, tests, empaquetado, apps Android y documentacion; sin entorno, datos,
    modelos, caches, compilados ni secretos (falla si se colara alguno)."""
    files = [f for f in sorted(ROOT.rglob("*")) if f.is_file() and not PROJECT_SKIP.search(f.relative_to(ROOT).as_posix())]
    leaks = [f.relative_to(ROOT).as_posix() for f in files if SECRET_FILES.search(f.relative_to(ROOT).as_posix())]
    if leaks:
        raise RuntimeError(f"ficheros sensibles en el codigo fuente, revisa PROJECT_SKIP: {leaks}")
    out = DIST / f"Nexus-Office-{VERSION}-codigo-fuente.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            z.write(f, f"Nexus-Office/{f.relative_to(ROOT).as_posix()}")
    log(f"-> {out.name} ({len(files)} ficheros, {out.stat().st_size / 1e6:.1f} MB)")
    return out


def _profiles_nsh(path: Path) -> None:
    """Perfiles de oficina para el desplegable del instalador ("id: Nombre")."""
    import yaml
    lines = []
    for f in sorted((ROOT / "nexus" / "office" / "perfiles").glob("*.yaml")):
        name = (yaml.safe_load(f.read_text(encoding="utf-8")) or {}).get("name", f.stem).replace('"', "'")
        lines.append(f'  ${{NSD_CB_AddString}} $hCombo "{f.stem}: {name}"')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _llama_python_deps(d: Path, py_target: str) -> None:
    """llama-cpp-python se instala con --no-deps: Jinja2 (plantillas de chat) y MarkupSafe van junto a su wheel
    (numpy y typing-extensions ya estan en el wheelhouse base)."""
    have = {p.name.split("-")[0].lower() for p in d.glob("*.whl")}
    need = [n for n in ("jinja2", "markupsafe") if n not in have]
    if not need:
        return
    plats = [a for p in TARGETS[py_target]["pip"] for a in ("--platform", p)]
    subprocess.run([sys.executable, "-m", "pip", "download", "-q", "--disable-pip-version-check", "--no-deps",
                    "--only-binary", ":all:", "--python-version", "3.11", "--implementation", "cp", *plats,
                    "-d", str(d), *need], check=True)


def windows_exe(target: str, dest: Path) -> Path | None:
    """Instalador grafico NSIS con el paquete portable completo dentro (makensis: Windows, Linux o macOS)."""
    makensis = shutil.which("makensis")
    if not makensis:
        log("instalador .exe: falta makensis (apt install nsis / choco install nsis / brew install makensis)")
        return None
    arch = "arm64" if target.endswith("arm64") else "x64"
    work = BUILD / f"nsis-{arch}"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    _profiles_nsh(work / "profiles.nsh")
    out = _out(target, f"Nexus-Office-Setup-{VERSION}-windows-{arch}.exe")
    defs = {"VERSION": VERSION, "ARCH": arch, "PAYLOAD": str(dest), "OUTFILE": str(out),
            "ICON": str(ROOT / "packaging" / "windows" / "nexus.ico"), "LICENSE": str(ROOT / "LICENSE"),
            "PROFILES": str(work / "profiles.nsh")}
    size = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file())
    if size > 1_800_000_000:     # /SOLID no admite paquetes de mas de ~2 GB sin comprimir
        defs["NOSOLID"] = "1"
    log(f"makensis {out.name} ({size / 1e9:.2f} GB sin comprimir, LZMA{'' if 'NOSOLID' in defs else ' solida'}, tarda)...")
    subprocess.run([makensis, "-V2", "-INPUTCHARSET", "UTF8", *[f"-D{k}={v}" for k, v in defs.items()],
                    str(ROOT / "packaging" / "windows" / "nexus.nsi")], check=True)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


README = """Nexus-Office {version} - instaladores (todos 100 % portables: se instalan sin internet)
===================================================================================

Todos llevan dentro Python, las dependencias y el motor de IA (llama-server y sd-cli: CPU, Vulkan y, segun la
plataforma, CUDA o Metal). Al instalar, Nexus-Office evalua el hardware (memoria, grafica, procesador) y elige el modelo
de IA adecuado; lo descarga si hay internet (sin internet lo recomienda y se descarga despues desde la app).
Todos pueden dejar Nexus-Office como servicio que arranca solo con el equipo.

windows/  Nexus-Office-Setup-{version}-windows-x64.exe    PC con Windows 10/11 de 64 bits (Intel/AMD; NVIDIA con CUDA)
          Nexus-Office-Setup-{version}-windows-arm64.exe  Windows 11 en ARM (Snapdragon X)
          Doble clic; si Windows avisa "protegio su PC": Mas informacion > Ejecutar de todas formas.
          Silencioso: Nexus-Office-Setup-...exe /S /OFICINA=herreria /SERVICIO /RED
linux/    sh Nexus-Office-{version}-linux-x86_64.run [carpeta] [--oficina=herreria] [--servicio] [--red]
          sh Nexus-Office-{version}-linux-aarch64.run      ARM de 64 bits: Raspberry Pi 4/5 (Raspberry Pi OS 64 bits),
                                                     Jetson, portatiles ARM
          sudo apt install ./nexus-office_{version}_amd64.deb   (o _arm64.deb en Raspberry Pi OS 64 bits / ARM)
          luego: nexus-office   |   servicio: sudo nexus-office servicio instalar [--red]
          Nexus-Office-{version}-linux-x86_64.AppImage / -aarch64.AppImage   sin instalar: chmod +x y abrir
          flatpak install --user Nexus-Office-{version}-linux-x86_64.flatpak  (o -aarch64; pide el runtime de Flathub)
macos/    Nexus-Office-{version}-macos-arm64.tar.gz (Apple Silicon) / macos-x86_64 (Intel): descomprimir y abrir
          "Nexus-Office.command" (o ./install.sh --oficina=herreria --servicio && ./start.sh)
android/  Nexus-Office-{version}-android-autonomo.apk  Nexus-Office completo dentro del movil (ARM64, Android 9+), sin PC;
                                                  usa la GPU (Adreno por OpenCL, Mali/Xclipse por Vulkan) si acelera
          Nexus-Office-{version}-android-vpn.apk       app ligera que conecta con el Nexus-Office de la oficina (VPN o Wi-Fi)
Nexus-Office-{version}-codigo-fuente.zip               proyecto limpio (codigo, tests, empaquetado y documentacion)
Manual-de-usuario.pdf / Documentacion-tecnica.pdf

Oficina: 58 perfiles de pyme por gremio; tras instalar, la vista Modelos sugiere modelos especificos del sector.
Raspberry Pi: hace falta el sistema de 64 bits (Raspberry Pi OS 64-bit, Ubuntu); Pi 4 o Pi 5 con 4 GB o mas.
Integridad: SHA256SUMS.txt
"""


def checksums() -> None:
    lines = []
    for f in sorted(DIST.rglob("*")):
        if f.is_file() and f.name != "SHA256SUMS.txt":
            h = hashlib.sha256()
            with open(f, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            lines.append(f"{h.hexdigest()}  {f.relative_to(DIST).as_posix()}")
    (DIST / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n")


def build_target(target: str) -> None:
    log(f"=== {target} ===")
    dest = stage(target)
    offline(target, dest)
    if target.startswith("windows"):
        windows_exe(target, dest)
    elif target.startswith("linux"):
        deb(target, dest)
        tarball = archive(target, dest)
        run_installer(target, tarball)
        tarball.unlink()
        import build_linux_bundles   # AppImage y Flatpak (Nexus-Office ya instalado dentro, solo lectura)
        build_linux_bundles.build(dest, target.split("-", 1)[1], TARGETS[target]["pip"], VERSION, DIST / "linux",
                                  BUILD / f"bundles-{target}")
    else:
        archive(target, dest)
    shutil.rmtree(dest.parent, ignore_errors=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--targets", nargs="*", default=list(TARGETS))
    ap.add_argument("--apk", nargs="*", default=[], help="APK ya compiladas (build_apk.py / build_apk_autonomo.py)")
    ap.add_argument("--docs", nargs="*", default=[], help="ficheros extra para la raiz (p.ej. los manuales en PDF)")
    ap.add_argument("--keep", action="store_true", help="no borra dist/ (anade plataformas a una entrega)")
    a = ap.parse_args()
    if not a.keep:
        shutil.rmtree(DIST, ignore_errors=True)
    DIST.mkdir(exist_ok=True)
    for target in a.targets:
        build_target(target)
    for extra in a.apk:
        (DIST / "android").mkdir(exist_ok=True)
        shutil.copy2(extra, DIST / "android" / Path(extra).name)
    for extra in a.docs:
        shutil.copy2(extra, DIST / Path(extra).name)
    source_zip()
    (DIST / "LEEME.txt").write_text(README.format(version=VERSION), encoding="utf-8")
    checksums()
    log(f"Listo: {DIST}")


if __name__ == "__main__":
    main()
