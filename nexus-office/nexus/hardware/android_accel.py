"""CPU, GPU y NPU del movil en la app Android autonoma, y como acelerar el modelo con ellos.

* **CPU**: el SoC (``ro.soc.model``, ``ro.board.platform``...) y sus nucleos (``nexus.android_tune``).
* **GPU**: la app lleva, ademas de los llama-server de CPU (musl, estaticos), ``libllama-server-gpu.so``: llama.cpp
  enlazado con la libc de Android y con los backends Vulkan y OpenCL, que se cargan solo si el movil tiene sus
  drivers. Ese binario, con ``--list-devices``, dice que GPU puede usar de verdad (Adreno por OpenCL, Mali,
  Immortalis, Xclipse, PowerVR... por Vulkan). El nombre de la GPU llega tambien de la app (EGL, ``device_info.json``).
* **NPU**: se reconoce por el SoC y por las bibliotecas del fabricante (Hexagon de Qualcomm, APU de MediaTek, TPU de
  Google Tensor, NPU de Exynos). llama.cpp no tiene backend para ellas en Android sin los SDK propietarios, asi que
  se informa de ella pero el modelo va en la GPU o en la CPU.

El resultado de la sonda se guarda (``android_accel.json``) por version de la app y huella del sistema: se repite al
actualizar la app o Android. Si la GPU falla al arrancar llama-server, se marca y Nexus-Office sigue en CPU.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

GPU_EXE = "libllama-server-gpu.so"
DEV_LINE = re.compile(r"^\s+([\w.\-]+): (.*?) \((\d+) MiB, (\d+) MiB free\)\s*$")
VENDOR_LIBS = ("/vendor/lib64", "/system/vendor/lib64", "/system/lib64")
PROPS = ("ro.soc.manufacturer", "ro.soc.model", "ro.board.platform", "ro.hardware", "ro.hardware.egl",
         "ro.hardware.vulkan", "ro.product.manufacturer", "ro.product.model", "ro.build.fingerprint",
         "ro.build.version.sdk")

# nombre comercial de algunos SoC Qualcomm (el resto se muestra con su codigo)
SNAPDRAGON = {"sm8850": "Snapdragon 8 Elite Gen 5", "sm8750": "Snapdragon 8 Elite", "sm8650": "Snapdragon 8 Gen 3",
              "sm8635": "Snapdragon 8s Gen 3", "sm8550": "Snapdragon 8 Gen 2", "sm8475": "Snapdragon 8+ Gen 1",
              "sm8450": "Snapdragon 8 Gen 1", "sm8350": "Snapdragon 888", "sm8250": "Snapdragon 865",
              "sm8150": "Snapdragon 855", "sm7675": "Snapdragon 7+ Gen 3", "sm7550": "Snapdragon 7 Gen 3",
              "sm7475": "Snapdragon 7+ Gen 2", "sm7450": "Snapdragon 7 Gen 1", "sm6450": "Snapdragon 6 Gen 1",
              "pineapple": "Snapdragon 8 Gen 3", "kalama": "Snapdragon 8 Gen 2", "taro": "Snapdragon 8 Gen 1",
              "sun": "Snapdragon 8 Elite", "lahaina": "Snapdragon 888", "kona": "Snapdragon 865"}
TENSOR = {"gs101": "Google Tensor", "gs201": "Google Tensor G2", "zuma": "Google Tensor G3",
          "zumapro": "Google Tensor G4", "laguna": "Google Tensor G5"}


# ------------------------------------------------------------------ datos del sistema
def _run(cmd: list[str], timeout: float = 20, env: dict | None = None) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env, errors="replace")
        return (r.stdout or "") + (r.stderr or "")
    except (OSError, subprocess.SubprocessError):
        return ""


def props() -> dict:
    """Propiedades del sistema (getprop): SoC, GPU (egl/vulkan), modelo y huella de Android."""
    out: dict = {}
    getprop = "/system/bin/getprop"
    if os.path.exists(getprop):
        for line in _run([getprop], timeout=10).splitlines():
            m = re.match(r"^\[([^\]]+)\]: \[(.*)\]$", line)
            if m and m.group(1) in PROPS:
                out[m.group(1)] = m.group(2)
    return out


def device_info(path: Path | None = None) -> dict:
    """Lo que la app sabe por Java: SoC (Build.SOC_*), GPU (EGL: GL_RENDERER/GL_VENDOR) y version de Vulkan."""
    f = path or Path(os.environ.get("NEXUS_DEVICE_INFO", "") or
                     Path(os.environ.get("NEXUS_DATA_DIR", ".")).parent / "device_info.json")
    try:
        return json.loads(Path(f).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def vendor_lib(names: tuple[str, ...], dirs: tuple[str, ...] = VENDOR_LIBS) -> str:
    for d in dirs:
        for n in names:
            if os.path.exists(os.path.join(d, n)):
                return os.path.join(d, n)
    return ""


# ------------------------------------------------------------------ CPU / SoC
def soc(p: dict, info: dict) -> dict:
    mfr = (info.get("soc_manufacturer") or p.get("ro.soc.manufacturer") or "").strip()
    model = (info.get("soc_model") or p.get("ro.soc.model") or "").strip()
    platform = (p.get("ro.board.platform") or info.get("board") or "").strip()
    hardware = (p.get("ro.hardware") or info.get("hardware") or "").strip()
    key = " ".join((mfr, model, platform, hardware)).lower()
    family = ("qualcomm" if re.search(r"qti|qualcomm|qcom|\bsm\d{4}|\bsdm\d|\bmsm\d|snapdragon|"
                                      r"pineapple|kalama|taro|lahaina|kona|\bsun\b", key) else
              "google" if re.search(r"google|tensor|gs101|gs201|zuma|laguna", key) else
              "mediatek" if re.search(r"mediatek|\bmt\d{4}|dimensity|helio", key) else
              "samsung" if re.search(r"samsung|exynos|s5e\d|universal\d", key) else
              "unisoc" if re.search(r"unisoc|spreadtrum|\bums\d|\bt\d{3}\b", key) else "")
    commercial = ""
    for code in (model.lower(), platform.lower()):
        commercial = commercial or SNAPDRAGON.get(code, "") or TENSOR.get(code, "")
    label = " ".join(x for x in (mfr if mfr.lower() not in ("qti", "") else "Qualcomm" if family == "qualcomm" else "",
                                 model or platform) if x)
    if commercial and commercial not in label:
        label = f"{label} ({commercial})" if label else commercial
    return {"family": family, "manufacturer": mfr, "model": model, "platform": platform, "hardware": hardware,
            "name": label or hardware or platform}


# ------------------------------------------------------------------ NPU
def npu(s: dict, lib=vendor_lib) -> dict | None:
    """La NPU del SoC: por la familia y, si esta, por la biblioteca del fabricante que la maneja."""
    fam = s.get("family")
    if fam == "qualcomm":
        found = lib(("libQnnHtp.so", "libcdsprpc.so", "libSNPE.so"))
        return {"vendor": "qualcomm", "name": f"Qualcomm Hexagon NPU{' · ' + s['name'] if s.get('name') else ''}",
                "runtime_hint": "QNN / Hexagon SDK", "source": "vendor-lib" if found else "soc", "lib": found}
    if fam == "mediatek":
        found = lib(("libneuron_adapter.so", "libneuron_adapter_mgvi.so", "libapu_mdw.so", "libneuronusdk_adapter.mtk.so"))
        return {"vendor": "mediatek", "name": "MediaTek APU (NeuroPilot)", "runtime_hint": "NeuroPilot",
                "source": "vendor-lib" if found else "soc", "lib": found}
    if fam == "google":
        found = lib(("libedgetpu_util.so", "libedgetpu_client.google.so", "libdarwinn_client.so"))
        return {"vendor": "google", "name": f"{TENSOR.get(s.get('platform', '').lower(), 'Google Tensor')} TPU",
                "runtime_hint": "Edge TPU / NNAPI", "source": "vendor-lib" if found else "soc", "lib": found}
    if fam == "samsung":
        found = lib(("libeden_nn_on_system.so", "libenn_user_driver.so", "libenn_public_api_cpp.so"))
        if found or re.search(r"s5e9|exynos(9|2)\d{2,3}|exynos1[0-9]{3}", (s.get("model", "") + s.get("platform", "")).lower()):
            return {"vendor": "samsung", "name": "Samsung Exynos NPU", "runtime_hint": "Exynos AI Studio (ENN)",
                    "source": "vendor-lib" if found else "soc", "lib": found}
    return None


# ------------------------------------------------------------------ GPU
def gpu_env(libs: str) -> dict:
    """Entorno de llama-server GPU: sus bibliotecas primero; despues las del sistema (Vulkan) y las del
    fabricante (libOpenCL.so de Adreno vive en /vendor/lib64)."""
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    paths = [libs] + [d for d in ("/system/lib64", "/vendor/lib64", "/system/vendor/lib64") if os.path.isdir(d)]
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    return env


def parse_devices(out: str) -> list[dict]:
    devs = []
    for line in out.splitlines():
        m = DEV_LINE.match(line)
        if not m:
            continue
        name = m.group(1)
        backend = "opencl" if "opencl" in name.lower() else "vulkan" if name.lower().startswith("vulkan") else name.lower()
        devs.append({"dev": name, "backend": backend, "name": m.group(2).strip(),
                     "mem_mb": int(m.group(3)), "free_mb": int(m.group(4))})
    return devs


def gpu_vendor(name: str) -> str:
    n = name.lower()
    for key, vendor in (("adreno", "qualcomm"), ("qualcomm", "qualcomm"), ("mali", "arm"), ("immortalis", "arm"),
                        ("xclipse", "samsung"), ("powervr", "imagination"), ("img ", "imagination"),
                        ("maleoon", "huawei"), ("nvidia", "nvidia"), ("intel", "intel")):
        if key in n:
            return vendor
    return "unknown"


def pick(devs: list[dict]) -> dict | None:
    """El dispositivo con el que llama.cpp corre mejor en cada GPU movil.
    Adreno: OpenCL (kernels de Qualcomm); su Vulkan suele ir peor que la CPU. Resto: Vulkan."""
    if not devs:
        return None
    adreno = [d for d in devs if "adreno" in d["name"].lower()]
    if adreno:
        return next((d for d in adreno if d["backend"] == "opencl"), None)
    return next((d for d in devs if d["backend"] == "vulkan"), None)


def probe_gpu(libs: str, exe_name: str = GPU_EXE) -> dict:
    exe = Path(libs) / exe_name if libs else None
    if not exe or not exe.is_file():
        return {"binary": False, "devices": [], "error": "la APK no trae llama-server con GPU"}
    out = _run([str(exe), "--list-devices"], timeout=45, env=gpu_env(libs))
    devs = parse_devices(out)
    res = {"binary": True, "devices": devs}
    if not devs:
        res["error"] = "ningun backend GPU (Vulkan/OpenCL) encontro una GPU utilizable"
        tail = [ln for ln in out.splitlines() if ln.strip() and "linker" not in ln][-4:]
        if tail:
            res["log"] = " | ".join(tail)[-400:]
    return res


# ------------------------------------------------------------------ resultado completo (con cache)
def _cache_file() -> Path:
    from .. import paths
    return paths.data_dir() / "android_accel.json"


def _cache_key(p: dict, libs: str) -> str:
    from .. import __version__
    exe = Path(libs) / GPU_EXE if libs else None
    size = exe.stat().st_size if exe and exe.is_file() else 0
    return f"{__version__}|{p.get('ro.build.fingerprint', '')}|{size}"


def detect(libs: str | None = None, use_cache: bool = True) -> dict:
    """{"soc", "npu", "gpu": {"name", "vendor", "devices", "use", ...}}. ``use`` es el dispositivo de llama.cpp
    elegido (o None: CPU)."""
    libs = os.environ.get("NEXUS_ANDROID_LIBS", "") if libs is None else libs
    p, info = props(), device_info()
    key = _cache_key(p, libs)
    cache: dict = {}
    if use_cache:
        try:
            cache = json.loads(_cache_file().read_text(encoding="utf-8"))
        except (OSError, ValueError):
            cache = {}
    if cache.get("key") == key and "gpu" in cache:
        probe = cache["gpu"].get("probe", {})
    else:
        probe = probe_gpu(libs)
        cache = {"key": key}
    s = soc(p, info)
    devs = probe.get("devices", [])
    use = pick(devs)
    egl = (info.get("gl_renderer") or "").strip()
    name = (use or (devs[0] if devs else {})).get("name") or egl or \
        {"adreno": "Qualcomm Adreno", "mali": "ARM Mali"}.get((p.get("ro.hardware.egl") or "").lower(), "")
    broken = cache.get("gpu_broken", "")
    gpu = {"name": name, "vendor": gpu_vendor(name or p.get("ro.hardware.egl", "")), "egl": egl,
           "gl_vendor": info.get("gl_vendor", ""), "vulkan": info.get("vulkan_version", "") or p.get("ro.hardware.vulkan", ""),
           "devices": devs, "use": None if broken else use, "broken": broken, "probe": probe,
           "binary": probe.get("binary", False)}
    if devs and not use and not broken:
        gpu["note"] = ("Adreno sin OpenCL: su Vulkan es mas lento que la CPU en llama.cpp"
                       if any("adreno" in d["name"].lower() for d in devs) else "")
    res = {"soc": s, "npu": npu(s), "gpu": gpu, "props": {k: v for k, v in p.items() if k != "ro.build.fingerprint"}}
    cache.update({"key": key, "gpu": gpu, "soc": s, "npu": res["npu"]})
    try:
        _cache_file().write_text(json.dumps(cache, indent=1, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass
    return res


def mark_gpu_broken(reason: str) -> None:
    """La GPU fallo al arrancar llama-server: se usa la CPU hasta la proxima version de la app o de Android."""
    f = _cache_file()
    try:
        cache = json.loads(f.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        cache = {}
    cache["gpu_broken"] = reason[:300] or "fallo al arrancar"
    try:
        f.write_text(json.dumps(cache, indent=1, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


def is_gpu_exe(exe) -> bool:
    return Path(str(exe)).name == GPU_EXE
