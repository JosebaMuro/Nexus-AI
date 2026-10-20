"""Compila la app Android «Nexus-Office autonomo» (APK firmado, ARM64) sin Gradle ni Android Studio.

    bash packaging/build_android_runtime.sh          # una vez: Python + llama.cpp para ARM64 en .cache/android-runtime
    venv/python packaging/build_apk_autonomo.py      # -> dist/Nexus-Office-<version>-android-autonomo.apk

La APK lo lleva todo menos los modelos de IA:
  lib/arm64-v8a/       libnexusld.so (cargador de musl) y llama-server en 3 variantes de CPU, y llama-server con GPU
                       (libllama-server-gpu.so + ggml con Vulkan, OpenCL y CPU por variantes de ARM)
  assets/runtime.zip   Python 3.12 + dependencias (py/) y el codigo de Nexus-Office (app/nexus), con .pyc precompilados
  assets/runtime.json  ficheros y bytes del runtime: la app comprueba con ellos que se ha descomprimido entero
La primera vez que se abre, la app descomprime runtime.zip en sus datos (en paralelo) y arranca Nexus-Office en 127.0.0.1:8775.
Usa las mismas herramientas de Android que packaging/build_apk.py (SDK o paquetes de Ubuntu) y su misma firma.
"""
from __future__ import annotations

import compileall
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_apk as base  # noqa: E402

ROOT = base.ROOT
SRC = ROOT / "android-autonomo"
RUNTIME = ROOT / ".cache" / "android-runtime"
OUT = ROOT / "build" / "apk-autonomo"
VERSION = base.VERSION
MIN_SDK = 28
NATIVE = ("libnexusld.so", "libnexussys.so", "libllama-server.so", "libllama-server-dotprod.so", "libllama-server-i8mm.so")
# llama.cpp con GPU (libc de Android, backends Vulkan y OpenCL que se cargan si hay driver): opcional, lo compila
# build_android_runtime.sh si encuentra el NDK. Nunca un libOpenCL.so propio: taparia el del fabricante.
GPU_NATIVE = ("libllama-server-gpu.so", "libllama-server-impl.so", "libllama-common.so", "libmtmd.so", "libllama.so",
              "libggml.so", "libggml-base.so", "libggml-vulkan.so", "libggml-opencl.so", "libc++_shared.so")


def gpu_native() -> list[str]:
    """Bibliotecas del llama-server con GPU presentes en el runtime (todas o ninguna)."""
    lib = RUNTIME / "lib"
    names = list(GPU_NATIVE) + sorted(p.name for p in lib.glob("libggml-cpu-*.so"))
    if not (lib / GPU_NATIVE[0]).is_file():
        return []
    missing = [n for n in GPU_NATIVE if not (lib / n).is_file()]
    if missing or (lib / "libOpenCL.so").exists():
        sys.exit(f"Runtime GPU incompleto o con libOpenCL.so propio ({', '.join(missing) or 'libOpenCL.so'}): "
                 "vuelve a ejecutar packaging/build_android_runtime.sh")
    return names

# lo que no hace falta en el movil: pruebas, IDLE/tkinter, cabeceras, la biblioteca estatica, pip...
SKIP_DIR = re.compile(r"^(test|tests|idlelib|tkinter|turtledemo|ensurepip|lib2to3|pydoc_data|__pycache__|config-3\.12-.*)$")
SKIP_FILE = re.compile(r"\.(a|pyi|c|h|pxd|pyx)$|^(2to3|idle3|pydoc3|python3-config|python3\.12-config)")


def stage(dest: Path) -> int:
    """Copia el runtime (py/) y Nexus-Office (app/nexus) recortados, y precompila los .pyc. Devuelve los bytes."""
    shutil.rmtree(dest, ignore_errors=True)
    py = RUNTIME / "py"
    ign = lambda d, names: [n for n in names if SKIP_DIR.match(n) or SKIP_FILE.search(n)]  # noqa: E731
    (dest / "py" / "bin").mkdir(parents=True)
    shutil.copy2(py / "bin" / "python3.12", dest / "py" / "bin" / "python3.12")
    shutil.copytree(py / "lib", dest / "py" / "lib", ignore=ign, symlinks=False,
                    ignore_dangling_symlinks=True)
    for dist in (dest / "py" / "lib" / "python3.12" / "site-packages").glob("*/bin"):
        shutil.rmtree(dist, ignore_errors=True)
    shutil.copytree(ROOT / "nexus", dest / "app" / "nexus",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    host = shutil.which("python3.12")
    if host:   # mismo numero magico que el Python de la app: el movil no compila nada al arrancar
        subprocess.run([host, "-m", "compileall", "-q", "-j", "0", "--invalidation-mode", "unchecked-hash",
                        str(dest / "py" / "lib" / "python3.12"), str(dest / "app")], check=False)
    elif sys.version_info[:2] == (3, 12):
        compileall.compile_dir(str(dest), quiet=1, workers=0,
                               invalidation_mode=compileall.py_compile.PycInvalidationMode.UNCHECKED_HASH)
    return sum(f.stat().st_size for f in dest.rglob("*") if f.is_file())


def runtime_zip(staged: Path, dest: Path) -> dict:
    """Comprime el runtime y devuelve {"files", "bytes"}: la app lo comprueba tras descomprimir (assets/runtime.json)."""
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(staged.rglob("*")):
            if f.is_file() and not f.is_symlink():
                z.write(f, f.relative_to(staged).as_posix())
        infos = [i for i in z.infolist() if not i.is_dir()]
    return {"files": len(infos), "bytes": sum(i.file_size for i in infos), "version": VERSION}


def build() -> Path:
    missing = [n for n in NATIVE if not (RUNTIME / "lib" / n).is_file()] + \
              ([] if (RUNTIME / "py" / "bin" / "python3.12").is_file() else ["py/bin/python3.12"])
    if missing:
        sys.exit(f"Falta el runtime de Android ({', '.join(missing)}): ejecuta antes bash packaging/build_android_runtime.sh")
    aapt2, zipalign = base.tool("aapt2"), base.tool("zipalign")
    apksigner = base.tool("apksigner", bat=True)
    d8, dx = base.tool("d8", bat=True), base.tool("dalvik-exchange", "dx")
    javac = (base.JDK / "bin" / f"javac{base.EXE}") if base.JDK.exists() else Path(shutil.which("javac") or "javac")
    if not all((aapt2, zipalign, apksigner, d8 or dx)):
        sys.exit("Faltan herramientas de Android (aapt2, zipalign, apksigner, d8/dx). Ver docs/PLATAFORMAS.md.")
    api_jar = base.android_jar()
    shutil.rmtree(OUT, ignore_errors=True)
    for d in ("gen", "classes", "dex"):
        (OUT / d).mkdir(parents=True)

    print("[apk] preparando el runtime (Python + Nexus-Office) ...")
    size = stage(OUT / "runtime")
    meta = runtime_zip(OUT / "runtime", OUT / "runtime.zip")
    print(f"[apk] runtime: {size / 1e6:.0f} MB -> runtime.zip {(OUT / 'runtime.zip').stat().st_size / 1e6:.0f} MB")

    base.icons(SRC / "res", shield=False)
    code = base.version_code()
    base.run([aapt2, "compile", "--dir", SRC / "res", "-o", OUT / "res.zip"])
    # el manifiesto usa atributos de API 29+ (networkSecurityConfig, foregroundServiceType): se enlaza con el de API 34
    base.run([aapt2, "link", "-o", OUT / "base.apk", "-I", api_jar, "--manifest", SRC / "AndroidManifest.xml",
              "-A", SRC / "assets", "--java", OUT / "gen", "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", "34",
              "--version-code", str(code), "--version-name", VERSION, OUT / "res.zip"])
    sources = [str(p) for p in (SRC / "src").rglob("*.java")] + [str(p) for p in (OUT / "gen").rglob("*.java")]
    base.run([javac, "-encoding", "UTF-8", "--release", "8", "-Xlint:-options", "-nowarn", "-cp", api_jar,
              "-d", OUT / "classes", *sources])
    classes = [str(p) for p in (OUT / "classes").rglob("*.class")]
    if d8:
        base.run([d8, "--release", "--min-api", str(MIN_SDK), "--lib", api_jar, "--output", OUT / "dex", *classes])
    else:
        base.run([dx, "--dex", f"--min-sdk-version={MIN_SDK}", f"--output={OUT / 'dex' / 'classes.dex'}", OUT / "classes"])
    with zipfile.ZipFile(OUT / "base.apk", "a", zipfile.ZIP_DEFLATED) as z:
        z.write(OUT / "dex" / "classes.dex", "classes.dex")
        gpu = gpu_native()
        print(f"[apk] llama.cpp con GPU (Vulkan/OpenCL): {'si, ' + str(len(gpu)) + ' bibliotecas' if gpu else 'no (sin NDK)'}")
        for n in list(NATIVE) + gpu:
            z.write(RUNTIME / "lib" / n, f"lib/arm64-v8a/{n}")
        # sin comprimir dentro de la APK: la app lo lee con openFd para saber su tamano y mostrar el progreso
        z.write(OUT / "runtime.zip", "assets/runtime.zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("assets/runtime.json", json.dumps(meta))
        z.writestr("assets/LICENSES/LEEME.txt",
                   "Python 3.12 (PSF License), llama.cpp y ggml (MIT), LLVM libc++ (Apache 2.0 con excepcion LLVM), musl (MIT), OpenSSL (Apache 2.0), SQLite (dominio publico), "
                   "zlib, bzip2, xz, libffi; y los paquetes de Python incluidos, cada uno con su licencia en "
                   "py/lib/python3.12/site-packages/*.dist-info.\n")
    base.run([zipalign, "-f", "-p", "4", OUT / "base.apk", OUT / "aligned.apk"])
    ks, pw = base.keystore()
    dest = ROOT / "dist" / f"Nexus-Office-{VERSION}-android-autonomo.apk"
    dest.parent.mkdir(exist_ok=True)
    base.run([apksigner, "sign", "--ks", ks, "--ks-key-alias", "nexus", "--ks-pass", f"pass:{pw}", "--key-pass", f"pass:{pw}",
              "--min-sdk-version", str(MIN_SDK), "--out", dest, OUT / "aligned.apk"])
    base.run([apksigner, "verify", "--min-sdk-version", str(MIN_SDK), dest])
    print(f"[apk] -> {dest} ({dest.stat().st_size / 1e6:.1f} MB)")
    return dest


if __name__ == "__main__":
    build()
