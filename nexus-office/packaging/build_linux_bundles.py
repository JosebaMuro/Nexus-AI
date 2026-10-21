"""AppImage y Flatpak de Nexus-Office (x86_64 y aarch64), a partir del paquete portable ya preparado por build.py.

Los dos se montan de solo lectura, asi que aqui se deja Nexus-Office ya instalado (Python con sus dependencias) y los
datos van a la carpeta del usuario: ~/.local/share/nexus-office (AppImage) o ~/.var/app/ai.nexus.NexusOffice (Flatpak).

Herramientas (en .cache/appimage/, se descargan solas): appimagetool y los runtime-<arch> de AppImage/type2-runtime.
Flatpak: el comando flatpak (apt install flatpak). El .flatpak se genera sin compilar nada: lleva Nexus-Office completo y
pide al instalarse el runtime org.freedesktop.Platform 24.08 de Flathub (--runtime-repo).
"""
from __future__ import annotations

import compileall  # noqa: F401  (documenta que el .pyc se genera con el Python del propio paquete)
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / ".cache" / "appimage"
APP_ID = "ai.nexus.NexusOffice"
FLATHUB = "https://dl.flathub.org/repo/flathub.flatpakrepo"
RUNTIME = "24.08"
URLS = {
    "appimagetool-x86_64.AppImage": "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage",
    "runtime-x86_64": "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64",
    "runtime-aarch64": "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-aarch64",
}

LAUNCHER = """#!/bin/sh
# Nexus-Office ({kind}): Nexus-Office ya instalado dentro (solo lectura); datos del usuario fuera.
#   sin argumentos: arranca Nexus-Office y abre el navegador
#   servicio instalar [--red] | modelos [--descargar] | oficina <perfil> | chat ...: comandos de python -m nexus
HERE="{here}"
export NEXUS_DATA_DIR="${{NEXUS_DATA_DIR:-{data}}}"
export PYTHONDONTWRITEBYTECODE=1
mkdir -p "$NEXUS_DATA_DIR"
cd "$HERE"
[ $# -eq 0 ] && set -- run
if [ "$1" = run ] && [ ! -f "$NEXUS_DATA_DIR/.modelos-evaluados" ]; then
  echo "Primera ejecucion: evaluando el hardware y eligiendo el modelo de IA..."
  "$HERE/venv/bin/python3" -m nexus modelos --descargar && touch "$NEXUS_DATA_DIR/.modelos-evaluados"
fi
exec "$HERE/venv/bin/python3" -m nexus "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=Nexus-Office
GenericName=Oficina de IA local
Comment=Agentes de IA para tu pyme, funcionando en tu equipo
Exec={exec}
Icon={icon}
Terminal=false
Categories=Office;Utility;
Keywords=IA;asistente;oficina;agentes;
StartupNotify=false
"""

METAINFO = """<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>{app_id}</id>
  <name>Nexus-Office</name>
  <summary>Oficina de IA local para pymes</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>LicenseRef-proprietary</project_license>
  <description><p>Agentes de IA por gremio, memoria, documentos y voz, funcionando entero en tu equipo.
  Elige el modelo de IA segun tu hardware y puede arrancar como servicio.</p></description>
  <launchable type="desktop-id">{app_id}.desktop</launchable>
  <releases><release version="{version}"/></releases>
  <content_rating type="oars-1.1"/>
</component>
"""


def log(msg: str) -> None:
    print(f"[bundles] {msg}", flush=True)


def tool(name: str) -> Path:
    TOOLS.mkdir(parents=True, exist_ok=True)
    p = TOOLS / name
    if not p.exists():
        log(f"descargando {name}")
        urllib.request.urlretrieve(URLS[name], p)
        p.chmod(0o755)
    return p


def appimagetool() -> Path:
    ext = TOOLS / "squashfs-root" / "AppRun"
    if not ext.exists():
        img = tool("appimagetool-x86_64.AppImage")
        subprocess.run([str(img), "--appimage-extract"], cwd=TOOLS, check=True, stdout=subprocess.DEVNULL)
    return ext


def installed_tree(dest: Path, arch: str, pip_platforms: list[str], work: Path) -> Path:
    """Copia el paquete portable y lo deja instalado: Python en venv/ y las dependencias base en su site-packages
    (con el pip de este equipo y --platform: sirve para construir el de ARM64 desde x86_64)."""
    tree = work / "nexus"
    shutil.rmtree(tree, ignore_errors=True)
    shutil.copytree(dest, tree, ignore=shutil.ignore_patterns("runtime", "wheelhouse", "install.ps1", "install.bat",
                                                              "start.bat", "Dockerfile", ".dockerignore"),
                    symlinks=True)   # GIMP trae enlaces a ficheros del sistema que no existen en el equipo de construccion
    rt = next((dest / "runtime").glob("cpython-*.tar.gz"))
    with tarfile.open(rt) as t:
        t.extractall(work, filter="tar") if sys.version_info >= (3, 12) else t.extractall(work)  # noqa: S202
    (work / "python").rename(tree / "venv")
    site = next((tree / "venv" / "lib").glob("python3.*")) / "site-packages"
    cmd = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-index", "--only-binary=:all:",
           "--find-links", str(dest / "wheelhouse"), "--target", str(site), "--python-version", "3.11",
           "--implementation", "cp", "--abi", "cp311", "--abi", "abi3", "--abi", "none", "-r", str(dest / "requirements.txt")]
    if (dest / "vendor" / "wheels").is_dir():
        cmd += ["--find-links", str(dest / "vendor" / "wheels")]
    for p in pip_platforms:
        cmd += ["--platform", p]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    lp = sorted((dest / "bundled" / "runtimes" / "llama-python").glob("*.whl"))
    if lp:   # motor «llama.cpp integrado»: ya instalado, porque AppImage y Flatpak son de solo lectura
        lcmd = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-index", "--no-deps",
                "--only-binary=:all:", "--python-version", "3.11", "--implementation", "cp", "--abi", "cp311",
                "--abi", "abi3", "--abi", "none", "--target", str(site), "--upgrade", *map(str, lp)]
        for p in pip_platforms:
            lcmd += ["--platform", p]
        subprocess.run(lcmd, check=True, stdout=subprocess.DEVNULL)
    if arch == platform.machine():                 # .pyc con el Python del paquete (arranque mas rapido)
        subprocess.run([str(tree / "venv" / "bin" / "python3"), "-m", "compileall", "-q", "-j0", str(tree / "nexus"), str(site)],
                       check=False, stdout=subprocess.DEVNULL)
    for exe in (tree / "bundled" / "runtimes").rglob("*"):
        if exe.name in ("llama-server", "sd-cli"):
            exe.chmod(0o755)
    (tree / "PLATAFORMA.txt").write_text(f"Nexus-Office - Linux {arch} (AppImage/Flatpak)\n", encoding="utf-8")
    return tree


def _icon(path: Path) -> None:
    from PIL import Image
    Image.open(ROOT / "packaging" / "windows" / "nexus.ico").convert("RGBA").resize((256, 256)).save(path)


def appimage(tree: Path, arch: str, out: Path, work: Path) -> Path:
    appdir = work / "AppDir"
    shutil.rmtree(appdir, ignore_errors=True)
    appdir.mkdir()
    shutil.move(str(tree), appdir / "nexus")
    run = appdir / "AppRun"
    run.write_text(LAUNCHER.format(kind="AppImage", here='$(dirname "$(readlink -f "$0")")/nexus',
                                   data="${XDG_DATA_HOME:-$HOME/.local/share}/nexus-office"))
    run.chmod(0o755)
    (appdir / "nexus-office.desktop").write_text(DESKTOP.format(exec="nexus-office", icon="nexus-office") + "X-AppImage-Name=Nexus-Office\n")
    _icon(appdir / "nexus-office.png")
    (appdir / ".DirIcon").symlink_to("nexus-office.png")
    env = {"ARCH": arch, "PATH": "/usr/bin:/bin", "HOME": str(work)}
    log(f"appimagetool {out.name}")
    subprocess.run([str(appimagetool()), "--no-appstream", "--comp", "zstd", "--runtime-file", str(tool(f"runtime-{arch}")),
                    str(appdir), str(out)], check=True, env=env, stdout=subprocess.DEVNULL)
    shutil.move(str(appdir / "nexus"), tree)       # el arbol lo reutiliza el Flatpak
    shutil.rmtree(appdir)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


def flatpak(tree: Path, arch: str, version: str, out: Path, work: Path) -> Path | None:
    if not shutil.which("flatpak"):
        log("Flatpak: falta el comando flatpak (apt install flatpak)")
        return None
    app = work / "flatpak-app"
    shutil.rmtree(app, ignore_errors=True)
    files = app / "files"
    files.mkdir(parents=True)
    shutil.move(str(tree), files / "nexus")
    (files / "bin").mkdir()
    launcher = files / "bin" / "nexus-office"
    launcher.write_text(LAUNCHER.format(kind="Flatpak", here="/app/nexus", data="${XDG_DATA_HOME:-$HOME/.local/share}/nexus-office"))
    launcher.chmod(0o755)
    share = files / "share"
    for rel, content in ((f"applications/{APP_ID}.desktop", DESKTOP.format(exec="nexus-office", icon=APP_ID)),
                         (f"metainfo/{APP_ID}.metainfo.xml", METAINFO.format(app_id=APP_ID, version=version))):
        (share / rel).parent.mkdir(parents=True, exist_ok=True)
        (share / rel).write_text(content, encoding="utf-8")
    icon = share / "icons/hicolor/256x256/apps" / f"{APP_ID}.png"
    icon.parent.mkdir(parents=True)
    _icon(icon)
    farch = {"x86_64": "x86_64", "aarch64": "aarch64"}[arch]
    (app / "metadata").write_text(f"""[Application]
name={APP_ID}
runtime=org.freedesktop.Platform/{farch}/{RUNTIME}
sdk=org.freedesktop.Sdk/{farch}/{RUNTIME}
command=nexus-office

[Context]
shared=network;ipc;
sockets=x11;wayland;pulseaudio;
devices=all;
filesystems=xdg-config/systemd:create;xdg-documents;xdg-download;

[Environment]
PYTHONDONTWRITEBYTECODE=1
""")
    for rel in (f"share/applications/{APP_ID}.desktop", f"share/icons/hicolor/256x256/apps/{APP_ID}.png"):
        (app / "export" / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(files / rel, app / "export" / rel)
    repo = work / "flatpak-repo"
    shutil.rmtree(repo, ignore_errors=True)
    log(f"flatpak build-export + build-bundle {out.name}")
    subprocess.run(["flatpak", "build-export", f"--arch={farch}", "--disable-sandbox", str(repo), str(app), "stable"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["flatpak", "build-bundle", f"--arch={farch}", f"--runtime-repo={FLATHUB}", str(repo), str(out), APP_ID, "stable"],
                   check=True)
    shutil.rmtree(app)
    shutil.rmtree(repo)
    log(f"-> {out.name} ({out.stat().st_size / 1e6:.1f} MB)")
    return out


def build(dest: Path, arch: str, pip_platforms: list[str], version: str, outdir: Path, work: Path) -> list[Path]:
    work.mkdir(parents=True, exist_ok=True)
    tree = installed_tree(dest, arch, pip_platforms, work)
    outs = [appimage(tree, arch, outdir / f"Nexus-Office-{version}-linux-{arch}.AppImage", work)]
    fp = flatpak(tree, arch, version, outdir / f"Nexus-Office-{version}-linux-{arch}.flatpak", work)
    if fp:
        outs.append(fp)
    shutil.rmtree(work, ignore_errors=True)
    return outs
