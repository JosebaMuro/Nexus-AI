"""Banco de motores: mide en ESTE equipo cada motor instalado con sus modelos y se queda con el mas rapido.

Para cada modelo de texto del catalogo cuyo motor esta instalado, genera una respuesta corta y mide el tiempo hasta
el primer token y los tokens por segundo. El resultado se guarda en ``data/motores/banco.json`` y el selector lo usa
por encima del orden de partida de la matriz: si en tu portatil OpenVINO en la NPU va mas lento que llama.cpp en la
iGPU, se elige llama.cpp, aunque la matriz diga lo contrario.
"""
from __future__ import annotations

import json
import time

from .. import log, paths

L = log.get("banco")
PROMPT = [{"role": "system", "content": "Responde en espanol, breve."},
          {"role": "user", "content": "Explica en tres frases que es una NPU y para que sirve."}]


def _file():
    return paths.sub("motores") / "banco.json"


def cargar() -> dict:
    try:
        return json.loads(_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"resultados": {}}


def guardar(data: dict) -> None:
    f = _file()
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(json.dumps(data, indent=1, ensure_ascii=False), encoding="utf-8")


def medir(backend, model_id: str, max_tokens: int = 96, n_ctx: int = 2048) -> dict:
    # una respuesta minima antes de medir: carga el modelo (y compila en la NPU o la GPU) para que el tiempo hasta el
    # primer token sea el de un chat ya abierto y no el de la carga, que se apunta aparte
    t_carga = time.time()
    for _ in backend.stream_chat([{"role": "user", "content": "hola"}], model=model_id, max_tokens=1, temperature=0.0,
                                 n_ctx=n_ctx):
        pass   # hasta el final: cortar a medias dejaria el motor ocupado
    carga = time.time() - t_carga
    t0 = time.time()
    ttft, piezas, texto = None, 0, ""
    for piece in backend.stream_chat(PROMPT, model=model_id, max_tokens=max_tokens, temperature=0.0, n_ctx=n_ctx):
        if ttft is None:
            ttft = time.time() - t0
        piezas += 1
        texto += piece
    total = time.time() - t0
    st = getattr(backend, "last_stats", {}) or {}
    tokens = st.get("tokens") or piezas
    gen_s = max(1e-3, total - (ttft or 0))
    return {"tps": round(tokens / gen_s, 2), "ttft_s": round(ttft or total, 3), "total_s": round(total, 2), "carga_s": round(carga, 2),
            "tokens": tokens, "acelerador": getattr(backend, "efectivo", {}).get("acelerador", ""),
            "muestra": texto[:160], "cuando": time.time()}


def ejecutar(llm_manager, catalog_items: list[dict], solo: list[str] | None = None, progreso=None) -> dict:
    """Mide todos los modelos de texto con motor instalado (o solo los ``motor:modelo`` de ``solo``)."""
    from . import disponible, motor_de_formato   # noqa: PLC0415
    data = cargar()
    res = data.setdefault("resultados", {})
    for e in catalog_items:
        if e.get("kind") != "llm":
            continue
        motor = motor_de_formato(e.get("format", ""))
        clave = f"{motor}:{e['id']}"
        if not motor or (solo and clave not in solo) or not disponible(motor)[0]:
            continue
        b = llm_manager.backends.get(motor)
        if b is None:
            continue
        if progreso:
            progreso(f"Midiendo {clave}")
        try:
            res[clave] = medir(b, e["id"])
            L.info("Banco %s: %.1f tok/s (primer token %.2f s, %s)", clave, res[clave]["tps"], res[clave]["ttft_s"],
                   res[clave]["acelerador"])
        except Exception as ex:  # noqa: BLE001
            res[clave] = {"error": str(ex)[:300], "cuando": time.time()}
            L.warning("Banco %s: %s", clave, ex)
        finally:
            if motor != "llamaserver" and hasattr(b, "stop"):
                b.stop()   # libera la memoria antes de cargar el siguiente
    data["cuando"] = time.time()
    guardar(data)
    return data
