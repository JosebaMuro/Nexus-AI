"""Backend principal: llama-server (llama.cpp oficial) gestionado como subproceso.

Soporta modelos multimodales (vision) con ``--mmproj`` y expone la API OpenAI en localhost.
Se reinicia automaticamente si cambian modelo o ajustes (contexto, hilos, capas GPU...).
"""
from __future__ import annotations

import os
import socket
import subprocess
import threading
import time
from pathlib import Path

import httpx

from .. import log, paths
from ..proc import popen
from .backends import OpenAICompatBackend

L = log.get("llamaserver")


def _free_port(preferred: int) -> int:
    for port in (preferred, 0):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return s.getsockname()[1]
            except OSError:
                continue
    return preferred


class _GpuFailed(RuntimeError):
    pass


class LlamaServerBackend(OpenAICompatBackend):
    name = "llamaserver"

    def __init__(self, runtimes, resolve, profile_getter, variant_getter=lambda: "") -> None:
        super().__init__("http://127.0.0.1:0/v1")
        self.runtimes, self.resolve, self.profile_getter, self.variant_getter = runtimes, resolve, profile_getter, variant_getter
        self.proc: subprocess.Popen | None = None
        self.key = None
        self.variant = ""
        self.effective: dict = {}      # donde ha quedado el modelo de verdad (leido del registro al arrancar)
        self._lock = threading.Lock()
        self.port = 0
        self.gpu: dict = {}            # GPU de llama.cpp elegida (cuando hay varias)
        self.gpu_error = ""

    def available(self) -> bool:
        return self.runtimes.exe("llama", self.profile_getter(), self.variant_getter()) is not None and self.resolve("") is not None

    def models(self) -> list[str]:
        r = self.resolve("")
        return [r[1]["id"]] if r else []

    def supports_vision(self, model: str = "") -> bool:
        r = self.resolve(model)
        return bool(r and r[1].get("mmproj"))

    def ensure(self, model: str, opts: dict) -> None:
        # Android: si la NPU o la GPU no arrancan con este driver, se apunta y se pasa a la siguiente (NPU -> GPU -> CPU)
        for _ in range(3):
            try:
                return self._ensure(model, opts)
            except _GpuFailed as e:
                if not os.environ.get("NEXUS_RUNTIME_LLAMA_CPU"):   # escritorio: la GPU elegida no arranca -> CPU
                    L.warning("%s: se sigue en la CPU", e)
                    self.gpu_error = str(e)[:300]
                    opts = {**opts, "gpu_layers": 0, "device": ""}
                    continue
                opts = {**opts, **self._gpu_failed(str(e), opts.get("device") or "")}

    def _ensure(self, model: str, opts: dict) -> None:
        resolved = self.resolve(model)
        if not resolved:
            raise RuntimeError("No hay modelo GGUF activo. Descarga uno en Modelos.")
        path, entry = resolved
        found = self.runtimes.exe("llama", self.profile_getter(), self.variant_getter())
        if not found:
            raise RuntimeError("Runtime llama.cpp no instalado (Sistema > Runtimes)")
        exe, variant = found
        ngl = opts.get("gpu_layers", 0)
        env, device = None, ""
        if variant == "embedded" and os.environ.get("NEXUS_RUNTIME_LLAMA_CPU"):
            # app Android: llama-server con GPU (Vulkan/OpenCL) si el perfil la usa; si no, el de CPU
            from ..hardware import android_accel
            device = opts.get("device") or "" if ngl else ""
            exe = Path(os.environ["NEXUS_RUNTIME_LLAMA_GPU" if device and os.environ.get("NEXUS_RUNTIME_LLAMA_GPU")
                                  else "NEXUS_RUNTIME_LLAMA_CPU"])
            if android_accel.is_gpu_exe(exe):
                env = android_accel.gpu_env(str(exe.parent), npu=android_accel.is_npu_dev(device))
            else:
                ngl, device = 0, ""
        if variant == "cpu":
            ngl = 0
        extra_env = dict(self.profile_getter().get("llm", {}).get("device_env") or {})
        if extra_env:      # OpenVINO NPU (GGML_OPENVINO_DEVICE) y similares
            env = {**(env or os.environ), **extra_env}
        if variant not in ("embedded", "system"):
            from ..runtimes import lib_env
            env = lib_env(exe, env)
        single = bool(self.profile_getter().get("llm", {}).get("single_slot"))
        if ngl and not device and variant not in ("cpu", "embedded"):
            # escritorio: con varias GPU (o una real y llvmpipe en Linux) se fija UNA, la elegida o la mejor
            from ..hardware import gpu_devices
            devs = gpu_devices.list_devices(exe, env)
            sel = gpu_devices.pick(devs, str(self.profile_getter().get("llm", {}).get("gpu_select") or ""))
            if devs and sel is None:
                L.warning("llama.cpp solo ve renderizadores por software (%s): se usa la CPU",
                          ", ".join(d["name"] for d in devs))
                ngl = 0
            elif sel and gpu_devices.needs_device_flag(devs):
                device = sel["id"]
            self.gpu = sel or {}
        kv_ram, mlock = bool(opts.get("kv_ram")), bool(opts.get("mlock"))
        key = (str(exe), path, entry.get("mmproj"), opts.get("n_ctx"), opts.get("n_threads"), opts.get("n_batch"), ngl, device,
               tuple(sorted(extra_env.items())), kv_ram, mlock)
        with self._lock:
            if self.proc and self.proc.poll() is None and self.key == key:
                return
            self.stop()
            self.port = _free_port(8181)
            cmd = [str(exe), "-m", path, "--host", "127.0.0.1", "--port", str(self.port),
                   "-c", str(opts.get("n_ctx", 4096)), "-b", str(opts.get("n_batch", 256)),
                   "-ngl", str(999 if ngl == -1 else ngl), "--jinja", "--no-webui"]
            if opts.get("n_threads"):
                cmd += ["-t", str(opts["n_threads"])]
            if device:
                cmd += ["--device", device]
            if kv_ram and ngl:     # perfil de hardware: la cache KV en la RAM (deja la VRAM para mas capas del modelo)
                cmd += ["--no-kv-offload"]
            if mlock:              # perfil de hardware: el modelo fijo en la RAM (sin paginar a disco)
                cmd += ["--mlock"]
            # en CPU un solo slot: las peticiones se encolan en vez de repartirse los pocos nucleos;
            # cache-reuse reaprovecha el KV del prompt de sistema entre turnos
            cmd += ["-np", "1" if variant in ("cpu", "system") or single else "2"]
            if not entry.get("mmproj"):  # no soportado con proyector multimodal
                cmd += ["--cache-reuse", "256"]
            if entry.get("mmproj"):
                cmd += ["--mmproj", entry["mmproj"]]
                if variant == "cpu":  # en CPU limitamos los tokens por imagen para acelerar la vision
                    cmd += ["--image-max-tokens", "384"]
            log_path = paths.logs_dir() / "llama-server.log"
            log_from = log_path.stat().st_size if log_path.exists() else 0
            logf = open(log_path, "ab")
            L.info("Arrancando llama-server [%s]: %s", variant, " ".join(cmd[1:]))
            self.proc = popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=str(exe.parent), env=env)
            self.base_url = f"http://127.0.0.1:{self.port}/v1"
            self.key, self.variant = key, variant
            t0 = time.time()
            while time.time() - t0 < 300:
                if self.proc.poll() is not None:
                    tail = (paths.logs_dir() / "llama-server.log").read_bytes()[-1500:].decode(errors="replace")
                    if "--no-webui" in tail or "invalid argument" in tail:
                        cmd.remove("--no-webui")  # versiones antiguas no conocen la opcion
                        self.proc = popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=str(exe.parent), env=env)
                        continue
                    self.proc = None
                    if device:
                        raise _GpuFailed(f"llama-server con {'NPU' if device.startswith('HTP') else 'GPU'} ({device}) termino al arrancar: {tail[-300:]}")
                    raise RuntimeError(f"llama-server termino al arrancar: {tail[-600:]}")
                try:
                    if httpx.get(f"http://127.0.0.1:{self.port}/health", timeout=1).status_code == 200:
                        L.info("llama-server listo en %.1fs", time.time() - t0)
                        self._check_device(log_path, log_from, variant, ngl, device, extra_env)
                        return
                except httpx.HTTPError:
                    pass
                time.sleep(0.5)
            if device:
                self.stop()
                raise _GpuFailed(f"llama-server con GPU ({device}) no respondio a tiempo")
            raise RuntimeError("llama-server no respondio a tiempo")

    def _check_device(self, log_path, log_from: int, variant: str, ngl, device: str, extra_env: dict) -> None:
        """Comprueba en el registro donde se ha cargado el modelo y lo deja en ``effective`` para la interfaz."""
        from ..hardware.accel_choice import effective_from_log, read_log_since
        eff = effective_from_log(read_log_since(log_path, log_from), extra_env)
        eff.update(variant=variant, requested="NPU" if extra_env.get("GGML_OPENVINO_DEVICE") == "NPU" or
                   (device or "").startswith("HTP") else "GPU" if ngl else "CPU", at=time.time())
        eff["ok"] = eff["requested"] == "CPU" or eff["device"].startswith(eff["requested"])
        if not eff["ok"]:
            L.warning("Se pidio %s pero el modelo ha quedado en %s (%s)", eff["requested"], eff["device"],
                      eff.get("detail") or variant)
        self.effective = eff

    def _gpu_failed(self, reason: str, device: str = "") -> dict:
        """Android: la NPU o la GPU no sirve para llama.cpp en este movil (driver). Se apunta y se sigue con la GPU
        (si fallo la NPU y hay GPU utilizable) o con la CPU. Devuelve las opciones nuevas del modelo."""
        from ..hardware import android_accel
        android_accel.mark_gpu_broken(reason, device)
        nxt = {"gpu_layers": 0, "device": ""}
        if android_accel.is_npu_dev(device):
            use = (android_accel.detect().get("gpu") or {}).get("use") or {}
            if use.get("dev") and not android_accel.is_npu_dev(use["dev"]):
                nxt = {"gpu_layers": -1, "device": use["dev"]}
        L.warning("%s: se sigue en %s", reason, nxt["device"] or "CPU")
        llm = self.profile_getter().get("llm", {})
        llm["gpu_layers"], llm["device"] = nxt["gpu_layers"], nxt["device"]
        return nxt

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        self.key = None

    def stream_chat(self, messages, model="", **opts):
        self.ensure(model, opts)
        if not self.supports_vision(model):
            messages = [{**m, "images": []} for m in messages]
        produced = False
        try:
            for piece in super().stream_chat(messages, model="local", **opts):
                produced = True
                yield piece
        except (httpx.TransportError, httpx.RemoteProtocolError):
            if produced or (self.proc and self.proc.poll() is None):
                raise
            L.warning("llama-server se detuvo durante la peticion; reiniciando y reintentando")
            if self.key and self.key[-1]:   # se cayo usando la GPU: driver inestable, se sigue en CPU
                self._gpu_failed(f"llama-server con GPU ({self.key[-1]}) se detuvo durante una respuesta")
                opts = {**opts, "gpu_layers": 0, "device": ""}
            self.key = None
            self.ensure(model, opts)
            yield from super().stream_chat(messages, model="local", **opts)
