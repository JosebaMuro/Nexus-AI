"""`nexus terminal`: el Terminal IA desde la consola, interactivo o de un tiro (-p), con salida de texto o JSON."""
from __future__ import annotations

import json
import sys

AYUDA = """Terminal IA. Escribe lo que quieres hacer y el agente trabaja en la carpeta actual.
  !orden            ejecuta la orden tal cual en la consola (sin pasar por el agente)
  /py codigo        ejecuta Python en el intérprete con memoria
  /modo [plan|construir]   plan: el agente solo lee y propone; construir: ejecuta y edita
  /carpeta RUTA     cambia la carpeta de trabajo
  /tareas           lista de tareas de la sesión
  /deshacer         devuelve el último fichero cambiado a como estaba
  /ordenes          órdenes personalizadas y habilidades del proyecto
  /salir            termina"""


def _pinta(texto: str) -> None:
    sys.stdout.write(texto)
    sys.stdout.flush()


def ejecutar(args) -> int:   # noqa: C901
    from ..core import Services
    s = Services()
    s.start()
    t = s.terminal
    sid = s.memory.new_session("Terminal IA")
    if args.cwd:
        t.cambiar_carpeta(sid, args.cwd)
    if args.modo:
        t.modo(sid, args.modo)
    en_json = args.formato == "json"
    interactivo = not args.pregunta
    usadas: list[dict] = []

    def emit(e: dict) -> None:
        tipo = e.get("type")
        if tipo == "token" and not en_json:
            _pinta(e["text"])
        elif tipo == "tool_call":
            usadas.append({"herramienta": e["name"], "argumentos": e["args"]})
            if not en_json:
                resumen = json.dumps(e["args"], ensure_ascii=False)
                _pinta(f"\n  ❯ {e['name']} {resumen[:300]}\n")
        elif tipo == "terminal" and e.get("evento") == "salida" and not en_json:
            _pinta(e.get("texto", ""))
        elif tipo == "tool_result":
            if usadas:
                usadas[-1]["resultado"] = e.get("result", "")[:1500]
        elif tipo == "approval_required":
            a = e["approval"]
            if args.si:
                ok = True
            elif interactivo:
                ok = input(f"\n¿Permitir {a['tool']} {json.dumps(a['args'], ensure_ascii=False)[:400]}? (s/n) ") \
                    .strip().lower().startswith("s")
            else:
                ok = False
                if not en_json:
                    _pinta(f"\n[{a['tool']} necesita permiso: repite con --si para concederlo]\n")
            s.supervisor.resolve(a["id"], ok, True)

    def preguntar(texto: str) -> dict:
        usadas.clear()
        r = s.agents.run(args.agente, t.expandir(texto, sid), sid, emit=emit)
        return {"respuesta": r.get("answer", ""), "agente": r.get("agent"), "herramientas": list(usadas),
                "sesion": sid, "carpeta": t.sesion(sid).carpeta, "modo": t.modo(sid)}

    try:
        if not interactivo:
            r = preguntar(args.pregunta)
            if en_json:
                print(json.dumps(r, ensure_ascii=False, indent=1))
            else:
                print()
            return 0
        clase, _ = t.consola()
        print(f"Terminal IA · consola {clase} · modelo {s.llm.status()['backend']} · /ayuda para ver las órdenes")
        while True:
            try:
                texto = input(f"\n[{t.modo(sid)}] {t.sesion(sid).carpeta} ❯ ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not texto:
                continue
            bajo = texto.lower()
            if bajo in ("/salir", "salir", "exit", "quit"):
                break
            if bajo in ("/ayuda", "/help", "?"):
                print(AYUDA)
            elif texto.startswith("!"):
                r = t.ejecutar(texto[1:], sid, al_salir=_pinta)
                if r.get("error"):
                    print(r["error"])
                elif r.get("codigo"):
                    print(f"\n[código de salida {r['codigo']}]")
            elif bajo.startswith("/py "):
                r = t.codigo("python", texto[4:], sid)
                _pinta(r.get("salida") or "")
                print(r.get("error") or (r.get("valor") or ""))
            elif bajo.startswith("/modo"):
                partes = texto.split()
                try:
                    print("Modo:", t.modo(sid, partes[1] if len(partes) > 1 else None))
                except ValueError as e:
                    print(e)
            elif bajo.startswith("/carpeta"):
                try:
                    print(t.cambiar_carpeta(sid, texto[8:].strip() or "."))
                except FileNotFoundError as e:
                    print(e)
            elif bajo == "/tareas":
                marca = {"pendiente": "[ ]", "en_curso": "[~]", "hecha": "[x]"}
                print("\n".join(f"{marca[x['estado']]} {x['texto']}" for x in t.tareas(sid)) or "Sin tareas")
            elif bajo.startswith("/deshacer"):
                print(t.deshacer(sid, texto[9:].strip()))
            elif bajo == "/ordenes":
                carpeta = t.sesion(sid).carpeta
                for o in t.ordenes(carpeta).values():
                    print(f"/{o['nombre']}: {o['descripcion']}")
                for h in t.habilidades(carpeta):
                    print(f"habilidad {h['nombre']}: {h['descripcion']}")
            else:
                preguntar(texto)
                print()
        return 0
    finally:
        s.shutdown()
