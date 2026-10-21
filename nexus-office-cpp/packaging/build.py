"""Instaladores de Nexus-Office 2.0 beta C (edicion C++). Todos son 100 % portables y se instalan sin internet: llevan
el ejecutable nativo (nexus-office), la interfaz web y los motores de IA (llama-server y sd-cli en sus variantes,
piper y whisper-cli); solo los modelos de IA se descargan, si hay red, elegidos segun el hardware.

    python packaging/build.py                         # todas las plataformas
    python packaging/build.py --targets linux-x86_64 windows-arm64
    python packaging/build.py --targets --apk dist-apk/*.apk --docs *.pdf   # solo codigo fuente, LEEME y sumas

Entradas (en .cache/, las deja .github/workflows/cpp-entrega.yml):
  .cache/cpp/<target>/Nexus-Office/            carpeta instalada por «cmake --install» en cada sistema (cpp.yml)
  .cache/desktop-runtimes/<target>/llama|sd/   llama-server y sd-cli de la Release de la edicion Python
  .cache/desktop-runtimes/<target>/piper|whisper/   voz: piper (rhasspy/piper) y whisper-cli (whisper.cpp)
Salida en dist/ (los nombres llevan la version, 2.0.0-beta-cpp, y no chocan con los de la edicion Python):
  windows/  Nexus-Office-Setup-<ver>-windows-x64.exe / -arm64.exe     instalador grafico (NSIS; makensis)
  linux/    Nexus-Office-<ver>-linux-<x86_64|aarch64>.run / .AppImage / .flatpak, nexus-office-c_<ver>_<amd64|arm64>.deb
  macos/    Nexus-Office-<ver>-macos-<arm64|x86_64>.tar.gz              abre «Nexus-Office.command»
  android/  las APK de --apk
  Nexus-Office-<ver>-codigo-fuente.zip, LEEME.txt, SHA256SUMS.txt
"""
from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # nexus-office-cpp/
REPO = ROOT.parent
PY_EDITION = REPO / "nexus-office"                     # recursos compartidos (nexus/web, perfiles, agentes) y manuales
DIST = Path(os.environ.get("NEXUS_DIST") or ROOT / "dist")
BUILD = ROOT / "build-paquetes"
CACHE = ROOT / ".cache"
sys.path.insert(0, str(PY_EDITION / "packaging"))


def _version() -> str:
    m = re.search(r'set\(NEXUS_VERSION "([^"]+)"\)', (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    return m.group(1)


VERSION = _version()                                   # 2.0.0-beta-cpp
NUMVER = re.match(r"\d+\.\d+\.\d+", VERSION).group(0)  # 2.0.0 (NSIS y metadatos que solo admiten numeros)
DEB_VERSION = VERSION.replace("-", "~", 1).replace("-", ".")   # 2.0.0~beta.cpp: dpkg la ordena antes que la 2.0.0
TARGETS = {
    "windows-x64": {"os": "windows"},
    "windows-arm64": {"os": "windows"},
    "linux-x86_64": {"os": "linux", "deb": "amd64", "note": "PC Linux, Steam Deck (SteamOS), servidores"},
    "linux-aarch64": {"os": "linux", "deb": "arm64",
                      "note": "Raspberry Pi 4/5 (64 bits), NVIDIA Jetson, portatiles y consolas portatiles ARM"},
    "macos-arm64": {"os": "macos"},
    "macos-x86_64": {"os": "macos"},
}
RUNTIMES = ("llama", "sd", "piper", "whisper")
EXECUTABLES = {"nexus-office", "llama-server", "sd-cli", "piper", "whisper-cli", "espeak-ng"}


def log(msg: str) -> None:
    print(f"[build] {msg}", flush=True)


def _out(target: str, name: str) -> Path:
    d = DIST / TARGETS[target]["os"]
    d.mkdir(parents=True, exist_ok=True)
    return d / name


# ------------------------------------------------------------------ carpeta del paquete
def stage(target: str) -> Path:
    src = CACHE / "cpp" / target / "Nexus-Office"
    exe = src / ("nexus-office.exe" if target.startswith("windows") else "nexus-office")
    if not exe.is_file():
        raise SystemExit(f"falta {exe}: descarga el artefacto cpp-{target} de cpp.yml en .cache/cpp/{target}/")
    dest = BUILD / target / "Nexus-Office"
    shutil.rmtree(dest.parent, ignore_errors=True)
    shutil.copytree(src, dest)
    scripts = ROOT / "packaging" / "scripts"
    for name in (("install.bat", "start.bat") if target.startswith("windows") else ("install.sh", "start.sh")):
        shutil.copy2(scripts / name, dest / name)
    shutil.copytree(PY_EDITION / "docs", dest / "docs", ignore=shutil.ignore_patterns("*.pdf"))
    for f in (ROOT / "docs").glob("*.md"):           # los de la edicion C++ mandan (USUARIO.md, CPP.md...)
        shutil.copy2(f, dest / "docs" / f.name)
    for name in ("README.md", "CHANGELOG.md"):
        shutil.copy2(ROOT / name, dest / name)
    shutil.copy2(PY_EDITION / "LICENSE", dest / "LICENSE")
    for rt in RUNTIMES:
        r = CACHE / "desktop-runtimes" / target / rt
        if r.is_dir() and any(r.iterdir()):
            shutil.copytree(r, dest / "bundled" / "runtimes" / rt, symlinks=True)
            log(f"{target}: {rt} {', '.join(sorted(p.name for p in r.iterdir()))}")
        else:
            log(f"{target}: sin {rt} incluido")
    if TARGETS[target]["os"] == "macos":
        _mac_launchers(dest)
    note = TARGETS[target].get("note", "")
    (dest / "PLATAFORMA.txt").write_text(f"Nexus-Office {VERSION} (edicion C++) - {target}\n{note}\n", encoding="utf-8")
    return dest


def _mac_launchers(dest: Path) -> None:
    """Lanzador de doble clic (.command) y un bundle .app minimo que lo abre en el Terminal."""
    cmd = dest / "Nexus-Office.command"
    cmd.write_text('#!/bin/bash\ncd "$(dirname "$0")"\n[ -f data/.preparado ] || { ./install.sh && mkdir -p data && '
                   'touch data/.preparado; }\nexec ./start.sh\n', encoding="utf-8")
    cmd.chmod(0o755)
    app = dest / "Nexus-Office.app" / "Contents"
    (app / "MacOS").mkdir(parents=True)
    (app / "Info.plist").write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Nexus-Office</string><key>CFBundleIdentifier</key><string>ai.nexus.office.cpp</string>
<key>CFBundleVersion</key><string>{NUMVER}</string><key>CFBundleShortVersionString</key><string>{VERSION}</string>
<key>CFBundleExecutable</key><string>nexus</string>
<key>CFBundlePackageType</key><string>APPL</string><key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
""", encoding="utf-8")
    launcher = app / "MacOS" / "nexus"
    launcher.write_text('#!/bin/bash\nDIR="$(cd "$(dirname "$0")/../../.." && pwd)"\nopen -a Terminal "$DIR/Nexus-Office.command"\n',
                        encoding="utf-8")
    launcher.chmod(0o755)


def _mode(name: str, is_dir: bool) -> int:
    base = name.rsplit("/", 1)[-1]
    if is_dir or base in EXECUTABLES or name.endswith((".sh", ".command")) or "/MacOS/" in name \
            or ("/bundled/runtimes/" in name and "." not in base):
        return 0o755
    return 0o644


def _tar_filter(ti: tarfile.TarInfo) -> tarfile.TarInfo:
    if not ti.issym():
        ti.mode = _mode(ti.name, ti.isdir())
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    return ti


def tarball(target: str, dest: Path) -> Path:
    out = _out(target, f"Nexus-Office-{VERSION}-{target}.tar.gz")
    with tarfile.open(out, "w:gz", compresslevel=9) as tar:
        tar.add(dest, arcname="Nexus-Office", filter=_tar_filter)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


# ------------------------------------------------------------------ Linux: .run, .deb, AppImage, Flatpak
RUN_HEADER = """#!/bin/sh
# Nexus-Office {version} (edicion C++) - instalador autoextraible para Linux ({arch}).
# Uso: sh {name} [carpeta_destino] [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto|todos|no]
# Todo va incluido (programa nativo y motores de IA): no necesita internet salvo para descargar los modelos.
set -e
DEFAULT="$HOME/.local/share/nexus-office-c"
[ "$(id -u)" = 0 ] && DEFAULT=/opt/nexus-office-c       # con sudo: para todo el equipo
DEST="${{1:-$DEFAULT}}"
case "$DEST" in --*) DEST="$DEFAULT";; *) [ $# -gt 0 ] && shift;; esac
echo "[Nexus-Office] Instalando en $DEST"
mkdir -p "$DEST"
LINE=$(awk '/^__NEXUS_ARCHIVE__$/{{print NR + 1; exit 0}}' "$0")
tail -n +"$LINE" "$0" | tar -xz -C "$DEST" --strip-components=1
cd "$DEST"
./install.sh "$@"
if [ "$(id -u)" = 0 ] && [ -n "${{SUDO_USER:-}}" ]; then chown -R "$SUDO_USER" "$DEST"; fi
echo "[Nexus-Office] Listo. Arranca con: $DEST/start.sh"
exit 0
__NEXUS_ARCHIVE__
"""


def run_installer(target: str, tgz: Path) -> Path:
    arch = target.split("-", 1)[1]
    name = f"Nexus-Office-{VERSION}-linux-{arch}.run"
    out = _out(target, name)
    with open(out, "wb") as f:
        f.write(RUN_HEADER.format(version=VERSION, arch=arch, name=name).encode())
        with open(tgz, "rb") as g:
            shutil.copyfileobj(g, f, 1 << 20)
    out.chmod(0o755)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


DEB_LAUNCHER = b"""#!/bin/sh
# Nexus-Office (edicion C++): programa en /opt/nexus-office-c (portable, sin internet), datos por usuario.
#   nexus-office-c                       arranca Nexus-Office y abre el navegador
#   nexus-office-c servicio instalar     arranque automatico (con sudo: con el equipo; --red: accesible desde moviles)
#   nexus-office-c modelos [--descargar] modelos de IA para este equipo | nexus-office-c oficina <perfil>
if [ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ]; then
  export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-$(getent passwd "$SUDO_USER" | cut -d: -f6)/.local/share/nexus-office-c}"
else
  export NEXUS_DATA_DIR="${NEXUS_DATA_DIR:-$HOME/.local/share/nexus-office-c}"
fi
mkdir -p "$NEXUS_DATA_DIR"
EXE=/opt/nexus-office-c/nexus-office
case "$1" in
  run|servicio|modelos|oficina|detect|pull|chat|--version|help|--help) exec "$EXE" "$@";;
esac
if [ ! -f "$NEXUS_DATA_DIR/.modelos-evaluados" ]; then
  echo "Primera ejecucion: evaluando el hardware y eligiendo el modelo de IA..."
  "$EXE" modelos --descargar && touch "$NEXUS_DATA_DIR/.modelos-evaluados"
fi
exec "$EXE" run "$@"
"""


def _ar(members: list[tuple[str, bytes | Path]]) -> bytes:
    out = io.BytesIO()
    out.write(b"!<arch>\n")
    for name, data in members:
        size = data.stat().st_size if isinstance(data, Path) else len(data)
        out.write(f"{name:<16}{int(time.time()):<12}{0:<6}{0:<6}{'100644':<8}{size:<10}`\n".encode())
        out.write(data.read_bytes() if isinstance(data, Path) else data)
        if size % 2:
            out.write(b"\n")
    return out.getvalue()


def deb(target: str, dest: Path) -> Path:
    """Paquete .deb en Python puro: /opt/nexus-office-c, lanzador /usr/bin/nexus-office-c y entrada de menu."""
    arch = TARGETS[target]["deb"]
    work = BUILD / f"deb-{target}"
    work.mkdir(parents=True, exist_ok=True)
    data = work / "data.tar.gz"
    with tarfile.open(data, "w:gz", format=tarfile.GNU_FORMAT) as tar:
        def add_bytes(path: str, content: bytes, mode: int = 0o644) -> None:
            ti = tarfile.TarInfo(path)
            ti.size, ti.mode, ti.mtime = len(content), mode, int(time.time())
            tar.addfile(ti, io.BytesIO(content))
        tar.add(dest, arcname="./opt/nexus-office-c", filter=_tar_filter)
        add_bytes("./usr/bin/nexus-office-c", DEB_LAUNCHER, 0o755)
        add_bytes("./usr/share/applications/nexus-office-c.desktop", b"""[Desktop Entry]
Type=Application
Name=Nexus-Office (C++)
Comment=Oficina de IA local para pymes (edicion C++)
Exec=nexus-office-c
Icon=/opt/nexus-office-c/nexus/web/icon.svg
Terminal=true
Categories=Office;Utility;
""")
    size_kb = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file()) // 1024
    control = f"""Package: nexus-office-c
Version: {DEB_VERSION}
Section: utils
Priority: optional
Architecture: {arch}
Depends: libc6 (>= 2.35)
Recommends: libvulkan1, libgomp1, ca-certificates
Installed-Size: {size_kb}
Maintainer: Nexus-Office <nexus@localhost>
Description: Oficina de IA local para pymes (edicion C++, beta)
 Un unico programa nativo con la misma interfaz que Nexus-Office: agentes por gremio, memoria,
 documentos y voz. Incluye los motores de IA: se instala sin internet. Detecta el hardware
 (CPU, GPU, NPU) y elige el modelo de IA adecuado. Puede funcionar como servicio.
""".encode()
    prerm = b"#!/bin/sh\n/opt/nexus-office-c/nexus-office servicio quitar >/dev/null 2>&1 || true\nexit 0\n"
    ctrl = io.BytesIO()
    with tarfile.open(fileobj=ctrl, mode="w:gz", format=tarfile.GNU_FORMAT) as tar:
        for name, content, m in (("./control", control, 0o644), ("./prerm", prerm, 0o755)):
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime = len(content), m, int(time.time())
            tar.addfile(ti, io.BytesIO(content))
    out = _out(target, f"nexus-office-c_{VERSION}_{arch}.deb")
    out.write_bytes(_ar([("debian-binary", b"2.0\n"), ("control.tar.gz", ctrl.getvalue()), ("data.tar.gz", data)]))
    shutil.rmtree(work)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


BUNDLE_LAUNCHER = """#!/bin/sh
# Nexus-Office ({kind}, edicion C++): el programa va dentro (solo lectura); los datos del usuario, fuera.
#   sin argumentos: arranca Nexus-Office y abre el navegador
#   servicio instalar [--red] | modelos [--descargar] | oficina <perfil> | chat ...: ordenes de nexus-office
HERE="{here}"
export NEXUS_DATA_DIR="${{NEXUS_DATA_DIR:-{data}}}"
mkdir -p "$NEXUS_DATA_DIR"
[ $# -eq 0 ] && set -- run
if [ "$1" = run ] && [ ! -f "$NEXUS_DATA_DIR/.modelos-evaluados" ]; then
  echo "Primera ejecucion: evaluando el hardware y eligiendo el modelo de IA..."
  "$HERE/nexus-office" modelos --descargar && touch "$NEXUS_DATA_DIR/.modelos-evaluados"
fi
exec "$HERE/nexus-office" "$@"
"""
APP_ID = "ai.nexus.NexusOfficeCpp"


def _bundle_tree(dest: Path, work: Path) -> Path:
    tree = work / "nexus"
    shutil.rmtree(tree, ignore_errors=True)
    shutil.copytree(dest, tree, symlinks=True, ignore=shutil.ignore_patterns("install.sh", "start.sh"))
    for f in tree.rglob("*"):
        if f.is_file() and not f.is_symlink():
            f.chmod(_mode(f.relative_to(work).as_posix(), False))
    return tree


def appimage(target: str, dest: Path) -> Path:
    import build_linux_bundles as lb                # herramientas y sus descargas, compartidas con la edicion Python
    arch = target.split("-", 1)[1]
    work = BUILD / f"appimage-{target}"
    shutil.rmtree(work, ignore_errors=True)
    appdir = work / "AppDir"
    appdir.mkdir(parents=True)
    shutil.move(str(_bundle_tree(dest, work)), appdir / "nexus")
    run = appdir / "AppRun"
    run.write_text(BUNDLE_LAUNCHER.format(kind="AppImage", here='$(dirname "$(readlink -f "$0")")/nexus',
                                          data="${XDG_DATA_HOME:-$HOME/.local/share}/nexus-office-c"))
    run.chmod(0o755)
    (appdir / "nexus-office-c.desktop").write_text(lb.DESKTOP.format(exec="nexus-office-c", icon="nexus-office-c")
                                                   .replace("Name=Nexus-Office", "Name=Nexus-Office (C++)")
                                                   + "X-AppImage-Name=Nexus-Office-C\n")
    _icon(appdir / "nexus-office-c.png")
    (appdir / ".DirIcon").symlink_to("nexus-office-c.png")
    out = _out(target, f"Nexus-Office-{VERSION}-linux-{arch}.AppImage")
    env = {"ARCH": arch, "PATH": "/usr/bin:/bin", "HOME": str(work)}
    subprocess.run([str(lb.appimagetool()), "--no-appstream", "--comp", "zstd", "--runtime-file", str(lb.tool(f"runtime-{arch}")),
                    str(appdir), str(out)], check=True, env=env, stdout=subprocess.DEVNULL)
    shutil.rmtree(work)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


def _icon(path: Path) -> None:
    from PIL import Image
    Image.open(ROOT / "packaging" / "windows" / "nexus.ico").convert("RGBA").resize((256, 256)).save(path)


def flatpak(target: str, dest: Path) -> Path | None:
    import build_linux_bundles as lb
    if not shutil.which("flatpak"):
        log("Flatpak: falta el comando flatpak (apt install flatpak)")
        return None
    arch = target.split("-", 1)[1]
    work = BUILD / f"flatpak-{target}"
    shutil.rmtree(work, ignore_errors=True)
    app = work / "app"
    files = app / "files"
    files.mkdir(parents=True)
    shutil.move(str(_bundle_tree(dest, work)), files / "nexus")
    (files / "bin").mkdir()
    launcher = files / "bin" / "nexus-office-c"
    launcher.write_text(BUNDLE_LAUNCHER.format(kind="Flatpak", here="/app/nexus",
                                               data="${XDG_DATA_HOME:-$HOME/.local/share}/nexus-office-c"))
    launcher.chmod(0o755)
    share = files / "share"
    for rel, content in ((f"applications/{APP_ID}.desktop", lb.DESKTOP.format(exec="nexus-office-c", icon=APP_ID)
                          .replace("Name=Nexus-Office", "Name=Nexus-Office (C++)")),
                         (f"metainfo/{APP_ID}.metainfo.xml", lb.METAINFO.format(app_id=APP_ID, version=NUMVER))):
        (share / rel).parent.mkdir(parents=True, exist_ok=True)
        (share / rel).write_text(content, encoding="utf-8")
    icon = share / "icons/hicolor/256x256/apps" / f"{APP_ID}.png"
    icon.parent.mkdir(parents=True)
    _icon(icon)
    (app / "metadata").write_text(f"""[Application]
name={APP_ID}
runtime=org.freedesktop.Platform/{arch}/{lb.RUNTIME}
sdk=org.freedesktop.Sdk/{arch}/{lb.RUNTIME}
command=nexus-office-c

[Context]
shared=network;ipc;
sockets=x11;wayland;pulseaudio;
devices=all;
filesystems=xdg-config/systemd:create;xdg-documents;xdg-download;
""")
    for rel in (f"share/applications/{APP_ID}.desktop", f"share/icons/hicolor/256x256/apps/{APP_ID}.png"):
        (app / "export" / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(files / rel, app / "export" / rel)
    repo = work / "repo"
    out = _out(target, f"Nexus-Office-{VERSION}-linux-{arch}.flatpak")
    subprocess.run(["flatpak", "build-export", f"--arch={arch}", "--disable-sandbox", str(repo), str(app), "stable"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["flatpak", "build-bundle", f"--arch={arch}", f"--runtime-repo={lb.FLATHUB}", str(repo), str(out),
                    APP_ID, "stable"], check=True)
    shutil.rmtree(work)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


# ------------------------------------------------------------------ Windows: instalador grafico
def _profiles_nsh(path: Path) -> None:
    """Perfiles de oficina para el desplegable del instalador ("id: Nombre"), leidos sin dependencias."""
    lines = []
    for f in sorted((PY_EDITION / "nexus" / "office" / "perfiles").glob("*.yaml")):
        m = re.search(r"^name:\s*(.+)$", f.read_text(encoding="utf-8"), re.M)
        name = (m.group(1).strip().strip("'\"") if m else f.stem).replace('"', "'")
        lines.append(f'  ${{NSD_CB_AddString}} $hCombo "{f.stem}: {name}"')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def windows_exe(target: str, dest: Path) -> Path | None:
    makensis = shutil.which("makensis")
    if not makensis:
        log("instalador .exe: falta makensis (apt install nsis / choco install nsis / brew install makensis)")
        return None
    arch = target.split("-", 1)[1]
    work = BUILD / f"nsis-{arch}"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    _profiles_nsh(work / "profiles.nsh")
    out = _out(target, f"Nexus-Office-Setup-{VERSION}-windows-{arch}.exe")
    win = ROOT / "packaging" / "windows"
    defs = {"VERSION": VERSION, "NUMVER": NUMVER, "ARCH": arch, "PAYLOAD": str(dest), "OUTFILE": str(out),
            "ICON": str(win / "nexus.ico"), "LICENSE": str(PY_EDITION / "LICENSE"), "PROFILES": str(work / "profiles.nsh")}
    size = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file())
    if size > 1_800_000_000:     # /SOLID no admite paquetes de mas de ~2 GB sin comprimir
        defs["NOSOLID"] = "1"
    log(f"makensis {out.name} ({size / 1e9:.2f} GB sin comprimir)...")
    subprocess.run([makensis, "-V2", "-INPUTCHARSET", "UTF8", *[f"-D{k}={v}" for k, v in defs.items()], str(win / "nexus.nsi")],
                   check=True)
    shutil.rmtree(work)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


# ------------------------------------------------------------------ codigo fuente, LEEME y sumas
# del repositorio: la edicion C++ entera y los recursos compartidos con la Python que necesita para compilar
SOURCE_PARTS = ["nexus-office-cpp", "nexus-office/nexus/web", "nexus-office/nexus/office/perfiles",
                "nexus-office/nexus/office/agentes", "nexus-office/nexus/office/conocimiento",
                "nexus-office/nexus/agents/defaults", "nexus-office/LICENSE", ".github/workflows/cpp.yml",
                ".github/workflows/cpp-entrega.yml"]
SOURCE_SKIP = re.compile(r"(^|/)(build[^/]*|dist|\.cache|__pycache__|node_modules|\.git)(/|$)|\.pyc$")
SECRET_FILES = re.compile(r"\.(jks|keystore|pass|pem|key|p12|log)$|(^|/)(config\.ya?ml|\.env)$", re.I)
SECRET_OK = {"nexus-office-cpp/third_party/certs/cacert.pem"}    # certificados raiz publicos (Mozilla)


def source_zip() -> Path:
    files = []
    for part in SOURCE_PARTS:
        p = REPO / part
        for f in ([p] if p.is_file() else sorted(p.rglob("*"))):
            rel = f.relative_to(REPO).as_posix()
            if f.is_file() and not SOURCE_SKIP.search(rel):
                files.append((f, rel))
    leaks = [rel for _, rel in files if SECRET_FILES.search(rel) and rel not in SECRET_OK]
    if leaks:
        raise RuntimeError(f"ficheros sensibles en el codigo fuente: {leaks}")
    DIST.mkdir(parents=True, exist_ok=True)
    out = DIST / f"Nexus-Office-{VERSION}-codigo-fuente.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f, rel in files:
            z.write(f, f"Nexus-Office-C/{rel}")
    log(f"-> {out.name} ({len(files)} ficheros, {out.stat().st_size / 1e6:.1f} MB)")
    return out


README = """Nexus-Office {version} - edicion C++ (beta) - instaladores 100 % portables (se instalan sin internet)
=====================================================================================================

La misma oficina de IA que Nexus-Office 2.0 (misma interfaz, mismos agentes, perfiles y datos), reescrita en C++: un
unico programa nativo, sin Python, que arranca en menos de un segundo y ocupa mucho menos. Lleva dentro los motores de
IA (llama-server y sd-cli: CPU, Vulkan y, segun la plataforma, CUDA o Metal; voz con piper y whisper). Al instalar
evalua el hardware (memoria, grafica, procesador) y elige el modelo de IA adecuado; lo descarga si hay internet.
Todos pueden dejar Nexus-Office como servicio que arranca solo con el equipo. Se instala junto a la edicion Python
sin pisarla. Lo que aun no hace la edicion C++ esta en docs/CPP.md.

windows/  Nexus-Office-Setup-{version}-windows-x64.exe    PC con Windows 10/11 de 64 bits (Intel/AMD; NVIDIA con CUDA)
          Nexus-Office-Setup-{version}-windows-arm64.exe  Windows 11 en ARM (Snapdragon X)
          Doble clic; si Windows avisa "protegio su PC": Mas informacion > Ejecutar de todas formas.
          Silencioso: Nexus-Office-Setup-...exe /S /OFICINA=herreria /SERVICIO /RED
linux/    sh Nexus-Office-{version}-linux-x86_64.run [carpeta] [--oficina=herreria] [--servicio] [--red]
          sh Nexus-Office-{version}-linux-aarch64.run      ARM de 64 bits: Raspberry Pi 4/5 (64 bits), Jetson, portatiles ARM
          sudo apt install ./nexus-office-c_{version}_amd64.deb   (o _arm64.deb en Raspberry Pi OS 64 bits / ARM)
          luego: nexus-office-c   |   servicio: sudo nexus-office-c servicio instalar [--red]
          Nexus-Office-{version}-linux-x86_64.AppImage / -aarch64.AppImage   sin instalar: chmod +x y abrir
          flatpak install --user Nexus-Office-{version}-linux-x86_64.flatpak  (o -aarch64; pide el runtime de Flathub)
macos/    Nexus-Office-{version}-macos-arm64.tar.gz (Apple Silicon) / macos-x86_64 (Intel): descomprimir y abrir
          "Nexus-Office.command" (o ./install.sh --oficina=herreria --servicio && ./start.sh)
android/  Nexus-Office-{version}-android-autonomo.apk  Nexus-Office C++ completo dentro del movil (ARM64, Android 9+)
          Nexus-Office-{version}-android-vpn.apk       app ligera que conecta con el Nexus-Office de la oficina
Nexus-Office-{version}-codigo-fuente.zip               codigo C++ limpio, pruebas, empaquetado y recursos compartidos
Manual-de-usuario.pdf / Documentacion-tecnica.pdf

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
    kind = TARGETS[target]["os"]
    if kind == "windows":
        windows_exe(target, dest)
    elif kind == "linux":
        deb(target, dest)
        tgz = tarball(target, dest)
        run_installer(target, tgz)
        tgz.unlink()
        appimage(target, dest)
        flatpak(target, dest)
    else:
        tarball(target, dest)
    shutil.rmtree(dest.parent, ignore_errors=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--targets", nargs="*", default=list(TARGETS))
    ap.add_argument("--apk", nargs="*", default=[], help="APK ya compiladas")
    ap.add_argument("--docs", nargs="*", default=[], help="ficheros extra para la raiz (los manuales en PDF)")
    ap.add_argument("--keep", action="store_true", help="no borra dist/ (anade plataformas a una entrega)")
    a = ap.parse_args()
    if not a.keep:
        shutil.rmtree(DIST, ignore_errors=True)
    DIST.mkdir(parents=True, exist_ok=True)
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
