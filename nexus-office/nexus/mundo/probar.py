"""`nexus mundo`: estado de las fuentes y prueba en vivo (pide cada fuente una vez y cuenta lo que llega)."""
from __future__ import annotations

import json
import time


def probar(mundo, solo: list[str] | None = None, todas: bool = False) -> list[dict]:
    """Pide una vez cada fuente activa (o las de «solo»; con todas, tambien las apagadas) y devuelve el resultado."""
    filas = []
    for f in mundo.fuentes.values():
        a = mundo.ajustes(f)
        if solo and f.id not in solo:
            continue
        if not solo and not todas and not a["activa"]:
            continue
        if not a["con_clave"]:
            filas.append({"fuente": f.id, "categoria": f.categoria, "ok": None, "registros": 0, "ms": 0,
                          "nota": f"necesita la clave «{f.clave}»"})
            continue
        t0 = time.time()
        e = mundo.actualizar(f.id)
        fila = {"fuente": f.id, "categoria": f.categoria, "ok": not e.get("error"), "registros": e.get("registros") or 0,
                "ms": round((time.time() - t0) * 1000), "nota": (e.get("error") or e.get("avisos") or "")[:160]}
        if fila["ok"]:
            muestra = mundo.consultar(fuente=f.id, limite=1, corto=True)
            if muestra:
                fila["ejemplo"] = (muestra[0].get("pais") or "--") + " · " + muestra[0]["titulo"][:90]
        filas.append(fila)
    return filas


def ejecutar(args) -> int:
    from ..config import get_config
    from . import Mundo
    m = Mundo(get_config())
    try:
        if args.accion == "probar":
            filas = probar(m, args.solo or None, args.todas)
            if args.json:
                print(json.dumps(filas, ensure_ascii=False, indent=1))
            else:
                for f in filas:
                    estado = "sin clave" if f["ok"] is None else "bien" if f["ok"] else "FALLA"
                    print(f"{f['fuente']:<22}{f['categoria']:<12}{estado:<10}{f['registros']:>6} reg {f['ms']:>7} ms  "
                          f"{f.get('ejemplo') or f['nota']}")
                bien = sum(1 for f in filas if f["ok"])
                print(f"\n{bien} de {len(filas)} fuentes han respondido.")
            return 0
        if args.accion == "resumen":
            print(json.dumps(m.resumen(args.pais or "", args.categoria or ""), ensure_ascii=False, indent=1))
            return 0
        e = m.estado()
        if args.json:
            print(json.dumps(e, ensure_ascii=False, indent=1))
            return 0
        print(f"Mundo {'activo' if e['activo'] else 'apagado'} · factor {e['factor']} · países {', '.join(e['paises'])}"
              f" · {e['total']} registros")
        for f in e["fuentes"]:
            hace = "nunca" if f["hace_s"] is None else f"hace {f['hace_s'] // 60} min"
            print(f"{f['id']:<22}{f['categoria']:<12}{'sí' if f['activa'] else 'no':<4}cada {f['intervalo_s'] // 60:>5} min"
                  f"{f['registros']:>7} reg  {hace:<14}{f['error'][:60]}")
        return 0
    finally:
        m.cerrar()
