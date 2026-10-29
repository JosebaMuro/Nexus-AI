"""llama-server con GPU armado con paquetes de conda-forge (sin GitHub, Xcode ni el SDK de CUDA).

    python packaging/build_gpu_runtimes.py [plataforma...]
        -> .cache/desktop-runtimes/<plataforma>/llama/<variante>/

  macos-arm64    metal   Apple Silicon (shaders de Metal incrustados)
  linux-x86_64   cuda    NVIDIA (CUDA 12.9 + cuBLAS; driver 525 o posterior)
  linux-aarch64  cuda    NVIDIA Jetson Orin, JetPack 6 (build «tegra» de conda-forge, glibc 2.35)
  windows-x64    cuda    NVIDIA (CUDA 12.9 + cuBLAS)

Se copia llama-server con las bibliotecas que carga, siguiendo sus dependencias (Mach-O, ELF o PE). Las del sistema
(libc, driver de la GPU: libcuda / nvcuda.dll, Metal) no se copian. Linux y macOS: llama-server en bin/ y las
bibliotecas en lib/ (conda-forge las enlaza con rutas relativas). Windows: todo junto al .exe.
Vulkan (cualquier GPU en Windows y Linux) sale de packaging/build_desktop_runtimes.sh. Requisitos: pip install
macholib pyelftools pefile.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_python_runtimes as bpr  # noqa: E402  (micromamba de conda-forge)

ROOT = bpr.ROOT
OUT = ROOT / ".cache" / "desktop-runtimes"
LLAMA = "9923"          # build de llama.cpp en conda-forge
# plataforma -> [(subdir de conda, variante de Nexus-Office, especificacion del paquete)]
TARGETS = {
    "macos-arm64": [("osx-arm64", "metal", f"llama.cpp={LLAMA}")],
    "linux-x86_64": [("linux-64", "cuda", f"llama.cpp={LLAMA}=cuda129*")],
    "linux-aarch64": [("linux-aarch64", "cuda", f"llama.cpp={LLAMA}=cuda129_hb31b5ce_0")],   # tegra (Jetson)
    "windows-x64": [("win-64", "cuda", f"llama.cpp={LLAMA}=cuda129*")],
}
ENV = {"CONDA_OVERRIDE_OSX": "13.0", "CONDA_OVERRIDE_CUDA": "12.9", "CONDA_OVERRIDE_GLIBC": "2.28"}
# del sistema: nunca se copian (libc y compania, el driver de la GPU)
SYSTEM_ELF = ("libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so", "ld-linux", "libcuda.so", "libvulkan.so")


def log(msg: str) -> None:
    print(f"[gpu] {msg}", flush=True)


def _deps(path: Path) -> list[str]:
    head = path.read_bytes()[:4]
    if head == b"\x7fELF":
        from elftools.elf.dynamic import DynamicSection
        from elftools.elf.elffile import ELFFile
        with open(path, "rb") as f:
            elf = ELFFile(f)
            return [t.needed for s in elf.iter_sections() if isinstance(s, DynamicSection)
                    for t in s.iter_tags() if t.entry.d_tag == "DT_NEEDED"]
    if head[:2] == b"MZ":
        import pefile
        pe = pefile.PE(str(path), fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                               pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"]])
        names = [e.dll.decode() for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])]
        names += [e.dll.decode() for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])]
        pe.close()
        return names
    from macholib.MachO import MachO
    out = []
    for h in MachO(str(path)).headers:
        for lc, _cmd, data in h.commands:
            if lc.cmd in (0xC, 0x80000018):   # LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB
                out.append(data.rstrip(b"\0").decode())
    return out


def _prefix(subdir: str, specs: list[str]) -> Path:
    tag = specs[0].split("=")[-1].strip("*")
    prefix = bpr.CACHE / f"llama-{subdir}-{tag}"
    if not any((prefix / d).exists() for d in ("bin/llama-server", "Library/bin/llama-server.exe")):
        shutil.rmtree(prefix, ignore_errors=True)
        log(f"conda-forge {subdir}: {' '.join(specs)}")
        subprocess.run([str(bpr.micromamba()), "create", "-y", "-q", "--platform", subdir, "-p", str(prefix),
                        "-c", "conda-forge", *specs], check=True,
                       env={**os.environ, **ENV, **({"CONDA_OVERRIDE_GLIBC": "2.35"} if subdir == "linux-aarch64" else {})})
    return prefix


def build_one(target: str, subdir: str, variant: str, specs: list[str]) -> Path:
    prefix = _prefix(subdir, specs)
    win = subdir.startswith("win")
    libdir = prefix / ("Library/bin" if win else "lib")
    exe = prefix / ("Library/bin/llama-server.exe" if win else "bin/llama-server")
    dest = OUT / target / "llama" / variant
    shutil.rmtree(dest, ignore_errors=True)
    out_bin, out_lib = (dest, dest) if win else (dest / "bin", dest / "lib")
    out_bin.mkdir(parents=True)
    out_lib.mkdir(exist_ok=True)
    shutil.copy2(exe, out_bin / exe.name)
    lower = {p.name.lower(): p for p in libdir.iterdir()} if win else {}
    todo, seen = [exe], set()
    while todo:
        for dep in _deps(todo.pop()):
            name = dep.rsplit("/", 1)[-1]
            if win:
                src = lower.get(name.lower())          # el resto (kernel32, nvcuda...) es del sistema
            elif subdir.startswith("osx"):
                if not dep.startswith(("@rpath/", "@loader_path/")):
                    if not dep.startswith(("/usr/lib/", "/System/")):
                        raise RuntimeError(f"dependencia fuera del paquete: {dep}")
                    continue
                src = libdir / name
            else:
                src = None if name.startswith(SYSTEM_ELF) else libdir / name
            if src is None or name in seen or not src.exists():
                continue
            seen.add(name)
            shutil.copy2(src.resolve(), out_lib / name)   # el fichero real con el nombre enlazado
            todo.append(src.resolve())
    lic = next(iter(sorted((ROOT / ".cache" / "desktop-build" / "src").glob("llama_cpp_python-*/vendor/llama.cpp/LICENSE"))), None)
    if lic:   # mismo proyecto (MIT); el sdist lo descarga packaging/build_desktop_runtimes.sh
        shutil.copy2(lic, dest / "LICENSE-llama.cpp.txt")
    extra = ("NVIDIA CUDA runtime y cuBLAS (licencia NVIDIA CUDA Toolkit EULA, redistribuibles), " if variant == "cuda"
             else "")
    (dest / "THIRD-PARTY.txt").write_text(
        f"Bibliotecas de conda-forge incluidas: llama.cpp/ggml (MIT), {extra}OpenSSL 3 (Apache-2.0),\n"
        "LLVM/GCC runtimes (Apache-2.0 WITH LLVM-exception / GPL con excepcion de runtime).\n", encoding="utf-8")
    (dest / "nexus_runtime.json").write_text(json.dumps(
        {"runtime": "llama", "source": f"llama.cpp {LLAMA} de conda-forge ({subdir})", "variant": variant}) + "\n")
    size = sum(f.stat().st_size for f in dest.rglob("*") if f.is_file())
    log(f"{target}: {variant} listo ({len(seen)} bibliotecas, {size / 1e6:.0f} MB)")
    return dest


def main() -> None:
    for t in sys.argv[1:] or list(TARGETS):
        for subdir, variant, *specs in TARGETS[t]:
            build_one(t, subdir, variant, specs)


if __name__ == "__main__":
    main()
