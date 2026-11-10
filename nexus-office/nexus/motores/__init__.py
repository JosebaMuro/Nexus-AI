"""Motores de inferencia por hardware (Agente IA 4.0, edicion Python).

Cada fabricante acelera mejor con su propio motor. Este paquete los reune detras de una interfaz comun y elige el
mas rapido para el equipo que ejecuta Agente IA:

=================  ==========================  ===========================================  ======================
motor              paquete / biblioteca        aceleradores                                 formato de modelo
=================  ==========================  ===========================================  ======================
``onnx``           onnxruntime-genai           CUDA, DirectML, QNN (NPU Snapdragon X),      carpeta con
                                               OpenVINO, VitisAI (NPU Ryzen AI), CPU        ``genai_config.json``
``openvino``       openvino-genai              NPU Intel (Core Ultra), iGPU/Arc, CPU        ``openvino_model.xml``
``litert``         liblitert-lm (API C)        NPU Tensor/Snapdragon/MediaTek/Intel, GPU,   ``.litertlm``
                                               CPU (XNNPACK)
``executorch``     executorch                  XNNPACK (CPU), QNN, Vulkan, CoreML, MPS      ``.pte`` + tokenizer
``mlc``            mlc-llm                     CUDA, ROCm, Vulkan, Metal, OpenCL            carpeta con
                                                                                            ``mlc-chat-config.json``
``tensorrt``       tensorrt-llm                NVIDIA (Tensor Cores)                        checkpoint de HF o
                                                                                            motor TensorRT
``genie``          libGenie (API C, QAIRT)     NPU Hexagon (HTP)                            ``genie_config.json``
``llamaserver``    llama-server (llama.cpp)    CPU, CUDA, ROCm, Vulkan, Metal, SYCL...      ``.gguf`` (respaldo)
=================  ==========================  ===========================================  ======================

Ningun motor es obligatorio: si su paquete o biblioteca no esta, se dice por que y se usa el siguiente. llama.cpp
queda siempre como respaldo. Ver ``chips.py`` (que motor va con cada chip), ``selector.py`` (eleccion) y
``banco.py`` (medir tokens/s de cada motor en este equipo y quedarse con el mas rapido).
"""
from __future__ import annotations

import importlib.util
import json
import os
import platform
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator

from .. import log, paths

L = log.get("motores")


@dataclass(frozen=True)
class Info:
    id: str
    nombre: str
    formato: str            # formato de modelo del catalogo
    paquete: str = ""       # modulo de Python (si lo hay)
    lib: str = ""           # biblioteca nativa cargada con ctypes (si la hay)
    aceleradores: tuple = ()


MOTORES: dict[str, Info] = {i.id: i for i in (
    Info("onnx", "ONNX Runtime GenAI", "onnx-genai", paquete="onnxruntime_genai",
         aceleradores=("modelo", "cuda", "dml", "qnn", "openvino", "vitisai", "cpu")),
    Info("openvino", "OpenVINO GenAI", "openvino", paquete="openvino_genai", aceleradores=("NPU", "GPU", "CPU")),
    Info("litert", "LiteRT-LM", "litertlm", lib="litert_lm_c", aceleradores=("npu", "gpu", "cpu")),
    Info("executorch", "ExecuTorch", "executorch", paquete="executorch", aceleradores=("xnnpack", "qnn", "vulkan", "coreml", "mps")),
    Info("mlc", "MLC LLM", "mlc", paquete="mlc_llm", aceleradores=("cuda", "rocm", "vulkan", "metal", "opencl", "cpu")),
    Info("tensorrt", "TensorRT-LLM", "tensorrt", paquete="tensorrt_llm", aceleradores=("cuda",)),
    Info("genie", "Qualcomm Genie (QNN)", "genie", lib="Genie", aceleradores=("npu",)),
)}

FORMATO_MOTOR = {i.formato: i.id for i in MOTORES.values()}
FORMATO_MOTOR["gguf"] = "llamaserver"


def motor_de_formato(formato: str) -> str:
    return FORMATO_MOTOR.get(formato or "", "")


# ---------------------------------------------------------------------------------------------- formatos de modelo
def detectar_formato(path: Path) -> dict | None:
    """Formato de un fichero o carpeta de modelo de alguno de los motores; None si no es de ninguno."""
    p = Path(path)
    if p.is_file():
        low = p.name.lower()
        if low.endswith(".litertlm"):
            return {"format": "litertlm", "kind": "llm"}
        if low.endswith(".pte"):
            return {"format": "executorch", "kind": "llm"}
        if low == "genai_config.json":
            return {"format": "onnx-genai", "kind": "llm"}
        if low == "genie_config.json":
            return {"format": "genie", "kind": "llm"}
        if low == "mlc-chat-config.json":
            return {"format": "mlc", "kind": "llm"}
        return None
    if not p.is_dir():
        return None
    if (p / "genai_config.json").exists():
        return {"format": "onnx-genai", "kind": "llm"}
    if (p / "genie_config.json").exists():
        return {"format": "genie", "kind": "llm"}
    if (p / "mlc-chat-config.json").exists():
        return {"format": "mlc", "kind": "llm"}
    if (p / "openvino_model.xml").exists() and ((p / "openvino_tokenizer.xml").exists() or (p / "tokenizer.json").exists()):
        return {"format": "openvino", "kind": "llm"}
    if list(p.glob("*.litertlm")):
        return {"format": "litertlm", "kind": "llm"}
    if list(p.glob("*.pte")):
        return {"format": "executorch", "kind": "llm"}
    # motor de TensorRT-LLM ya compilado (rank0.engine) o checkpoint de Hugging Face marcado para TensorRT
    if list(p.glob("rank*.engine")) or (p / "trtllm.json").exists():
        return {"format": "tensorrt", "kind": "llm"}
    return None


def ruta_modelo(entry: dict) -> Path:
    """Carpeta (o fichero) que se le pasa al motor."""
    p = Path(entry.get("path", ""))
    fmt = entry.get("format", "")
    if p.is_file() and fmt in ("onnx-genai", "genie", "mlc"):
        return p.parent
    if p.is_dir() and fmt in ("litertlm", "executorch"):
        hits = sorted(p.glob("*.litertlm" if fmt == "litertlm" else "*.pte"))
        if hits:
            return hits[0]
    return p


# ---------------------------------------------------------------------------------------------- runtimes nativos
BUNDLED = paths.ROOT / "bundled" / "runtimes"


def _ext() -> tuple[str, str]:
    if sys.platform == "win32":
        return "", ".dll"
    if sys.platform == "darwin":
        return "lib", ".dylib"
    return "lib", ".so"


def lib_path(motor: str, variante: str = "") -> Path | None:
    """Biblioteca nativa de un motor que se carga con ctypes (LiteRT-LM, Genie).

    Orden: NEXUS_RUNTIME_<MOTOR> (fichero o carpeta), data/runtimes/<motor>/<variante>, bundled/runtimes/<motor>/...,
    y en Android la carpeta de bibliotecas de la app (NEXUS_NATIVE_LIB_DIR)."""
    info = MOTORES.get(motor)
    if not info or not info.lib:
        return None
    pre, ext = _ext()
    nombre = f"{pre}{info.lib}{ext}"
    forced = os.environ.get(f"NEXUS_RUNTIME_{motor.upper()}", "")
    cands: list[Path] = []
    if forced:
        f = Path(forced)
        cands.append(f if f.is_file() else f / nombre)
    for base in (paths.sub("runtimes") / motor, BUNDLED / motor):
        if variante:
            cands.append(base / variante / nombre)
        cands.append(base / nombre)
        if base.is_dir():
            cands += sorted(base.glob(f"*/{nombre}"))
    nat = os.environ.get("NEXUS_NATIVE_LIB_DIR", "")
    if nat:
        cands.append(Path(nat) / nombre)
    return next((c for c in cands if c.is_file()), None)


def variantes(motor: str) -> list[str]:
    out: list[str] = []
    for base in (paths.sub("runtimes") / motor, BUNDLED / motor):
        if base.is_dir():
            out += [d.name for d in sorted(base.iterdir()) if d.is_dir() and d.name not in out]
    return out


def _modulo(nombre: str) -> bool:
    try:
        return importlib.util.find_spec(nombre) is not None
    except (ImportError, ValueError):
        return False


def disponible(motor: str) -> tuple[bool, str]:
    """(instalado, motivo si no). Solo mira si esta el paquete o la biblioteca, no carga nada."""
    if motor == "llamaserver":
        return True, ""
    info = MOTORES.get(motor)
    if not info:
        return False, f"motor desconocido: {motor}"
    if info.paquete:
        if _modulo(info.paquete):
            return True, ""
        return False, f"falta el paquete de Python {info.paquete.replace('_', '-')} (va en el paquete offline de tu plataforma)"
    if lib_path(motor):
        return True, ""
    return False, f"falta la biblioteca {info.lib} (Sistema > Runtimes o bundled/runtimes/{motor})"


# ---------------------------------------------------------------------------------------------- interfaz comun
@dataclass
class Params:
    max_tokens: int = 1024
    temperature: float = 0.7
    top_p: float = 0.95
    top_k: int = 40
    seed: int = -1
    n_ctx: int = 4096
    hilos: int = 0


class Motor:
    """Un modelo cargado en un motor con un acelerador. ``generar`` va dando trozos de texto."""
    id = ""

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        self.modelo, self.acelerador, self.params = Path(modelo), acelerador, params

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        raise NotImplementedError

    def cerrar(self) -> None:
        pass


def mensajes_simples(messages: list[dict]) -> list[dict]:
    """Mensajes de Agente IA (role/content/images) a texto por turno, uniendo los seguidos del mismo papel."""
    out: list[dict] = []
    for m in messages:
        role = m.get("role", "user")
        if role not in ("system", "user", "assistant"):
            role = "user"   # resultados de herramientas: se le dan como texto del usuario
        content = m.get("content") or ""
        if isinstance(content, list):
            content = "".join(c.get("text", "") for c in content if isinstance(c, dict))
        if out and out[-1]["role"] == role and role != "system":
            out[-1]["content"] += "\n\n" + content
        else:
            out.append({"role": role, "content": content})
    return out


def abrir(motor: str, modelo: Path, acelerador: str, params: Params) -> Motor:
    """Carga un modelo en un motor. RuntimeError con el motivo si no se puede."""
    if motor == "onnx":
        from .onnx_genai import OnnxGenAI as C
    elif motor == "openvino":
        from .openvino_genai import OpenVinoGenAI as C
    elif motor == "litert":
        from .litert import LiteRtLm as C
    elif motor == "executorch":
        from .executorch import ExecuTorch as C
    elif motor == "mlc":
        from .mlc import Mlc as C
    elif motor == "tensorrt":
        from .tensorrt import TensorRtLlm as C
    elif motor == "genie":
        from .genie import Genie as C
    else:
        raise RuntimeError(f"motor desconocido: {motor}")
    return C(modelo, acelerador, params)


# ---------------------------------------------------------------------------------------------- caidas
def _caidas_file() -> Path:
    return paths.sub("motores") / "caidas.json"


def caidas() -> dict:
    """Motores y aceleradores que tumbaron el proceso al cargar o generar (driver de NPU/GPU): no se vuelven a
    probar hasta que se olvidan en Sistema."""
    try:
        return json.loads(_caidas_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def olvidar_caidas() -> None:
    try:
        _caidas_file().unlink()
    except OSError:
        pass


class Marca:
    """Marca en disco mientras un motor carga o genera. Si Agente IA se cae con la marca puesta, al volver a arrancar
    esa combinacion se apunta como caida y se pasa a la siguiente (NPU -> GPU -> CPU)."""

    def __init__(self, motor: str, acelerador: str) -> None:
        self.f = paths.sub("motores") / f"en_curso-{motor}-{acelerador}.marca"

    def __enter__(self):
        try:
            self.f.parent.mkdir(parents=True, exist_ok=True)
            self.f.write_text(str(time.time()), encoding="utf-8")
        except OSError:
            pass
        return self

    def __exit__(self, *exc) -> None:
        try:
            self.f.unlink()
        except OSError:
            pass


def revisar_marcas() -> list[str]:
    """Al arrancar: las marcas que quedaron son caidas del arranque anterior."""
    d = paths.sub("motores")
    nuevas = []
    if not d.is_dir():
        return nuevas
    data = caidas()
    for f in d.glob("en_curso-*.marca"):
        clave = f.stem[len("en_curso-"):]
        data[clave] = {"cuando": time.time(), "motivo": "el proceso se cerro mientras el motor trabajaba (driver)"}
        nuevas.append(clave)
        try:
            f.unlink()
        except OSError:
            pass
    if nuevas:
        _caidas_file().write_text(json.dumps(data, indent=1), encoding="utf-8")
        L.warning("Motores que se cayeron en el arranque anterior (no se usan hasta olvidarlos): %s", ", ".join(nuevas))
    return nuevas


def plataforma() -> str:
    m = platform.machine().lower()
    arch = "arm64" if m in ("arm64", "aarch64") else "x86_64" if m in ("x86_64", "amd64") else m
    if os.environ.get("ANDROID_ROOT") or hasattr(sys, "getandroidapilevel"):
        return f"android-{arch}"
    so = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    return f"{so}-{arch}"
