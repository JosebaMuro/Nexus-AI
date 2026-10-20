"""CPython 3.11 portable para cada plataforma, armado con paquetes de conda-forge (sin GitHub).

    python packaging/build_python_runtimes.py [plataforma...]
        -> .cache/desktop-runtimes/<plataforma>/cpython-3.11.17+conda-<triple>-install_only.tar.gz

El resultado tiene la misma forma que python-build-standalone «install_only» (una carpeta python/ con
bin/python3 o python.exe), asi que install.sh / install.ps1 lo usan igual desde runtime/. Es relocalizable:
conda-forge enlaza sus bibliotecas con rutas relativas ($ORIGIN, @loader_path). Ajustes para moverlo:
  - certificados: SSL_CERT_FILE apunta a los de conda (python/ssl/cacert.pem) si no se ha definido otro;
  - Windows: las DLL de Library\\bin se copian junto a python.exe (ahi las busca Windows siempre).
Usa micromamba (conda-forge) con --platform para descargar los paquetes de otras plataformas.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / ".cache" / "conda"
OUT = ROOT / ".cache" / "desktop-runtimes"
PY = "3.11.17"
MICROMAMBA = "https://conda.anaconda.org/conda-forge/linux-64/micromamba-2.9.0-0.tar.bz2"
TARGETS = {   # plataforma de Nexus-Office -> (subdir de conda, triple de python-build-standalone)
    "linux-x86_64": ("linux-64", "x86_64-unknown-linux-gnu"),
    "linux-aarch64": ("linux-aarch64", "aarch64-unknown-linux-gnu"),
    "windows-x64": ("win-64", "x86_64-pc-windows-msvc"),
    "macos-arm64": ("osx-arm64", "aarch64-apple-darwin"),
    "macos-x86_64": ("osx-64", "x86_64-apple-darwin"),
}
# windows-arm64: conda-forge no tiene CPython 3.11 para ARM64; Windows 11 ARM ejecuta el de x64 (emulado)
CERTS = ("import os, sys; os.environ.setdefault('SSL_CERT_FILE', os.path.join(sys.prefix, "
         "'Library' if os.name == 'nt' else '', 'ssl', 'cacert.pem'))\n")
DROP_DIRS = {"conda-meta", "include", "man", "doc", "info", "test", "tests", "idlelib", "turtledemo", "pkgconfig",
             "cmake", "Tools", "libs", "gtk-doc", "aclocal"}
KEEP_BIN = ("python", "pip")


def log(msg: str) -> None:
    print(f"[python] {msg}", flush=True)


def micromamba() -> Path:
    exe = CACHE / "mm" / "bin" / "micromamba"
    if not exe.exists():
        exe.parent.parent.mkdir(parents=True, exist_ok=True)
        arc = CACHE / "micromamba.tar.bz2"
        log("descargando micromamba (conda-forge)")
        urllib.request.urlretrieve(MICROMAMBA, arc)
        with tarfile.open(arc) as t:
            t.extractall(exe.parent.parent, filter="data")
        exe.chmod(0o755)
    return exe


def create(subdir: str) -> Path:
    prefix = CACHE / "py" / subdir
    if (prefix / "conda-meta").is_dir():
        return prefix
    shutil.rmtree(prefix, ignore_errors=True)
    env = {**os.environ, "MAMBA_ROOT_PREFIX": str(CACHE / "root"), "CONDA_OVERRIDE_OSX": "13.0"}
    log(f"{subdir}: python={PY} + pip")
    subprocess.run([str(micromamba()), "create", "-y", "-q", "--platform", subdir, "-p", str(prefix), "-c", "conda-forge",
                    "--override-channels", f"python={PY}", "pip", "ca-certificates"], check=True, env=env)
    return prefix


def trim(root: Path, windows: bool) -> None:
    for d in sorted(root.rglob("*"), key=lambda p: -len(p.parts)):
        if d.is_dir() and d.name in DROP_DIRS and not (d.name == "test" and "site-packages" in d.parts):
            shutil.rmtree(d, ignore_errors=True)
    for f in root.rglob("*"):
        if f.is_file() and (f.suffix in (".a", ".pdb", ".lib") or f.name.endswith(".prl")):
            f.unlink()
    for cfg in (root / "lib").glob("python3.*/config-3.*"):
        shutil.rmtree(cfg, ignore_errors=True)
    if windows:
        # micromamba en Linux enlaza los paquetes noarch (pip, setuptools...) con la estructura de Unix
        # (lib/python3.11/site-packages, bin/): en Windows van a Lib\site-packages
        unix_site = next((root / "lib").glob("python3.*/site-packages"), None) if (root / "lib").is_dir() else None
        if unix_site:
            dest = root / "Lib" / "site-packages"
            for item in unix_site.iterdir():
                if not (dest / item.name).exists():
                    shutil.move(str(item), dest / item.name)
        for d in ("lib", "bin"):
            shutil.rmtree(root / d, ignore_errors=True)
        lib_bin = root / "Library" / "bin"
        for dll in lib_bin.glob("*.dll"):   # junto a python.exe: Windows siempre busca ahi
            if not (root / dll.name).exists():
                shutil.copy2(dll, root / dll.name)
        for exe in lib_bin.glob("*.exe"):
            exe.unlink()
    else:
        for f in (root / "bin").iterdir():
            if not f.name.startswith(KEEP_BIN):
                f.unlink() if f.is_file() or f.is_symlink() else shutil.rmtree(f)
    site = root / "Lib" / "site-packages" if windows else next((root / "lib").glob("python3.*/site-packages"))
    (site / "nexus_certs.pth").write_text(CERTS, encoding="utf-8")


def build(target: str) -> Path:
    subdir, triple = TARGETS[target]
    out = OUT / target / f"cpython-{PY}+conda-{triple}-install_only.tar.gz"
    if out.exists():
        log(f"{target}: ya existe {out.name}")
        return out
    prefix = create(subdir)
    stage = CACHE / "stage" / target / "python"
    shutil.rmtree(stage.parent, ignore_errors=True)
    shutil.copytree(prefix, stage, symlinks=True)
    trim(stage, subdir.startswith("win"))
    (stage / "NEXUS_RUNTIME.txt").write_text(f"CPython {PY} de conda-forge para {triple} (Nexus-Office).\n", encoding="utf-8")
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".part")
    with tarfile.open(tmp, "w:gz", compresslevel=9) as t:
        t.add(stage, arcname="python")
    tmp.replace(out)
    log(f"{target}: {out.name} ({out.stat().st_size >> 20} MB)")
    return out


def main() -> int:
    targets = sys.argv[1:] or list(TARGETS)
    for t in targets:
        if t == "windows-arm64":
            log("windows-arm64: se usa el runtime de windows-x64 (emulado por Windows 11 ARM)")
            continue
        build(t)
    return 0


if __name__ == "__main__":
    sys.exit(main())
