"""Herramientas basicas de bajo riesgo: calculadora segura, fecha/hora, memoria, grafo y sistema."""
from __future__ import annotations

import ast
import datetime as dt
import math
import operator

_OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv,
        ast.Pow: operator.pow, ast.Mod: operator.mod, ast.FloorDiv: operator.floordiv, ast.USub: operator.neg,
        ast.UAdd: operator.pos}
_FUNCS = {k: getattr(math, k) for k in ("sqrt", "sin", "cos", "tan", "log", "log10", "log2", "exp", "floor", "ceil",
                                        "factorial", "asin", "acos", "atan", "radians", "degrees")}
_FUNCS.update(abs=abs, round=round, min=min, max=max)
_CONSTS = {"pi": math.pi, "e": math.e, "tau": math.tau}


def _eval(node):
    if isinstance(node, ast.Expression):
        return _eval(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
        return node.value
    if isinstance(node, ast.BinOp) and type(node.op) in _OPS:
        left, right = _eval(node.left), _eval(node.right)
        if isinstance(node.op, ast.Pow) and abs(right) > 1000:
            raise ValueError("exponente demasiado grande")
        return _OPS[type(node.op)](left, right)
    if isinstance(node, ast.UnaryOp) and type(node.op) in _OPS:
        return _OPS[type(node.op)](_eval(node.operand))
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in _FUNCS:
        return _FUNCS[node.func.id](*[_eval(a) for a in node.args])
    if isinstance(node, ast.Name) and node.id in _CONSTS:
        return _CONSTS[node.id]
    raise ValueError("expresion no permitida")


def calculator(args, ctx):
    expr = args["expression"].replace("^", "**").replace(",", ".").replace("×", "*").replace("÷", "/")
    result = _eval(ast.parse(expr, mode="eval"))
    if isinstance(result, float) and result.is_integer() and abs(result) < 1e15:
        result = int(result)
    elif isinstance(result, float):
        result = round(result, 10)
    return {"expression": args["expression"], "result": result}


def datetime_now(args, ctx):
    now = dt.datetime.now()
    days = ["lunes", "martes", "miercoles", "jueves", "viernes", "sabado", "domingo"]
    return {"date": now.strftime("%d/%m/%Y"), "time": now.strftime("%H:%M"), "weekday": days[now.weekday()],
            "iso": now.isoformat(timespec="seconds")}


def memory_search(args, ctx):
    s = ctx.services
    items = s.memory.recall(args["query"], k=int(args.get("k", 6)), agent=ctx.agent or None)
    results = [{"content": i["content"], "kind": i["kind"], "score": i["score"]} for i in items]
    if "usuario" in args["query"].lower() or args["query"].lower() in ("yo", "mi", "me"):
        known = " ".join(r["content"].lower() for r in results)
        for f in s.graph.user_facts(8):  # hechos del grafo que la memoria aun no contiene
            if f.split(" ", 3)[-1].lower() not in known:
                results.append({"content": f, "kind": "grafo"})
    return {"results": results}


def memory_save(args, ctx):
    mid = ctx.services.memory.remember(args["content"], args.get("kind", "note"), float(args.get("importance", 0.7)),
                                       source=f"agente:{ctx.agent}")
    # datos del usuario, pautas y relaciones al general; los temas, al especialista que corresponda (o comunes)
    ctx.services.learner.learn_text(args["content"], ctx.agent)
    return {"saved": True, "id": mid}


def knowledge_graph(args, ctx):
    g = ctx.services.graph
    entity = args.get("entity", "")
    if entity:   # lo propio del agente y lo comun
        own = g.for_agent(ctx.agent).facts_about([entity], 12) if ctx.agent else []
        own_notes = g.for_agent(ctx.agent).notes_about([entity], 3) if ctx.agent else []
        return {"facts": list(dict.fromkeys(own + g.facts_about([entity], 20)))[:20],
                "attributes": own_notes + g.general().notes_about([entity], 3)}
    return g.stats()


def graph_import(args, ctx):
    """Incorpora al conocimiento un grafo de cualquier formato: un fichero del sandbox o un texto que trae el agente."""
    from ...memory import kg_io
    from .sandbox_tools import _safe
    s = ctx.services
    if args.get("path"):
        p = _safe(args["path"])
        if not p.is_file():
            raise ValueError(f"no existe {args['path']} en el sandbox")
        if p.stat().st_size > 300 * 1024 * 1024:
            raise ValueError("el grafo supera 300 MB")
        data, name = p.read_bytes(), p.name
    elif args.get("content"):
        data, name = args["content"].encode("utf-8"), args.get("name") or "grafo.txt"
    else:
        raise ValueError("indica path (fichero del sandbox) o content (el grafo en texto)")
    target = args.get("target", "general")
    target = (ctx.agent or "") if target == "agente" else "auto" if target == "auto" else ""
    parsed = kg_io.parse_bytes(data, name)
    res = kg_io.insert_graph(s.graph, parsed, target, s.organizer, s.agents.ids(), f"grafo:{name}")
    if ctx.emit:
        ctx.emit({"type": "graph_update"})
    return res


def system_info(args, ctx):
    s = ctx.services
    return {"profile": s.profile, "live": s.monitor.live()["current"], "llm": s.llm.status(),
            "tuning": s.tuning_reasons}


TOOLS = [
    ({"name": "calculator", "description": "Evalua expresiones matematicas de forma segura (sqrt, sin, log, pi...)",
      "parameters": {"type": "object", "properties": {"expression": {"type": "string"}}, "required": ["expression"]},
      "risk": "low", "permissions": []}, calculator),
    ({"name": "datetime_now", "description": "Fecha y hora actuales", "parameters": {"type": "object", "properties": {}},
      "risk": "low", "permissions": []}, datetime_now),
    ({"name": "memory_search", "description": "Busca en la memoria a largo plazo lo que se sabe sobre algo o sobre el usuario",
      "parameters": {"type": "object", "properties": {"query": {"type": "string"}, "k": {"type": "integer", "default": 6}},
                     "required": ["query"]}, "risk": "low", "permissions": ["memory"]}, memory_search),
    ({"name": "memory_save", "description": "Guarda un dato importante en la memoria a largo plazo",
      "parameters": {"type": "object", "properties": {"content": {"type": "string"},
                                                      "kind": {"type": "string", "enum": ["fact", "preference", "note"], "default": "note"}},
                     "required": ["content"]}, "risk": "low", "permissions": ["memory"]}, memory_save),
    ({"name": "knowledge_graph", "description": "Consulta el grafo de conocimiento (hechos sobre una entidad)",
      "parameters": {"type": "object", "properties": {"entity": {"type": "string"}}}, "risk": "low",
      "permissions": ["memory"]}, knowledge_graph),
    ({"name": "graph_import",
      "description": ("Importa como conocimiento un grafo de cualquier formato (JSON, CSV, GraphML, GEXF, GML, DOT, RDF, "
                      "Cypher de Neo4j, Mermaid, Excel, SQLite, ZIP...): se detecta solo, se fusiona con lo que ya se "
                      "sabe y queda disponible para todos los agentes o solo para ti"),
      "parameters": {"type": "object", "properties": {
          "path": {"type": "string", "description": "fichero dentro del sandbox"},
          "content": {"type": "string", "description": "o el grafo en texto (p. ej. 'A -> B : relacion' por linea)"},
          "name": {"type": "string", "description": "nombre con extension para orientar el formato del texto"},
          "target": {"type": "string", "enum": ["general", "agente", "auto"], "default": "general",
                     "description": "general: para todos; agente: solo tu grafo; auto: cada nodo a su especialista"}}},
      "risk": "medium", "permissions": ["memory"], "timeout": 600}, graph_import),
    ({"name": "system_info", "description": "Perfil de hardware, rendimiento en vivo y backend de IA activo",
      "parameters": {"type": "object", "properties": {}}, "risk": "low", "permissions": ["system"]}, system_info),
]
