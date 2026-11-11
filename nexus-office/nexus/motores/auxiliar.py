"""Motores nativos en un proceso auxiliar (app Android).

El Python de la APK es de musl y no puede cargar con ctypes las bibliotecas de Android (bionic) de LiteRT-LM, Qualcomm
Genie (QNN) ni ONNX Runtime GenAI. Por eso en el movil cada uno corre en su propio proceso nativo:

* ``libnexusmotor.so`` (el ejecutable de la edicion C++ para Android, orden ``motor``) carga LiteRT-LM, Genie u ONNX
  Runtime GenAI con dlopen y sirve el modelo con la API de OpenAI en 127.0.0.1 (``/health``,
  ``/v1/chat/completions`` con SSE).
* ExecuTorch solo tiene API Java (``LlmModule``): lo sirve la propia app en su puente (``NEXUS_PUENTE_URL``), ver
  ``android-autonomo/src/.../MotorExecuTorch.java``.

Las bibliotecas van todas en la carpeta de bibliotecas nativas de la APK (``NEXUS_NATIVE_LIB_DIR``): Android solo
deja ejecutar codigo nativo desde ahi. La NPU Hexagon busca sus programas (skel) en ``ADSP_LIBRARY_PATH``.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import time
from pathlib import Path
from typing import Callable, Iterator

from .. import log, paths
from . import Motor, Params, mensajes_simples

L = log.get("motores")

AYUDANTE = "libnexusmotor.so"
# biblioteca de cada motor dentro de la APK
LIBS = {"litert": "liblitert-lm.so", "genie": "libGenie.so", "onnx": "libonnxruntime-genai.so"}
# lo que necesita cada motor ademas de su biblioteca para poder cargar algo
EXTRA = {"genie": ("libQnnHtp.so", "libQnnSystem.so"), "onnx": ("libonnxruntime.so",), "litert": ()}
DSP_DIRS = ("/vendor/dsp/cdsp", "/vendor/lib/rfsa/adsp", "/system/lib/rfsa/adsp", "/dsp")


def android() -> bool:
    return bool(os.environ.get("NEXUS_ANDROID") or os.environ.get("ANDROID_ROOT"))


def libs_dir() -> Path | None:
    d = os.environ.get("NEXUS_NATIVE_LIB_DIR", "")
    return Path(d) if d and Path(d).is_dir() else None


def disponible(motor: str) -> tuple[bool, str]:
    """En Android: si la APK trae el proceso auxiliar y las bibliotecas del motor."""
    if motor == "executorch":
        if not os.environ.get("NEXUS_PUENTE_URL"):
            return False, "esta APK no trae el puente de ExecuTorch (actualiza Agente IA)"
        d = libs_dir()
        if d and not (d / "libexecutorch.so").is_file():
            return False, "falta libexecutorch.so en la APK"
        return True, ""
    if motor not in LIBS:
        return False, "no disponible en Android"
    d = libs_dir()
    if not d:
        return False, "no se encuentra la carpeta de bibliotecas de la app"
    faltan = [n for n in (AYUDANTE, LIBS[motor], *EXTRA[motor]) if not (d / n).is_file()]
    if faltan:
        return False, "esta APK no trae " + ", ".join(faltan)
    return True, ""


def _puerto_libre() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def entorno(libs: Path) -> dict:
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)   # el cargador de musl y el manejador de SIGSYS son del Python, no del motor
    extra = [d for d in ("/system/lib64", "/vendor/lib64", "/system/vendor/lib64") if os.path.isdir(d)]
    env["LD_LIBRARY_PATH"] = ":".join([str(libs), *extra])
    env["ADSP_LIBRARY_PATH"] = ";".join([str(libs), *DSP_DIRS])
    if (libs / "libonnxruntime.so").is_file():
        env["ORT_LIB_PATH"] = str(libs / "libonnxruntime.so")
    return env


def _sse(lineas: Iterator[str]) -> Iterator[str]:
    """Texto de los trozos ``data: {...}`` de /v1/chat/completions."""
    for linea in lineas:
        linea = linea.strip()
        if not linea.startswith("data:"):
            continue
        dato = linea[5:].strip()
        if dato == "[DONE]":
            return
        try:
            j = json.loads(dato)
        except ValueError:
            continue
        if isinstance(j, dict) and j.get("error"):
            err = j["error"]
            raise RuntimeError(err.get("message", str(err)) if isinstance(err, dict) else str(err))
        for c in (j.get("choices") or []) if isinstance(j, dict) else []:
            t = (c.get("delta") or {}).get("content")
            if t:
                yield t


class Auxiliar(Motor):
    """Un modelo cargado en el proceso auxiliar nativo. Si el proceso no llega a estar listo, RuntimeError con las
    ultimas lineas de su registro (el gestor prueba entonces el siguiente acelerador)."""

    ESPERA = 600.0   # los modelos de NPU grandes tardan en preparar la primera vez

    def __init__(self, motor: str, modelo: Path, acelerador: str, params: Params, libs: Path | None = None) -> None:
        super().__init__(modelo, acelerador or ("npu" if motor == "genie" else "cpu"), params)
        self.id = motor
        self.libs = libs or libs_dir()
        if not self.libs:
            raise RuntimeError("no se encuentra la carpeta de bibliotecas de la app")
        ok, motivo = disponible(motor) if libs is None else (True, "")
        if not ok:
            raise RuntimeError(motivo)
        self.puerto = _puerto_libre()
        cache = paths.sub("cache") / "motores" / motor
        cache.mkdir(parents=True, exist_ok=True)
        self.registro = paths.sub("logs") / f"motor-{motor}.log"
        self.registro.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(self.libs / AYUDANTE), "motor", motor, "--modelo", str(self.modelo), "--puerto", str(self.puerto),
               "--acelerador", self.acelerador, "--lib", str(self.libs / LIBS[motor]), "--ctx", str(params.n_ctx),
               "--cache", str(cache), "--dispatch", str(self.libs)]
        if params.hilos > 0:
            cmd += ["--hilos", str(params.hilos)]
        env = entorno(self.libs)
        env.setdefault("NEXUS_DATA_DIR", str(paths.sub("motores")))
        L.info("Arrancando %s en el proceso auxiliar [%s]: %s", motor, self.acelerador, self.modelo)
        with open(self.registro, "wb") as out:
            self.proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env, cwd=str(self.libs))
        self.base = f"http://127.0.0.1:{self.puerto}"
        self._esperar()

    def _cola_registro(self, n: int = 600) -> str:
        try:
            return self.registro.read_bytes()[-n:].decode(errors="replace").strip()
        except OSError:
            return ""

    def _esperar(self) -> None:
        import httpx   # noqa: PLC0415
        t0 = time.time()
        while time.time() - t0 < self.ESPERA:
            if self.proc.poll() is not None:
                break
            try:
                if httpx.get(self.base + "/health", timeout=1.0).status_code == 200:
                    return
            except httpx.HTTPError:
                pass
            time.sleep(0.3)
        cola = self._cola_registro()
        self.cerrar()
        raise RuntimeError(f"el proceso de {self.id} no arranco en {self.acelerador}: {cola or 'sin registro'}")

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        import httpx   # noqa: PLC0415
        body = {"messages": mensajes_simples(messages), "stream": True, "max_tokens": max(16, p.max_tokens),
                "temperature": p.temperature, "top_p": p.top_p, "top_k": p.top_k, "seed": p.seed}
        try:
            with httpx.stream("POST", self.base + "/v1/chat/completions", json=body,
                              timeout=httpx.Timeout(10.0, read=None)) as r:
                if r.status_code != 200:
                    r.read()
                    raise RuntimeError(f"{self.id}: {r.text[:300]}")
                for t in _sse(r.iter_lines()):
                    yield t
                    if parar():
                        break   # al cerrar la conexion el proceso auxiliar deja de generar
        except httpx.HTTPError as e:
            if self.proc.poll() is not None:
                raise RuntimeError(f"el proceso de {self.id} se cerro: {self._cola_registro(300)}") from e
            raise RuntimeError(f"{self.id}: {e}") from e

    def cerrar(self) -> None:
        p = getattr(self, "proc", None)
        if p is not None and p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()


def _clave() -> dict:
    return {"X-Nexus-Clave": os.environ.get("NEXUS_PUENTE_CLAVE", "")}


class PuenteJava(Motor):
    """ExecuTorch en Android por el puente de la app (Java, ``LlmModule``): la plantilla de chat se aplica aqui."""
    id = "executorch"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        import httpx   # noqa: PLC0415
        from . import plantillas   # noqa: PLC0415
        from .executorch import _tokenizer   # noqa: PLC0415
        super().__init__(modelo, acelerador or "xnnpack", params)
        self.base = os.environ.get("NEXUS_PUENTE_URL", "").rstrip("/")
        if not self.base:
            raise RuntimeError("esta APK no trae el puente de ExecuTorch")
        pte = self.modelo if self.modelo.is_file() else next(iter(sorted(self.modelo.glob("*.pte"))), None)
        if not pte:
            raise RuntimeError(f"no hay ningun .pte en {self.modelo}")
        self.carpeta = pte.parent
        tok = _tokenizer(self.carpeta)
        if not tok:
            raise RuntimeError(f"falta el tokenizer (tokenizer.json o tokenizer.model) junto a {pte.name}")
        self.plantillas = plantillas
        r = httpx.post(self.base + "/executorch/cargar", json={"modelo": str(pte), "tokenizer": str(tok),
                                                              "temperatura": 0.7}, headers=_clave(), timeout=600)
        if r.status_code != 200:
            raise RuntimeError(f"ExecuTorch no pudo cargar {pte.name}: {r.text[:300]}")

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        import httpx   # noqa: PLC0415
        prompt = self.plantillas.prompt(self.carpeta, mensajes_simples(messages))
        body = {"prompt": prompt, "max_tokens": max(16, p.max_tokens), "temperatura": p.temperature,
                "ctx": p.n_ctx}
        with httpx.stream("POST", self.base + "/executorch/generar", json=body, headers=_clave(),
                          timeout=httpx.Timeout(10.0, read=None)) as r:
            if r.status_code != 200:
                r.read()
                raise RuntimeError(f"ExecuTorch: {r.text[:300]}")
            for t in r.iter_text():
                if t:
                    yield self.plantillas.limpiar(t)
                if parar():
                    try:
                        httpx.post(self.base + "/executorch/parar", headers=_clave(), timeout=5)
                    except httpx.HTTPError:
                        pass
                    break

    def cerrar(self) -> None:
        import httpx   # noqa: PLC0415
        try:
            httpx.post(self.base + "/executorch/cerrar", headers=_clave(), timeout=10)
        except httpx.HTTPError:
            pass
