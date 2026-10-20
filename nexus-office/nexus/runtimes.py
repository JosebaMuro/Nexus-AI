"""Gestor de runtimes nativos precompilados (llama.cpp y stable-diffusion.cpp).

Descarga de GitHub la variante optima para el hardware (CPU, CUDA, ROCm, Vulkan, SYCL, OpenVINO,
Metal), la guarda en data/runtimes/<runtime>/<variante>/ y localiza el ejecutable.
Sin compilar nada y sin PyTorch: maxima compatibilidad y arranque inmediato.
"""
from __future__ import annotations

import json
import os
import platform
import re
import shutil
import stat
import sys
import tarfile
import zipfile
from pathlib import Path

from . import log, net, paths

L = log.get("runtimes")

RUNTIMES = {
    "llama": {"repo": "ggml-org/llama.cpp", "exe": ["llama-server"], "desc": "LLM / multimodal (llama-server)"},
    "sd": {"repo": "leejet/stable-diffusion.cpp", "exe": ["sd-cli", "sd"], "desc": "Generacion de imagen (sd.cpp)"},
}


def _os() -> str:
    return "win" if os.name == "nt" else "mac" if sys.platform == "darwin" else "linux"


def _arch() -> str:
    return "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"


def asset_pattern(runtime: str, variant: str) -> re.Pattern:
    o, a = _os(), _arch()
    if runtime == "llama":
        if o == "win":
            v = {"rocm": r"rocm-[\d.]+", "openvino": r"openvino-[\d.]+", "cuda": r"cuda-12\.\d"}.get(variant, re.escape(variant))
            return re.compile(rf"^llama-b\d+-bin-win-{v}-{a}\.zip$")
        if o == "mac":
            return re.compile(rf"^llama-b\d+-bin-macos-{a}\.tar\.gz$")
        v = {"cpu": "", "rocm": r"rocm-[\d.]+-", "openvino": r"openvino-[\d.]+-", "cuda": r"cuda-12\.\d-",
             "sycl": "sycl-fp16-"}.get(variant, f"{variant}-")
        return re.compile(rf"^llama-b\d+-bin-ubuntu-{v}{a}\.tar\.gz$")
    if runtime == "sd":
        if o == "win":
            v = {"cuda": "cuda12", "rocm": r"rocm-[\d.]+"}.get(variant, variant)
            return re.compile(rf"^sd-.*-bin-win-{v}-x64\.zip$")
        if o == "mac":
            return re.compile(r"^sd-.*-bin-Darwin-.*-arm64\.zip$")
        v = {"cpu": "", "vulkan": "-vulkan", "rocm": r"-rocm-[\d.]+"}.get(variant, f"-{variant}")
        return re.compile(rf"^sd-.*-bin-Linux-.*-x86_64{v}\.zip$")
    raise ValueError(runtime)


def cudart_pattern(runtime: str) -> re.Pattern | None:
    if _os() != "win":
        return None
    return re.compile(r"^cudart-llama-bin-win-cuda-12\.\d-x64\.zip$" if runtime == "llama" else r"^cudart-sd-bin-win-cu12-x64\.zip$")


def candidates(profile: dict, runtime: str) -> list[str]:
    """Variantes en orden de preferencia para el perfil. El autoajuste puede explorar las siguientes."""
    acc = profile["accelerator"]
    if _os() == "mac":
        return ["metal"]
    vendor = profile["id"].split("-")[1] if "-" in profile["id"] else "cpu"
    igpu = profile["id"].endswith("igpu")
    if acc == "cuda":
        return ["cuda", "vulkan", "cpu"]
    if vendor == "amd" and not igpu:
        return ["vulkan", "rocm", "cpu"]
    if vendor == "intel" and not igpu:
        return (["vulkan", "sycl", "cpu"] if runtime == "llama" else ["vulkan", "cpu"])
    if vendor == "intel" and igpu and runtime == "llama":
        # iGPU Intel: la CPU suele ganar en modelos pequenos; OpenVINO/Vulkan quedan como alternativas medibles
        return ["cpu", "openvino", "vulkan"]
    return ["cpu"]


BUNDLED = paths.ROOT / "bundled" / "runtimes"   # paquetes offline: llama-server ya incluido (sin descargar nada)


def _has_avx2() -> bool:
    if platform.machine().lower() not in ("x86_64", "amd64"):
        return True
    try:
        from .hardware.detect import _cpu_flags
        return "avx2" in _cpu_flags()
    except Exception:  # noqa: BLE001
        return True


def bundled_variants(runtime: str) -> list[str]:
    """Variantes incluidas en el paquete, en orden de preferencia para esta CPU."""
    base = BUNDLED / runtime
    if not base.is_dir():
        return []
    have = sorted(p.name for p in base.iterdir() if p.is_dir())
    order = ["cpu", "cpu-compat"] if _has_avx2() else ["cpu-compat"]
    return [v for v in order if v in have] + [v for v in have if v not in order and not v.startswith("cpu")]


_USABLE: dict[Path, bool] = {}


def bundled_exe(runtime: str, variant: str) -> Path | None:
    """Ejecutable de una variante incluida en el paquete. Las de GPU se prueban una vez (``--version``): sin el
    driver (libvulkan.so.1 / vulkan-1.dll) no arrancan y se pasa a la siguiente."""
    p = RuntimeManager._find_in(BUNDLED / runtime / variant, runtime)
    if not p:
        return None
    if os.name != "nt" and not os.access(p, os.X_OK):
        try:   # el zip o la copia pudo perder el permiso de ejecucion
            p.chmod(p.stat().st_mode | 0o755)
        except OSError:
            return None
    if variant.startswith("cpu"):
        return p
    if p not in _USABLE:
        import subprocess
        try:
            r = subprocess.run([str(p), "--version"], capture_output=True, timeout=30, cwd=str(p.parent))
            _USABLE[p] = r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            _USABLE[p] = False
        if not _USABLE[p]:
            L.info("Runtime %s %s incluido pero no arranca en este equipo (sin driver de GPU): se usa otro", runtime, variant)
    return p if _USABLE[p] else None


class RuntimeManager:
    def __init__(self) -> None:
        self.root = paths.sub("runtimes")
        self.progress: dict[str, dict] = {}

    def _dir(self, runtime: str, variant: str) -> Path:
        return self.root / runtime / variant

    def find_exe(self, runtime: str, variant: str) -> Path | None:
        return self._find_in(self._dir(runtime, variant), runtime)

    @staticmethod
    def _find_in(d: Path, runtime: str) -> Path | None:
        if not d.exists():
            return None
        suffix = ".exe" if os.name == "nt" else ""
        for name in RUNTIMES[runtime]["exe"]:
            for p in d.rglob(name + suffix):
                if p.is_file():
                    return p
        return None

    def installed(self) -> dict:
        out = {}
        for rt in RUNTIMES:
            base = self.root / rt
            out[rt] = sorted(p.name for p in base.iterdir() if p.is_dir() and self.find_exe(rt, p.name)) if base.exists() else []
        return out

    def exe(self, runtime: str, profile: dict, preferred: str = "") -> tuple[Path, str] | None:
        # binario fijado por quien empaqueta Nexus-Office (p. ej. la app Android autonoma: libllama-server.so)
        forced = os.environ.get(f"NEXUS_RUNTIME_{runtime.upper()}", "")
        if forced and Path(forced).is_file():
            return Path(forced), "embedded"
        order = ([preferred] if preferred else []) + candidates(profile, runtime)
        for v in order + self.installed().get(runtime, []):
            p = self.find_exe(runtime, v)
            if p:
                return p, v
            if not v.startswith("cpu"):   # GPU incluida en el paquete offline (vulkan, metal)
                p = bundled_exe(runtime, v)
                if p:
                    return p, v
        for v in bundled_variants(runtime):   # incluido en el paquete offline: CPU
            if v.startswith("cpu"):
                p = bundled_exe(runtime, v)
                if p:
                    return p, "cpu"
        # binarios del sistema (paquetes de Termux/Android, distros Linux, Homebrew...)
        for name in RUNTIMES[runtime]["exe"]:
            if name == "sd":  # nombre demasiado generico (existe otra utilidad 'sd'): solo sd-cli
                continue
            found = shutil.which(name)
            if found:
                return Path(found), "system"
        return None

    def install(self, runtime: str, variant: str) -> Path:
        key = f"{runtime}:{variant}"
        self.progress[key] = {"status": "buscando", "done": 0, "total": 0}
        try:
            # "latest" puede apuntar a una etiqueta sin binarios: se busca en las ultimas releases
            releases = net.get_json(f"https://api.github.com/repos/{RUNTIMES[runtime]['repo']}/releases",
                                    params={"per_page": 10})
            pat = asset_pattern(runtime, variant)
            rel, assets = {}, []
            for rel in releases:
                assets = [a for a in rel.get("assets", []) if pat.match(a["name"])]
                if assets:
                    break
            if not assets:
                raise RuntimeError(f"No hay binario '{variant}' para {_os()}-{_arch()} en las ultimas releases")
            wanted = [assets[0]]
            cp = cudart_pattern(runtime) if variant == "cuda" else None
            if cp:
                wanted += [a for a in rel["assets"] if cp.match(a["name"])][:1]
            dest = self._dir(runtime, variant)
            tmp = dest.with_name(dest.name + ".tmp")
            shutil.rmtree(tmp, ignore_errors=True)
            tmp.mkdir(parents=True)
            for a in wanted:
                arc = paths.sub("cache") / a["name"]
                self.progress[key] = {"status": f"descargando {a['name']}", "done": 0, "total": a["size"]}
                net.download(a["browser_download_url"], arc,
                             progress=lambda d, t: self.progress[key].update(done=d, total=t))
                self.progress[key]["status"] = "extrayendo"
                _extract(arc, tmp)
                arc.unlink(missing_ok=True)
            shutil.rmtree(dest, ignore_errors=True)
            tmp.rename(dest)
            (dest / "nexus_runtime.json").write_text(json.dumps({"tag": rel.get("tag_name"), "assets": [a["name"] for a in wanted]}))
            exe = self.find_exe(runtime, variant)
            if not exe:
                raise RuntimeError("Ejecutable no encontrado tras extraer")
            if os.name != "nt":
                for f in exe.parent.iterdir():
                    f.chmod(f.stat().st_mode | stat.S_IEXEC)
            self.progress[key] = {"status": "instalado", "done": 1, "total": 1, "tag": rel.get("tag_name")}
            L.info("Runtime %s (%s) instalado: %s", runtime, variant, exe)
            return exe
        except Exception as e:
            self.progress[key] = {"status": "error", "error": str(e)}
            raise


def _extract(archive: Path, dest: Path) -> None:
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for m in z.namelist():  # proteccion contra rutas maliciosas
                if (dest / m).resolve().is_relative_to(dest.resolve()):
                    z.extract(m, dest)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest, filter="data")
