"""Perfiles de hardware configurables: motor, acelerador, GPU, modelo y donde va cada cosa (VRAM o RAM).

Un perfil junta en un nombre lo que antes habia que tocar por separado:

* ``motor``: auto · llamaserver · llamacpp, y ``acelerador``: auto · cpu · gpu · npu (``accel_choice``).
* ``gpu``: cual usar si hay varias (nombre o dispositivo de llama.cpp, p. ej. «Vulkan1»); vacio = la mejor.
* ``modelo``: modelo de texto que se activa con el perfil (vacio = el que haya).
* ``capas_gpu``: «auto», «todas» (modelo entero en la VRAM), «ninguna» (todo en la RAM) o un numero de capas
  (lo que no cabe en la VRAM va a la RAM). ``kv``: «auto», «vram» o «ram» (la cache de contexto en la RAM deja la
  VRAM para mas capas). ``mlock``: el modelo fijo en la RAM. ``contexto``, ``hilos`` y ``lote`` (0 = automatico;
  contexto -1 = el maximo que aguanta el equipo).

Hay perfiles de serie (Automatico, Rendimiento, Equilibrado, Ahorro, Solo CPU, Modelo grande) y el usuario puede
crear los suyos. Se eligen en la barra superior (PERFIL), en Sistema o con los modos (Rendimiento, Ahorro...).
"""
from __future__ import annotations

import re

from .. import log
from . import profiles

L = log.get("perfiles")

CAMPOS = {"nombre": "", "descripcion": "", "motor": "auto", "acelerador": "auto", "gpu": "", "modelo": "",
          "capas_gpu": "auto", "kv": "auto", "mlock": False, "contexto": 0, "hilos": 0, "lote": 0}

DE_SERIE: list[dict] = [
    {"id": "auto", "nombre": "Automático", "descripcion": "Lo que Nexus elige para tu equipo"},
    {"id": "rendimiento", "nombre": "Rendimiento", "descripcion": "GPU, modelo entero en la VRAM y contexto máximo",
     "acelerador": "gpu", "capas_gpu": "todas", "kv": "vram", "contexto": -1, "lote": 1024},
    {"id": "equilibrado", "nombre": "Equilibrado", "descripcion": "GPU si la hay, contexto moderado",
     "contexto": 8192},
    {"id": "grande", "nombre": "Modelo grande", "descripcion": "Parte del modelo en la VRAM y el resto y el contexto en la RAM",
     "acelerador": "gpu", "capas_gpu": "auto", "kv": "ram", "mlock": True},
    {"id": "ahorro", "nombre": "Ahorro", "descripcion": "CPU, poco contexto y la mitad de los hilos: menos consumo",
     "acelerador": "cpu", "contexto": 2048, "hilos": -50},
    {"id": "cpu", "nombre": "Solo CPU", "descripcion": "Sin GPU ni NPU", "acelerador": "cpu"},
]


def _norm(p: dict) -> dict:
    out = {**CAMPOS, **{k: v for k, v in p.items() if k in CAMPOS or k in ("id", "de_serie")}}
    out["id"] = re.sub(r"[^a-z0-9_-]+", "-", str(p.get("id") or out["nombre"] or "perfil").lower()).strip("-")[:40] or "perfil"
    if out["motor"] not in ("auto", "llamaserver", "llamacpp"):
        out["motor"] = "auto"
    if out["acelerador"] not in ("auto", "cpu", "gpu", "npu"):
        out["acelerador"] = "auto"
    if out["kv"] not in ("auto", "vram", "ram"):
        out["kv"] = "auto"
    c = out["capas_gpu"]
    if not (c in ("auto", "todas", "ninguna") or (isinstance(c, int) and not isinstance(c, bool) and c >= 0)):
        try:
            out["capas_gpu"] = max(0, int(c))
        except (TypeError, ValueError):
            out["capas_gpu"] = "auto"
    for k in ("contexto", "hilos", "lote"):
        try:
            out[k] = int(out[k] or 0)
        except (TypeError, ValueError):
            out[k] = 0
    out["mlock"] = bool(out["mlock"])
    return out


def lista(cfg) -> list[dict]:
    propios = [_norm(p) for p in (cfg.get("hardware.perfiles", []) or []) if isinstance(p, dict)]
    ids = {p["id"] for p in propios}
    return [{**_norm(p), "de_serie": True} for p in DE_SERIE if p["id"] not in ids] + propios


def get(cfg, pid: str) -> dict | None:
    return next((p for p in lista(cfg) if p["id"] == pid), None)


def guardar(cfg, p: dict) -> dict:
    p = _norm(p)
    propios = [x for x in (cfg.get("hardware.perfiles", []) or []) if isinstance(x, dict) and x.get("id") != p["id"]]
    propios.append({k: v for k, v in p.items() if k != "de_serie"})
    cfg.set("hardware.perfiles", propios)
    return p


def borrar(cfg, pid: str) -> bool:
    propios = [x for x in (cfg.get("hardware.perfiles", []) or []) if isinstance(x, dict)]
    nuevos = [x for x in propios if x.get("id") != pid]
    if len(nuevos) == len(propios):
        return False
    cfg.set("hardware.perfiles", nuevos)
    if cfg.get("hardware.perfil", "auto") == pid:
        cfg.set("hardware.perfil", "auto")
    return True


def superponer(profile: dict, p: dict) -> dict:
    """Aplica al perfil calculado los ajustes del perfil de hardware (contexto, capas, KV, hilos...)."""
    llm = profile["llm"]
    gpu_on = llm.get("gpu_layers", 0) != 0
    if p["contexto"] == -1:
        llm["n_ctx"] = llm.get("max_ctx", llm["n_ctx"])
    elif p["contexto"] > 0:
        llm["n_ctx"] = max(512, min(p["contexto"], llm.get("max_ctx", p["contexto"]) * 2))
    if gpu_on:
        c = p["capas_gpu"]
        llm["gpu_layers"] = -1 if c == "todas" else 0 if c == "ninguna" else c if isinstance(c, int) and c > 0 else llm["gpu_layers"]
    llm["kv_ram"] = p["kv"] == "ram"
    llm["mlock"] = p["mlock"]
    if p["hilos"] > 0:
        llm["n_threads"] = p["hilos"]
    elif p["hilos"] < 0:   # porcentaje de los automaticos (-50 = la mitad)
        llm["n_threads"] = max(1, round(llm["n_threads"] * -p["hilos"] / 100))
    if p["lote"] > 0:
        llm["n_batch"] = p["lote"]
    if p["gpu"] and gpu_on:
        llm["gpu_select"] = p["gpu"]
    profile["perfil_hw"] = {"id": p["id"], "nombre": p["nombre"]}
    return profile


def al_arrancar(cfg) -> None:
    """Antes de construir el perfil: la GPU elegida (para que el calculo de VRAM y modelo sea el de esa GPU)."""
    p = get(cfg, cfg.get("hardware.perfil", "auto") or "auto")
    profiles.select_gpu((p or {}).get("gpu") or cfg.get("hardware.gpu", ""))


def aplicar_al_perfil(services) -> None:
    """Tras construir el perfil al arrancar: superpone el perfil de hardware activo."""
    p = get(services.config, services.config.get("hardware.perfil", "auto") or "auto")
    if p and p["id"] != "auto":
        superponer(services.profile, p)


def activar(pid: str, cfg=None, services=None) -> dict:
    """Activa un perfil: motor y acelerador (reconstruye el perfil), GPU, ajustes de memoria y modelo."""
    cfg = cfg if cfg is not None else services.config
    p = get(cfg, pid)
    if not p:
        raise ValueError(f"perfil de hardware desconocido: {pid}")
    cfg.set("hardware.perfil", p["id"])
    cfg.set("hardware.gpu", p["gpu"])
    profiles.select_gpu(p["gpu"])
    res: dict = {"perfil": p, "nota": ""}
    if services is None:
        return res   # se aplicara al arrancar
    from .accel_choice import apply
    r = apply(services, p["motor"], p["acelerador"], remember=None)
    res["nota"] = r.get("note", "")
    superponer(services.profile, p)
    if p["modelo"]:
        try:
            services.models.activate(p["modelo"])
        except (KeyError, ValueError) as e:
            res["nota"] = "; ".join(x for x in (res["nota"], f"modelo {p['modelo']}: {e}") if x)
    services.llm.rebuild()
    L.info("Perfil de hardware «%s» activo (%s, %s)", p["nombre"], p["motor"], p["acelerador"])
    res["llm"] = {k: services.profile["llm"].get(k) for k in ("n_ctx", "gpu_layers", "kv_ram", "mlock", "n_threads",
                                                             "n_batch", "gpu_select")}
    return res


def elegir_gpu(services, gpu: str) -> dict:
    """Cambia solo la GPU (barra superior): reconstruye el perfil con esa GPU y reinicia el modelo."""
    cfg = services.config
    cfg.set("hardware.gpu", gpu or "")
    profiles.select_gpu(gpu or "")
    from .accel_choice import build_for
    profile, _ = build_for(services.hw, cfg.get("hardware.accel", "auto"))
    p = get(cfg, cfg.get("hardware.perfil", "auto") or "auto")
    if p and p["id"] != "auto":
        superponer(profile, {**p, "gpu": gpu or p["gpu"]})
    services.profile.clear()
    services.profile.update(profile)
    services.llm.rebuild()
    return {"gpu": gpu, "dispositivo": profile["device_name"], "acelerador": profile["accelerator"]}


def gpus(services) -> list[dict]:
    """GPU del equipo con lo que ve llama.cpp de cada una, para el selector."""
    hw = services.hw
    devs = (hw.runtimes or {}).get("llama_devices", [])
    sel = profiles.SELECTED["gpu"]
    out = []
    for i, g in enumerate(hw.gpus):
        dev = next((d for d in devs if not d.get("software") and profiles._matches(g, d.get("name", ""))), None)
        out.append({"indice": i, "nombre": g.name, "fabricante": g.vendor, "vram_gb": g.vram_gb or (round(dev["mem_mb"] / 1024, 1) if dev else 0),
                    "integrada": g.integrated, "llama": dev["id"] if dev else "", "variante": dev.get("variant", "") if dev else "",
                    "elegida": bool(sel) and (profiles._matches(g, sel) or (dev is not None and dev["id"].lower() == sel.lower()))})
    soft = [d for d in devs if d.get("software")]
    return out + [{"indice": -1, "nombre": d["name"], "fabricante": "software", "vram_gb": 0, "integrada": False,
                   "llama": d["id"], "software": True, "elegida": False} for d in soft]
