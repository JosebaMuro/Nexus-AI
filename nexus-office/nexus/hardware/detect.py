"""Deteccion de hardware multiplataforma: CPU, RAM, disco, GPUs (NVIDIA/AMD/Intel/Apple/Qualcomm),
NPUs (Intel AI Boost, AMD Ryzen AI/XDNA, Qualcomm Hexagon, Apple Neural Engine) y runtimes de IA.

Cada sonda va protegida: si algo falla, se omite y se registra, nunca rompe el arranque.
"""
from __future__ import annotations

import ctypes
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

import psutil

from .. import log, paths

L = log.get("hardware")

VENDOR_IDS = {"10de": "nvidia", "1002": "amd", "1022": "amd", "8086": "intel", "5143": "qualcomm",
              "qcom": "qualcomm", "106b": "apple"}
# Patrones de NPU con limites de palabra (evita falsos positivos como "iNPUt")
NPU_PATTERN = re.compile(r"\b(NPU|AI Boost|Ryzen\s*AI|IPU|Hexagon|Neural Process\w*|XDNA)\b", re.I)


@dataclass
class GPU:
    vendor: str
    name: str
    vram_gb: float = 0.0
    integrated: bool = False
    driver: str = ""
    compute_capability: str = ""
    source: str = ""


@dataclass
class NPU:
    vendor: str
    name: str
    runtime_hint: str = ""
    source: str = ""


@dataclass
class HardwareInfo:
    os: str
    os_version: str
    arch: str
    cpu_name: str
    cores_physical: int
    cores_logical: int
    cpu_flags: list[str]
    ram_gb: float
    disk_free_gb: float
    gpus: list[GPU] = field(default_factory=list)
    npus: list[NPU] = field(default_factory=list)
    runtimes: dict = field(default_factory=dict)
    python: str = sys.version.split()[0]

    def to_dict(self) -> dict:
        return asdict(self)

    @staticmethod
    def from_dict(d: dict) -> "HardwareInfo":
        d = dict(d)
        d["gpus"] = [GPU(**g) for g in d.get("gpus", [])]
        d["npus"] = [NPU(**n) for n in d.get("npus", [])]
        return HardwareInfo(**d)

    @property
    def discrete_gpus(self) -> list[GPU]:
        return [g for g in self.gpus if not g.integrated]


def _run(cmd: list[str], timeout: float = 15) -> str:
    try:
        flags = 0x08000000 if os.name == "nt" else 0  # CREATE_NO_WINDOW
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, creationflags=flags,
                           encoding="utf-8", errors="replace")
        return r.stdout if r.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def _vendor_from_text(text: str) -> str:
    t = text.lower()
    m = re.search(r"ven_([0-9a-z]{4})", t)
    if m and m.group(1) in VENDOR_IDS:
        return VENDOR_IDS[m.group(1)]
    for key, vendor in (("nvidia", "nvidia"), ("geforce", "nvidia"), ("quadro", "nvidia"), ("radeon", "amd"),
                        ("amd", "amd"), ("intel", "intel"), ("arc", "intel"), ("adreno", "qualcomm"),
                        ("qualcomm", "qualcomm"), ("apple", "apple")):
        if key in t:
            return vendor
    return "unknown"


def _is_integrated(vendor: str, name: str) -> bool:
    n = name.lower()
    if vendor == "nvidia":
        return False
    if vendor == "intel":
        # Arc A/B (A770, B580...) son dedicadas; "Arc Graphics" de Meteor/Lunar Lake es integrada
        return not re.search(r"arc.*\b[ab]\d{3}", n)
    if vendor == "amd":
        if re.search(r"\brx\b|radeon pro|instinct|\bw\d{4}", n):
            return False
        return bool(re.search(r"radeon\(tm\) graphics|vega \d+|radeon \d{3}m|radeon graphics", n)) or "rx" not in n
    return vendor in ("qualcomm", "apple")


# ---------------------------------------------------------------------------- CPU
def _cpu_flags() -> list[str]:
    flags: list[str] = []
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return ["neon"]
    if os.name == "nt":
        try:
            fn = ctypes.windll.kernel32.IsProcessorFeaturePresent
            for name, code in (("sse2", 10), ("sse3", 13), ("avx", 39), ("avx2", 40), ("avx512f", 41)):
                if fn(code):
                    flags.append(name)
        except (AttributeError, OSError):
            pass
    elif Path("/proc/cpuinfo").exists():
        txt = Path("/proc/cpuinfo").read_text(errors="ignore")
        m = re.search(r"^flags\s*:\s*(.*)$", txt, re.M)
        have = set(m.group(1).split()) if m else set()
        flags = [f for f in ("sse2", "sse3", "avx", "avx2", "fma", "f16c", "avx512f", "avx512_vnni", "amx_tile")
                 if f in have]
    elif sys.platform == "darwin":
        out = _run(["sysctl", "-n", "machdep.cpu.features", "machdep.cpu.leaf7_features"]).lower()
        flags = [f for f in ("sse2", "sse3", "avx1.0", "avx2", "avx512f") if f in out]
    return flags


def _cpu_name() -> str:
    if sys.platform == "darwin":
        return _run(["sysctl", "-n", "machdep.cpu.brand_string"]).strip() or platform.processor()
    if Path("/proc/cpuinfo").exists():
        txt = Path("/proc/cpuinfo").read_text(errors="ignore")
        m = re.search(r"^model name\s*:\s*(.*)$", txt, re.M)
        if m:
            return m.group(1).strip()
        # ARM (Raspberry Pi, Jetson, Snapdragon...): /proc/cpuinfo no trae "model name"
        if shutil.which("lscpu"):
            m = re.search(r"^Model name:\s*(.+)$", _run(["lscpu"]), re.M)
            if m and m.group(1).strip() != "-":
                return m.group(1).strip()
        for f in ("/sys/firmware/devicetree/base/model", "/proc/device-tree/model"):
            try:
                model = Path(f).read_text(errors="ignore").strip("\x00 \n")
            except OSError:
                continue
            if model:
                return model
        m = re.search(r"^Hardware\s*:\s*(.*)$", txt, re.M)
        if m:
            return m.group(1).strip()
    return platform.processor() or platform.machine()


# ---------------------------------------------------------------------------- Windows
_WIN_PS = r"""
$v = @(Get-CimInstance Win32_VideoController | Select-Object Name,AdapterRAM,DriverVersion,PNPDeviceID)
$p = @(Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPClass -eq 'ComputeAccelerator' -or
       $_.Name -match '(?i)\b(NPU|AI Boost|Ryzen\s*AI|IPU|Hexagon|Neural Process\w*|XDNA)\b' } |
       Select-Object Name,PNPClass,PNPDeviceID,Manufacturer,Status)
$c = @(Get-CimInstance Win32_Processor | Select-Object Name)
@{video=$v; npu=$p; cpu=$c} | ConvertTo-Json -Depth 3 -Compress
"""


def _win_registry_vram() -> dict[str, float]:
    """VRAM real por adaptador (Win32_VideoController.AdapterRAM esta limitado a 4 GB)."""
    out: dict[str, float] = {}
    try:
        import winreg
        base = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as root:
            for i in range(64):
                try:
                    sub = winreg.EnumKey(root, i)
                except OSError:
                    break
                try:
                    with winreg.OpenKey(root, sub) as k:
                        desc = winreg.QueryValueEx(k, "DriverDesc")[0]
                        size = 0
                        for val in ("HardwareInformation.qwMemorySize", "HardwareInformation.MemorySize"):
                            try:
                                raw = winreg.QueryValueEx(k, val)[0]
                                size = int.from_bytes(raw[:8], "little") if isinstance(raw, bytes) else int(raw)
                                if size:
                                    break
                            except OSError:
                                continue
                        if size:
                            out[desc] = round(size / 1024**3, 2)
                except OSError:
                    continue
    except ImportError:
        pass
    return out


def _detect_windows(info: HardwareInfo) -> None:
    raw = _run(["powershell", "-NoProfile", "-NonInteractive", "-Command", _WIN_PS], timeout=40)
    try:
        data = json.loads(raw) if raw.strip() else {}
    except json.JSONDecodeError:
        data = {}
    if data.get("cpu"):
        info.cpu_name = (data["cpu"][0].get("Name") or info.cpu_name).strip()
    reg_vram = _win_registry_vram()
    for v in data.get("video", []) or []:
        name = (v.get("Name") or "").strip()
        vendor = _vendor_from_text(f"{v.get('PNPDeviceID', '')} {name}")
        if "basic display" in name.lower() or "remote" in name.lower() or vendor == "unknown":
            continue
        vram = reg_vram.get(name) or round((v.get("AdapterRAM") or 0) / 1024**3, 2)
        info.gpus.append(GPU(vendor=vendor, name=name, vram_gb=vram, integrated=_is_integrated(vendor, name),
                             driver=v.get("DriverVersion") or "", source="wmi"))
    for p in data.get("npu", []) or []:
        name = (p.get("Name") or "").strip()
        if not (p.get("PNPClass") == "ComputeAccelerator" or NPU_PATTERN.search(name)):
            continue
        vendor = _vendor_from_text(f"{p.get('PNPDeviceID', '')} {p.get('Manufacturer', '')} {name}")
        info.npus.append(NPU(vendor=vendor, name=name, runtime_hint=_npu_runtime(vendor), source="pnp"))


# ---------------------------------------------------------------------------- Linux / macOS
SYSFS = Path("/sys")
# Codigos de las APU AMD (iGPU) tal y como los nombra lspci
_AMD_APU = re.compile(r"renoir|cezanne|lucienne|barcelo|rembrandt|phoenix|hawk point|strix|krackan|raphael|"
                      r"granite ridge|mendocino|van gogh|picasso|raven|dali|pollock|carrizo|stoney|kaveri|"
                      r"radeon(\(tm\))? graphics|radeon \d{3}m|vega \d+ mobile|\bvega \d\b", re.I)
_AMD_DGPU = re.compile(r"\brx\b|radeon pro|instinct|firepro|\br[579]\b|\bw\d{4}\b|\bmi\d{2,3}\b", re.I)


def _which(tool: str, extra: tuple[str, ...] = ()) -> str | None:
    """shutil.which + rutas que no siempre estan en el PATH (/usr/sbin para usuarios normales, WSL...)."""
    exe = shutil.which(tool)
    if exe:
        return exe
    for d in ("/usr/sbin", "/sbin", "/usr/local/bin", "/usr/bin") + extra:
        cand = Path(d) / tool
        if cand.is_file() and os.access(cand, os.X_OK):
            return str(cand)
    return None


def find_nvidia_smi() -> str | None:
    if os.name == "nt":
        exe = shutil.which("nvidia-smi")
        if not exe:
            cand = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "nvidia-smi.exe"
            exe = str(cand) if cand.exists() else None
        return exe
    return _which("nvidia-smi", ("/usr/lib/wsl/lib", "/opt/nvidia/bin"))


def _lspci_names() -> dict[str, str]:
    """Nombre comercial de cada GPU por ranura PCI (0000:01:00.0 -> 'NVIDIA Corporation AD104 [RTX 4070]')."""
    exe = _which("lspci")
    names: dict[str, str] = {}
    if not exe:
        return names
    for line in _run([exe, "-mm", "-D"]).splitlines():
        parts = [p.strip('"') for p in re.findall(r'"[^"]*"|\S+', line)]
        if len(parts) >= 4 and re.search(r"VGA|3D|Display", parts[1]):
            names[parts[0].lower()] = f"{parts[2]} {parts[3]}"
    return names


def _amd_integrated(name: str, vram: float, named: bool) -> bool:
    if named and _AMD_DGPU.search(name):
        return False
    if named and _AMD_APU.search(name):
        return True
    return vram < 2.1   # sin nombre fiable: las APU reservan poca VRAM (512 MB - 2 GB)


def _largest_bar_gb(dev: Path) -> float:
    """Tamano del mayor BAR PCI: con Resizable BAR equivale a la VRAM de las Intel Arc (i915/xe no la exponen)."""
    best = 0
    try:
        for line in (dev / "resource").read_text().splitlines():
            a, b, *_ = (int(x, 16) for x in line.split())
            if b > a:
                best = max(best, b - a + 1)
    except (OSError, ValueError):
        return 0.0
    return round(best / 1024**3, 2) if best >= 1024**3 else 0.0


def linux_gpus(sys_root: Path | None = None) -> list[dict]:
    """GPUs PCI vistas por el kernel en /sys/class/drm (una por dispositivo, sin conectores ni renderD)."""
    root = sys_root or SYSFS
    names = _lspci_names()
    seen: set[str] = set()
    out: list[dict] = []
    cards = [c for c in (root / "class" / "drm").glob("card*") if re.fullmatch(r"card\d+", c.name)]
    for card in sorted(cards, key=lambda c: int(c.name[4:])):
        dev = card / "device"
        try:
            vid = (dev / "vendor").read_text().strip().lower().replace("0x", "")
        except OSError:
            continue   # simpledrm/efifb y similares no son GPUs PCI
        try:
            slot = dev.resolve().name.lower()
        except OSError:
            slot = card.name
        if slot in seen:
            continue
        seen.add(slot)
        try:
            driver = os.path.basename(os.readlink(dev / "driver"))
        except OSError:
            driver = ""
        vendor = VENDOR_IDS.get(vid, "unknown")
        if vendor == "unknown":
            continue
        vram = 0.0
        try:
            vram = round(int((dev / "mem_info_vram_total").read_text()) / 1024**3, 2)
        except (OSError, ValueError):
            pass
        out.append({"vendor": vendor, "name": names.get(slot, ""), "vram_gb": vram, "driver": driver,
                    "slot": slot, "dev": dev})
    return out


def _detect_linux(info: HardwareInfo, sys_root: Path | None = None) -> None:
    root = sys_root or SYSFS
    for g in linux_gpus(root):
        vendor, named = g["vendor"], bool(g["name"])
        name = g["name"] or f"{vendor.upper()} GPU"
        vram = g["vram_gb"]
        if vendor == "amd":
            integrated = _amd_integrated(name, vram, named)
        else:
            integrated = _is_integrated(vendor, name)
        if vendor == "intel" and not integrated and not vram:
            vram = _largest_bar_gb(g["dev"])
        # NVIDIA: aqui solo queda constancia de la tarjeta; nvidia-smi la sustituye con VRAM y driver reales.
        # Sin nvidia-smi (driver nouveau o sin instalar) la VRAM queda a 0 y el perfil no intenta usar CUDA.
        info.gpus.append(GPU(vendor=vendor, name=name, vram_gb=vram, integrated=integrated, driver=g["driver"],
                             source="sysfs"))
    for accel in sorted((root / "class" / "accel").glob("accel*")):
        drv = ""
        try:
            drv = os.path.basename(os.readlink(accel / "device" / "driver"))
        except OSError:
            pass
        vendor = {"intel_vpu": "intel", "amdxdna": "amd", "qaic": "qualcomm"}.get(drv, "unknown")
        info.npus.append(NPU(vendor=vendor, name=f"{drv or 'accel'} ({accel.name})",
                             runtime_hint=_npu_runtime(vendor), source="sysfs"))


def _detect_android(info: HardwareInfo) -> None:
    """App Android autonoma: SoC, GPU (la que llama.cpp puede usar por Vulkan/OpenCL) y NPU. Ver android_accel."""
    from . import android_accel
    acc = android_accel.detect()
    s, g, n = acc["soc"], acc["gpu"], acc["npu"]
    if s.get("name"):
        info.cpu_name = s["name"]
    info.gpus = []   # en Android /sys/class/drm no es accesible: la GPU la da la sonda
    if g.get("name"):
        use = g.get("use") or {}
        info.gpus.append(GPU(vendor=g["vendor"], name=g["name"], vram_gb=round(use.get("mem_mb", 0) / 1024, 2),
                             integrated=True, driver=g.get("gl_vendor", ""),
                             source=f"android-{use['backend']}" if use else "android-egl"))
    if n:
        info.npus.append(NPU(vendor=n["vendor"], name=n["name"], runtime_hint=n["runtime_hint"], source=n["source"]))
    info.runtimes["android_accel"] = {"gpu_backend": (g.get("use") or {}).get("backend", ""),
                                      "gpu_device": (g.get("use") or {}).get("dev", ""),
                                      "gpu_devices": g.get("devices", []), "gpu_broken": g.get("broken", ""),
                                      "gpu_note": g.get("note", ""), "gpu_binary": g.get("binary", False),
                                      "vulkan": g.get("vulkan", ""), "soc": s}


def _is_android() -> bool:
    return os.environ.get("NEXUS_ANDROID") == "1" or hasattr(sys, "getandroidapilevel")


def _detect_macos(info: HardwareInfo) -> None:
    if platform.machine() == "arm64":
        info.gpus.append(GPU(vendor="apple", name=f"{info.cpu_name} GPU", vram_gb=round(info.ram_gb * 0.7, 1),
                             integrated=True, source="apple-silicon"))
        info.npus.append(NPU(vendor="apple", name="Apple Neural Engine", runtime_hint="coreml", source="apple-silicon"))


def _parse_nvidia_smi(out: str) -> list[GPU]:
    found = []
    for line in out.strip().splitlines():
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 3 or not parts[0]:
            continue
        try:
            vram = round(float(parts[1]) / 1024, 2)
        except ValueError:   # "[N/A]" en algunas GPU/drivers
            vram = 0.0
        found.append(GPU(vendor="nvidia", name=parts[0], vram_gb=vram, driver=parts[2],
                         compute_capability=parts[3] if len(parts) > 3 else "", source="nvidia-smi"))
    return found


def _detect_nvidia(info: HardwareInfo) -> None:
    exe = find_nvidia_smi()
    if not exe:
        return
    fmt = "--format=csv,noheader,nounits"
    out = _run([exe, "--query-gpu=name,memory.total,driver_version,compute_cap", fmt])
    if not out.strip():   # drivers < 510 no conocen compute_cap y nvidia-smi falla entero
        out = _run([exe, "--query-gpu=name,memory.total,driver_version", fmt])
    found = _parse_nvidia_smi(out)
    if found:
        info.gpus = [g for g in info.gpus if g.vendor != "nvidia"] + found


def _npu_runtime(vendor: str) -> str:
    return {"intel": "openvino", "amd": "onnxruntime-vitisai (Ryzen AI SDK)", "qualcomm": "onnxruntime-qnn",
            "apple": "coreml"}.get(vendor, "")


# ---------------------------------------------------------------------------- runtimes
def _probe_runtimes(cfg_ollama_url: str = "http://127.0.0.1:11434") -> dict:
    rt: dict = {}
    has = lambda m: importlib.util.find_spec(m) is not None  # noqa: E731
    rt["llama_cpp"] = has("llama_cpp")
    if rt["llama_cpp"]:
        try:
            import llama_cpp
            rt["llama_cpp_gpu_offload"] = bool(llama_cpp.llama_supports_gpu_offload())
            rt["llama_cpp_version"] = getattr(llama_cpp, "__version__", "")
        except Exception as e:  # noqa: BLE001
            rt["llama_cpp_error"] = str(e)[:200]
    if has("torch"):
        try:
            import torch
            rt["torch"] = torch.__version__
            rt["torch_cuda"] = torch.cuda.is_available()
            rt["torch_hip"] = bool(getattr(torch.version, "hip", None))
            rt["torch_xpu"] = bool(hasattr(torch, "xpu") and torch.xpu.is_available())
            rt["torch_mps"] = bool(getattr(torch.backends, "mps", None) and torch.backends.mps.is_available())
        except Exception as e:  # noqa: BLE001
            rt["torch_error"] = str(e)[:200]
    rt["torch_directml"] = has("torch_directml")
    if has("onnxruntime"):
        try:
            import onnxruntime
            rt["onnxruntime_providers"] = onnxruntime.get_available_providers()
        except Exception:  # noqa: BLE001
            pass
    if has("openvino"):
        try:
            import openvino
            rt["openvino_devices"] = openvino.Core().available_devices
        except Exception:  # noqa: BLE001
            pass
    for mod in ("diffusers", "transformers", "faster_whisper", "pyttsx3", "edge_tts", "piper", "vosk"):
        rt[mod] = has(mod)
    try:
        import httpx
        r = httpx.get(f"{cfg_ollama_url}/api/version", timeout=1.0)
        rt["ollama"] = r.json().get("version") if r.status_code == 200 else False
    except Exception:  # noqa: BLE001
        rt["ollama"] = False
    return rt


def detect(ollama_url: str = "http://127.0.0.1:11434", probe_runtimes: bool = True) -> HardwareInfo:
    def safe(fn, default):
        try:
            return fn()
        except Exception as e:  # noqa: BLE001  (psutil en Termux/contenedores, /proc restringido...)
            L.warning("Sonda %s fallo: %s", getattr(fn, "__name__", "cpu"), e)
            return default

    try:
        disk = psutil.disk_usage(str(paths.data_dir())).free / 1024**3
    except OSError:
        disk = 0.0
    logical = safe(lambda: psutil.cpu_count(logical=True), None) or os.cpu_count() or 1
    info = HardwareInfo(
        os=platform.system(), os_version=platform.version(), arch=platform.machine(),
        cpu_name=safe(_cpu_name, platform.machine()),
        cores_physical=safe(lambda: psutil.cpu_count(logical=False), None) or logical,
        cores_logical=logical, cpu_flags=safe(_cpu_flags, []),
        ram_gb=round(safe(lambda: psutil.virtual_memory().total, 0) / 1024**3, 2), disk_free_gb=round(disk, 1),
    )
    for name, fn in (("windows", _detect_windows if os.name == "nt" else None),
                     ("linux", _detect_linux if sys.platform.startswith("linux") else None),
                     ("macos", _detect_macos if sys.platform == "darwin" else None),
                     ("android", _detect_android if _is_android() else None),
                     ("nvidia", _detect_nvidia)):
        if fn is None:
            continue
        try:
            fn(info)
        except Exception as e:  # noqa: BLE001
            L.warning("Sonda %s fallo: %s", name, e)
    if probe_runtimes:
        info.runtimes = {**_probe_runtimes(ollama_url), **info.runtimes}
        # OpenVINO puede ver una NPU Intel que la enumeracion PnP no nombra
        if "NPU" in info.runtimes.get("openvino_devices", []) and not any(n.vendor == "intel" for n in info.npus):
            info.npus.append(NPU(vendor="intel", name="Intel NPU (OpenVINO)", runtime_hint="openvino", source="openvino"))
    return info


def save(info: HardwareInfo) -> None:
    paths.hardware_file().write_text(json.dumps(info.to_dict(), indent=2, ensure_ascii=False), encoding="utf-8")


def load_cached() -> HardwareInfo | None:
    f = paths.hardware_file()
    if f.exists():
        try:
            return HardwareInfo.from_dict(json.loads(f.read_text(encoding="utf-8")))
        except (json.JSONDecodeError, TypeError):
            return None
    return None
