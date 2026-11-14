"""Laboratorio BETA: tiempos de la API (cabecera Server-Timing y estadistica por ruta), banco de pruebas del equipo
(base de datos, embeddings, memoria, RAG, grafo y LLM) y opiniones de quien prueba. Desde la terminal: ``nexus bench``.
"""
from __future__ import annotations

import json
import platform
import threading
import time
from collections import deque

from . import __version__, paths

_mu = threading.Lock()
_rutas: dict[str, dict] = {}
_lentas: deque = deque(maxlen=30)
_ultimo_banco: dict | None = None


# ------------------------------------------------------------------ tiempos de la API
def anotar(metodo: str, ruta: str, ms: float, estado: int) -> None:
    clave = f"{metodo} {ruta}"
    with _mu:
        r = _rutas.get(clave)
        if r is None:
            if len(_rutas) >= 400:
                return
            r = _rutas[clave] = {"n": 0, "total": 0.0, "max": 0.0, "errores": 0, "ultimas": deque(maxlen=200)}
        r["n"] += 1
        r["total"] += ms
        r["max"] = max(r["max"], ms)
        r["errores"] += estado >= 500
        r["ultimas"].append(ms)
        if ms >= 500:
            _lentas.append({"ts": time.time(), "ruta": clave, "ms": round(ms, 1), "estado": estado})


def tiempos(limite: int = 60) -> dict:
    with _mu:
        filas = []
        for clave, r in _rutas.items():
            u = sorted(r["ultimas"])
            filas.append({"ruta": clave, "n": r["n"], "media_ms": round(r["total"] / r["n"], 2),
                          "p50_ms": round(u[len(u) // 2], 2), "p95_ms": round(u[min(len(u) - 1, int(len(u) * 0.95))], 2),
                          "max_ms": round(r["max"], 2), "errores": r["errores"]})
        filas.sort(key=lambda f: -f["media_ms"] * f["n"])
        return {"rutas": filas[:limite], "lentas": list(_lentas), "total_rutas": len(_rutas),
                "peticiones": sum(r["n"] for r in _rutas.values())}


def borrar_tiempos() -> None:
    with _mu:
        _rutas.clear()
        _lentas.clear()


# ------------------------------------------------------------------ banco de pruebas
def _medir(nombre: str, fn, unidad: str, repeticiones: int = 1) -> dict:
    t0 = time.perf_counter()
    try:
        n = 0
        for _ in range(repeticiones):
            n += fn() or 1
        s = time.perf_counter() - t0
        return {"prueba": nombre, "ok": True, "ms": round(s * 1000, 1), "valor": round(n / s, 1) if s > 0 else 0.0,
                "unidad": unidad}
    except Exception as e:  # noqa: BLE001
        return {"prueba": nombre, "ok": False, "ms": round((time.perf_counter() - t0) * 1000, 1),
                "error": f"{type(e).__name__}: {e}"[:200]}


def banco(s, con_llm: bool = True) -> dict:
    """Mide las piezas que marcan la velocidad de un turno. No toca los datos del usuario (tabla y textos propios)."""
    global _ultimo_banco
    out = []
    db = s.db
    texto = ("La oficina prepara el presupuesto trimestral con las facturas de proveedores y los pedidos pendientes "
             "de entrega para revisar el margen de cada linea de producto. ")

    def escribir():
        db.execute("CREATE TABLE IF NOT EXISTS beta_banco (id INTEGER PRIMARY KEY, t TEXT)")
        db.execute("DELETE FROM beta_banco")
        db.executemany("INSERT INTO beta_banco(t) VALUES(?)", [(texto + str(i),) for i in range(2000)])
        return 2000

    def leer():
        n = 0
        for i in range(200):
            n += len(db.query("SELECT id, t FROM beta_banco WHERE id>? LIMIT 20", (i * 7,)))
        return n

    out.append(_medir("Base de datos: escribir", escribir, "filas/s"))
    out.append(_medir("Base de datos: leer", leer, "filas/s"))
    try:
        db.execute("DROP TABLE IF EXISTS beta_banco")
    except Exception:  # noqa: BLE001
        pass

    emb = s.embedder
    encode = getattr(emb, "embed", None) or getattr(emb, "encode", None)
    if encode is not None:
        frases = [texto + str(i) for i in range(24)]

        def embeber():
            try:
                encode(frases)
            except TypeError:
                for f in frases:
                    encode(f)
            return len(frases)
        r = _medir(f"Embeddings ({getattr(emb, 'id', '?')})", embeber, "textos/s")
        out.append(r)
    out.append(_medir("Memoria: recordar", lambda: s.memory.recall("presupuesto trimestral de la oficina", k=6) and 1,
                      "consultas/s", 5))
    out.append(_medir("Biblioteca RAG: buscar", lambda: s.rag.search("facturas de proveedores", k=4) and 1,
                      "consultas/s", 5))
    out.append(_medir("Grafo: consultar", lambda: len(s.graph.to_json(120)["nodes"]) + 1, "nodos/s", 3))
    out.append(_medir("Agentes: enrutar", lambda: len(s.agents.route("necesito una factura para un cliente")) and 1,
                      "mensajes/s", 10))
    if con_llm and s.llm.has_real_llm():
        def generar():
            t0, primero, n = time.perf_counter(), None, 0
            for trozo in s.llm.stream([{"role": "user", "content": "Cuenta del 1 al 30 separando con comas."}],
                                      max_tokens=96, temperature=0.0, reasoning="off"):
                if trozo and primero is None:
                    primero = time.perf_counter() - t0
                n += 1
            generar.primero = primero
            return max(1, n)
        r = _medir("LLM: generar", generar, "tokens/s (aprox.)")
        if r["ok"] and getattr(generar, "primero", None) is not None:
            r["primer_token_ms"] = round(generar.primero * 1000, 1)
        try:
            r["modelo"] = ":".join(s.llm.select("default"))
        except Exception:  # noqa: BLE001
            pass
        out.append(r)
    else:
        out.append({"prueba": "LLM: generar", "ok": False, "ms": 0, "error": "sin modelo local cargado"})
    hw = s.hw.to_dict() if hasattr(s.hw, "to_dict") else {}
    res = {"ts": time.time(), "version": __version__, "sistema": f"{platform.system()} {platform.machine()}",
           "cpu": (hw.get("cpu") or {}).get("name", "") if isinstance(hw.get("cpu"), dict) else "",
           "pruebas": out, "fallos": sum(1 for p in out if not p["ok"] and p["prueba"] != "LLM: generar")}
    _ultimo_banco = res
    try:
        (paths.sub("beta") / "banco.json").write_text(json.dumps(res, ensure_ascii=False, indent=1), encoding="utf-8")
    except OSError:
        pass
    return res


def ultimo_banco() -> dict | None:
    global _ultimo_banco
    if _ultimo_banco is None:
        try:
            _ultimo_banco = json.loads((paths.sub("beta") / "banco.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
    return _ultimo_banco


# ------------------------------------------------------------------ opiniones
def _fichero_opiniones():
    return paths.sub("beta") / "opiniones.jsonl"


def opinar(texto: str, nota: int = 0, vista: str = "") -> dict:
    texto = (texto or "").strip()[:4000]
    if not texto:
        raise ValueError("Escribe la opinión")
    fila = {"ts": time.time(), "texto": texto, "nota": max(0, min(int(nota or 0), 5)), "vista": vista[:40],
            "version": __version__}
    with _mu, open(_fichero_opiniones(), "a", encoding="utf-8") as f:
        f.write(json.dumps(fila, ensure_ascii=False) + "\n")
    return fila


def opiniones(limite: int = 100) -> list[dict]:
    try:
        lineas = _fichero_opiniones().read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    out = []
    for linea in lineas[-limite:]:
        try:
            out.append(json.loads(linea))
        except ValueError:
            continue
    return out[::-1]


def informe() -> str:
    """Informe en Markdown para mandar a quien desarrolla: version, banco, rutas lentas y opiniones."""
    b, t, ops = ultimo_banco(), tiempos(15), opiniones(50)
    lineas = [f"# Informe beta de Agente IA {__version__}", "",
              f"- Sistema: {platform.system()} {platform.release()} ({platform.machine()})",
              f"- Fecha: {time.strftime('%Y-%m-%d %H:%M')}", ""]
    if b:
        lineas += ["## Banco de pruebas", "", "| Prueba | Resultado | Tiempo |", "|---|---|---|"]
        for p in b["pruebas"]:
            lineas.append(f"| {p['prueba']} | " + (f"{p['valor']} {p['unidad']}" if p["ok"] else f"falló: {p.get('error', '')}") +
                          f" | {p['ms']} ms |")
        lineas.append("")
    if t["rutas"]:
        lineas += ["## Rutas de la API con más tiempo acumulado", "", "| Ruta | Veces | Media | p95 | Máx. |",
                   "|---|---|---|---|---|"]
        lineas += [f"| {r['ruta']} | {r['n']} | {r['media_ms']} ms | {r['p95_ms']} ms | {r['max_ms']} ms |"
                   for r in t["rutas"]]
        lineas.append("")
    if ops:
        lineas += ["## Opiniones", ""] + [f"- ({'★' * o['nota'] or 's/n'}) {o['texto']}" for o in ops]
    return "\n".join(lineas) + "\n"
