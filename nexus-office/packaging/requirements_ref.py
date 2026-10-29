"""Genera requirements.txt: dependencias pip + referencias del software nativo y de los pesos de IA.

    venv/python.exe packaging/requirements_ref.py          # reescribe requirements.txt
    venv/python.exe packaging/requirements_ref.py --check  # falla si requirements.txt esta desactualizado

Fuente unica de verdad: el catalogo (nexus/models/recommended.py), la seleccion de perfil
(nexus/hardware/profiles.py) y de variante de runtime (nexus/runtimes.py), y las versiones probadas
que hay instaladas en este equipo (venv/, .cache/, data/runtimes/). Solo las lineas sin '#' las lee pip.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from importlib import metadata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from nexus import __version__  # noqa: E402
from nexus.hardware.detect import GPU, HardwareInfo  # noqa: E402
from nexus.hardware.profiles import build_profile  # noqa: E402
from nexus.models.recommended import RECOMMENDED, STT_MODELS, recommend  # noqa: E402
from nexus.runtimes import RUNTIMES, candidates  # noqa: E402

# ---------------------------------------------------------------- dependencias pip (minimo compatible)
BASE = [("fastapi", "0.110"), ("uvicorn", "0.29"), ("websockets", "12"), ("pydantic", "2.5"), ("httpx", "0.27"), ("anthropic", "1.0"),
        ("psutil", "5.9"), ("numpy", "1.26"), ("pyyaml", "6.0"), ("pillow", "10.0"), ("python-multipart", "0.0.9"),
        ("cryptography", "42"), ("fpdf2", "2.7"), ("pypdf", "4.0"), ("pyserial", "3.5"), ("esptool", "4.7"),
        ("pytest", "8.0")]
EXTRAS = [("piper-tts", "voz neuronal offline (Piper)"), ("faster-whisper", "voz a texto (Whisper sobre CTranslate2)"),
          ("pyttsx3", "voz del sistema de respaldo (SAPI / espeak)"), ("openvino", "NPU / iGPU Intel"),
          ("onnxruntime", "NPU AMD (Ryzen AI) y modelos ONNX"), ("onnxruntime-qnn", "NPU Qualcomm (Snapdragon X)"),
          ("torch", "opcional: musica MusicGen en GPU (+ transformers, scipy)"), ("pyinstaller", "solo para crear el setup .exe")]
PBS_TARGETS = ["x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc", "x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu",
               "aarch64-apple-darwin", "x86_64-apple-darwin"]
PBS_FALLBACK = ("3.11.17", "20261009")

# ---------------------------------------------------------------- perfiles de hardware de referencia
def _hw(ram, cores, gpus=(), arch="AMD64", flags=("avx2",)):
    return HardwareInfo(os="Windows", os_version="", arch=arch, cpu_name="CPU", cores_physical=cores, cores_logical=cores * 2,
                        cpu_flags=list(flags), ram_gb=ram, disk_free_gb=100, gpus=list(gpus))


PROFILES = [
    ("CPU basica · 2 nucleos · 8 GB RAM (portatil sencillo, iGPU Intel UHD)", _hw(7.8, 2, [GPU("intel", "Intel UHD", 0, True)]), None),
    ("CPU antigua sin AVX2 · 4 nucleos · 8 GB", _hw(8, 4, flags=()), None),
    ("CPU media · 4 nucleos · 16 GB (iGPU Iris Xe / Radeon)", _hw(16, 4, [GPU("intel", "Intel Iris Xe", 0, True)]), None),
    ("CPU potente · 8+ nucleos · 32 GB, sin GPU dedicada", _hw(32, 12), None),
    ("NVIDIA 6 GB VRAM · 16 GB RAM (RTX 3050/3060 portatil)", _hw(16, 6, [GPU("nvidia", "RTX 3060 Laptop", 6)]), None),
    ("NVIDIA 12 GB VRAM · 32 GB RAM (RTX 3060 12G / 4070)", _hw(32, 8, [GPU("nvidia", "RTX 4070", 12)]), None),
    ("NVIDIA 24 GB VRAM · 64 GB RAM (RTX 3090 / 4090)", _hw(64, 16, [GPU("nvidia", "RTX 4090", 24)]), None),
    ("AMD Radeon dedicada 8 GB · 16 GB RAM", _hw(16, 6, [GPU("amd", "Radeon RX 7600", 8)]), None),
    ("Intel Arc dedicada 8 GB · 16 GB RAM", _hw(16, 6, [GPU("intel", "Arc A750", 8)]), None),
    ("Apple Silicon M1-M4 · 16 GB memoria unificada", _hw(16, 8, [GPU("apple", "Apple M2", 11.2, True)], arch="arm64"), ["metal"]),
    ("Android ARM64 (Termux) · 8 GB · o Raspberry Pi 5", _hw(8, 4, arch="aarch64", flags=()), ["cpu"]),
]


def _tested(pkg: str) -> str:
    try:
        return metadata.version(pkg)
    except metadata.PackageNotFoundError:
        return ""


def _pbs() -> tuple[str, str]:
    for f in sorted((ROOT / ".cache").glob("cpython-3.11.*+*-install_only*.tar.gz")):
        m = re.match(r"cpython-(3\.11\.\d+)\+(\d+)-", f.name)
        if m:
            return m.group(1), m.group(2)
    return PBS_FALLBACK


def _runtime_tag(rt: str) -> str:
    for f in (ROOT / "data" / "runtimes" / rt).glob("*/nexus_runtime.json"):
        try:
            return json.loads(f.read_text(encoding="utf-8")).get("tag", "")
        except (OSError, ValueError):
            pass
    return ""


def _cat_line(r: dict) -> str:
    files = r["file"] + (f" + {r['mmproj']}" if r.get("mmproj") else "") + "".join(f" + {Path(e).name}" for e in r.get("extra", []))
    return f"#   {r['id']:<16} {r['size_mb']:>5} MB  https://huggingface.co/{r['repo']}  ->  {files}"


def render() -> str:
    pyv, pbs_tag = _pbs()
    L = [f"# Nexus-Office {__version__} - dependencias y referencias",
         "# Generado por packaging/requirements_ref.py (no editar a mano: regeneralo tras cambiar dependencias o el catalogo).",
         "# pip solo lee las lineas sin '#'. El resto es la referencia del software nativo y de los pesos de IA.",
         "#",
         "# ============================================================================ 1. Paquetes Python (pip)",
         "# Minimo compatible; al lado, la version con la que se ha probado esta version de Nexus-Office."]
    for pkg, low in BASE:
        t = _tested(pkg)
        L.append(f"{pkg + '>=' + low:<28}# probado {t}" if t else f"{pkg}>={low}")
    L += ["#", "# Extras por hardware: los instala `python -m nexus setup --auto` segun el perfil detectado",
          "# (nexus/setup_extras.py). No van aqui para no forzar paquetes pesados donde no hacen falta.",
          ]
    for pkg, why in EXTRAS:
        t = _tested(pkg)
        L.append(f"#   {pkg:<18}{'probado ' + t if t else '':<17}{why}")
    L += ["#", "# ============================================================================ 2. Software nativo (no pip)",
          "# Lo descargan los instaladores (install.ps1 / install.sh / install-termux.sh) y `nexus setup --auto`;",
          "# las versiones offline de los paquetes ya lo incluyen.",
          "#",
          f"# Python portable: python-build-standalone (astral-sh), CPython {pyv}, build {pbs_tag}, variante install_only_stripped",
          f"#   https://github.com/astral-sh/python-build-standalone/releases/tag/{pbs_tag}"]
    for t in PBS_TARGETS:
        L.append(f"#   cpython-{pyv}+{pbs_tag}-{t}-install_only_stripped.tar.gz")
    L += ["#   Android: el Python de Termux (pkg install python) · Docker: imagen python:3.11-slim", "#"]
    for rt, info in RUNTIMES.items():
        tag = _runtime_tag(rt)
        L.append(f"# {info['desc']}: https://github.com/{info['repo']}/releases"
                 + (f"  (probado {tag}; Nexus-Office usa la release mas reciente con binarios)" if tag else ""))
    L += ["#   Variantes de binario: cpu, cuda (12.x, con cudart), vulkan, rocm, sycl, openvino, metal (macOS).",
          "#   Termux: pkg install llama-cpp (CPU ARM64).",
          "#   Archivos RAR y 7z en el RAG (ZIP y TAR no necesitan nada): unrar, 7-Zip (7z/7zz), bsdtar o unar.",
          "#     Windows: 7-Zip o WinRAR (o el tar del sistema) · Ubuntu/WSL: sudo apt install unrar 7zip · macOS: brew install sevenzip",
          "#     Termux: pkg install unrar p7zip · el paquete 7zip de Debian/Ubuntu no lee RAR: para RAR, unrar o unar.",
          "#   VPN para la app Nexus-Office VPN: WireGuard (Windows: winget install WireGuard.WireGuard · Linux/WSL: wireguard-tools",
          "#     · macOS: brew install wireguard-tools o la app WireGuard). Ver docs/VPN.md.",
          "#",
          "# Herramientas de compilacion (solo para generar paquetes):",
          "#   APK Nexus-Office VPN (packaging/build_apk.py): JDK 17, Android build-tools 34.0.0 y platform android-34 en .cache/android/",
          "#     (o en Ubuntu/WSL: aapt zipalign apksigner dalvik-exchange); WireGuard tunnel 1.0.20230706 (Maven Central)",
          "#   APK Nexus-Office autonomo (packaging/build_android_runtime.sh + build_apk_autonomo.py): zig 0.17.0 (PyPI), CPython 3.12.3,",
          "#     musl 1.2.5, OpenSSL 3.5.5, SQLite 3.46.1, llama.cpp de llama-cpp-python 0.3.36, ruedas musllinux_aarch64 de PyPI",
          "#   Setup .exe: pyinstaller (ver arriba) · .deb y .run: los genera packaging/build.py sin herramientas externas",
          "#",
          "# ============================================================================ 3. Pesos de IA (referencia)",
          "# No se incluyen en los paquetes (varios GB y licencias propias). Se descargan desde la pestana Modelos",
          "# o con `python -m nexus pull auto` (elige los adecuados al perfil detectado).",
          "#",
          "# Catalogo recomendado (id, tamano, repositorio Hugging Face, ficheros):"]
    for kind, title in (("llm", "LLM multimodal (texto + vision), formato GGUF para llama.cpp"),
                        ("image", "Imagen, GGUF para stable-diffusion.cpp"), ("tts", "Voz (texto a voz)"),
                        ("embed", "Embeddings (memoria, biblioteca y organizacion del conocimiento), GGUF en CPU")):
        L.append(f"#  {title}")
        L += [_cat_line(r) for r in RECOMMENDED if r["kind"] == kind]
    L.append("#  Voz a texto: faster-whisper " + ", ".join(f"{k} (~{v} MB)" for k, v in STT_MODELS.items())
             + "; se descarga al primer uso")
    L += ["#", "# Pesos sugeridos por perfil de hardware (calculado con la logica real de Nexus-Office).",
          "# Primero el recomendado; entre parentesis las alternativas que tambien caben.", "#"]
    for name, hw, forced in PROFILES:
        p = build_profile(hw)
        recs = recommend(p)
        variant = (forced or candidates(p, "llama"))[0]
        L.append(f"# [{name}]")
        L.append(f"#   perfil {p['tier']} · acelerador {p['accelerator']} · llama.cpp '{variant}' · LLM hasta {p['llm']['max_model_b']}B, "
                 f"ideal {p['llm']['recommended_model_b']}B {p['llm']['quant']} · contexto {p['llm']['n_ctx']}")
        for kind, label in (("llm", "LLM"), ("image", "imagen"), ("tts", "voz"), ("embed", "embed")):
            ok = [r for r in recs if r["kind"] == kind and r["fit"] in ("ideal", "ok")]
            best = next((r for r in ok if r.get("best")), ok[0] if ok else None)
            alt = [r["id"] for r in ok if r is not best]
            L.append(f"#   {label:<7}{best['id'] if best else '(usa la IA de otro nodo de la red)'}" + (f"  ({', '.join(alt)})" if alt else ""))
        L.append(f"#   STT    whisper {p['stt']['model']} {p['stt']['compute_type']} en {p['stt']['device']}"
                 + (f" · imagen en GPU (sugerido {p['image']['suggested_model']})" if p["image"]["neural"] else ""))
    return "\n".join(L) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    out, text = ROOT / "requirements.txt", render()
    if a.check:
        same = out.read_text(encoding="utf-8") == text
        print("requirements.txt al dia" if same else "requirements.txt desactualizado: ejecuta packaging/requirements_ref.py")
        return 0 if same else 1
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"escrito {out} ({len(text.splitlines())} lineas)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
