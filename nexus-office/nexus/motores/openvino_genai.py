"""OpenVINO GenAI (Intel): NPU de los Core Ultra, iGPU Xe/Arc y CPU.

El modelo es una carpeta exportada a OpenVINO (``openvino_model.xml`` + ``openvino_tokenizer.xml``), p. ej. los de
la organizacion OpenVINO en Hugging Face (int4). En la NPU el primer arranque compila el modelo: la cache
(``CACHE_DIR``) hace que el segundo sea mucho mas rapido.
"""
from __future__ import annotations

import queue
import threading
from pathlib import Path
from typing import Callable, Iterator

from .. import paths
from . import Motor, Params, mensajes_simples, plantillas


class OpenVinoGenAI(Motor):
    id = "openvino"

    def __init__(self, modelo: Path, acelerador: str, params: Params) -> None:
        super().__init__(modelo, acelerador.upper() or "CPU", params)
        import openvino_genai as ov   # noqa: PLC0415
        self.ov = ov
        self.carpeta = self.modelo if self.modelo.is_dir() else self.modelo.parent
        cache = paths.sub("motores", "cache-openvino")
        props = {"CACHE_DIR": str(cache)}
        if self.acelerador in ("CPU", "GPU"):
            props["PERFORMANCE_HINT"] = "LATENCY"   # una respuesta cada vez: prioridad al primer token, no al caudal
        if self.acelerador == "CPU" and params.hilos > 0:
            # los nucleos fisicos (o los rapidos) del perfil: con los eficientes mezclados la generacion va mas lenta
            props["INFERENCE_NUM_THREADS"] = params.hilos
        if self.acelerador == "NPU":
            # la NPU necesita un tamano de prompt fijo: se reserva lo que cabe en el contexto
            props["MAX_PROMPT_LEN"] = max(1024, min(params.n_ctx - 512, 4096))
            props["MIN_RESPONSE_LEN"] = 512
        try:
            self.pipe = ov.LLMPipeline(str(self.carpeta), self.acelerador, **props)
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(f"OpenVINO no pudo cargar el modelo en {self.acelerador}: {e}") from e

    def _prompt(self, msgs: list[dict]) -> str:
        try:
            return self.pipe.get_tokenizer().apply_chat_template(msgs, add_generation_prompt=True)
        except Exception:  # noqa: BLE001
            return plantillas.prompt(self.carpeta, msgs)

    def generar(self, messages: list[dict], p: Params, parar: Callable[[], bool]) -> Iterator[str]:
        ov = self.ov
        prompt = self._prompt(mensajes_simples(messages))
        cfg = ov.GenerationConfig()
        cfg.max_new_tokens = max(16, p.max_tokens)
        if p.temperature > 0.01:
            cfg.do_sample = True
            cfg.temperature, cfg.top_p, cfg.top_k = p.temperature, p.top_p, max(1, p.top_k)
        if hasattr(cfg, "apply_chat_template"):
            cfg.apply_chat_template = False   # el prompt ya lleva la plantilla
        q: queue.Queue = queue.Queue()
        fin = object()
        estado = getattr(ov, "StreamingStatus", None)

        def streamer(sub: str):
            q.put(sub)
            cortar = parar()
            if estado is not None:
                return estado.CANCEL if cortar else estado.RUNNING
            return cortar   # versiones anteriores: True = parar

        error: list[BaseException] = []

        def trabajo():
            try:
                self.pipe.generate(prompt, cfg, streamer)
            except BaseException as e:  # noqa: BLE001
                error.append(e)
            finally:
                q.put(fin)
        threading.Thread(target=trabajo, daemon=True, name="openvino-gen").start()
        while True:
            x = q.get()
            if x is fin:
                break
            yield plantillas.limpiar(x)
        if error and not parar():
            raise RuntimeError(f"OpenVINO: {error[0]}")

    def cerrar(self) -> None:
        self.pipe = None
