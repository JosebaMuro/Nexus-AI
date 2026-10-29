"""Colaboracion entre agentes y evolucion del repositorio de herramientas."""
from __future__ import annotations


def delegate(args, ctx):
    if ctx.depth >= 2:
        return {"error": "profundidad maxima de delegacion alcanzada"}
    agents = ctx.services.agents
    spec = agents.get(ctx.agent)
    target = args["agent"]
    if spec and spec.can_delegate_to and target not in spec.can_delegate_to and "*" not in spec.can_delegate_to:
        return {"error": f"{ctx.agent} no puede delegar en {target}"}
    if not agents.get(target):
        return {"error": f"agente desconocido: {target}. Disponibles: {', '.join(agents.ids())}"}
    answer = agents.run_subtask(target, args["task"], ctx)
    return {"agent": target, "answer": answer}


def consultar_profesor(args, ctx):
    """Pregunta al Profesor (IA online si hay clave, si no el modelo local) y aprende de su respuesta."""
    if ctx.agent == "profesor":
        return {"error": "el Profesor no se consulta a si mismo"}
    if ctx.depth >= 2:
        return {"error": "profundidad maxima de delegacion alcanzada"}
    agents = ctx.services.agents
    if not agents.get("profesor"):
        return {"error": "no hay agente Profesor (restauralo en Agentes)"}
    pregunta = str(args.get("pregunta", "")).strip()
    if not pregunta:
        return {"error": "falta la pregunta"}
    tarea = pregunta + (f"\n\nContexto: {args['contexto']}" if args.get("contexto") else "")
    answer = agents.run_subtask("profesor", tarea, ctx)
    out = {"agent": "profesor", "answer": answer, "online": ctx.services.llm.backends["nube"].ready()}
    if args.get("guardar", True) and answer.strip():
        # lo aprendido queda en la memoria y el conocimiento del agente que pregunto
        lesson = f"Lección del Profesor sobre «{pregunta[:160]}»: {answer.strip()[:1200]}"
        out["memoria"] = ctx.services.memory.remember(lesson, "leccion", 0.75, source="agente:profesor",
                                                     agent=ctx.agent)
        ctx.services.learner.learn_text(lesson, ctx.agent)
    return out


def propose_tool(args, ctx):
    if not ctx.services.config.get("tools.allow_agent_proposals", True):
        return {"error": "las propuestas de herramientas estan desactivadas"}
    prop = ctx.services.proposals.create(
        name=args["name"], description=args["description"], code=args["code"],
        parameters=args.get("parameters") or {"type": "object", "properties": {}},
        risk=args.get("risk", "medium"), rationale=args.get("rationale", ""), author=f"agente:{ctx.agent}")
    return {"proposal_id": prop["id"], "status": prop["status"], "analysis": prop["analysis"],
            "note": "La herramienta quedara disponible cuando un administrador la apruebe."}


def list_agents(args, ctx):
    return {"agents": [{"id": a.id, "name": a.name, "description": a.description} for a in ctx.services.agents.list()]}


TOOLS = [
    ({"name": "delegate", "description": "Encarga una subtarea a otro agente especialista y devuelve su respuesta",
      "parameters": {"type": "object", "properties": {"agent": {"type": "string"}, "task": {"type": "string"}},
                     "required": ["agent", "task"]}, "risk": "low", "permissions": ["agents"], "timeout": 900}, delegate),
    ({"name": "consultar_profesor",
      "description": ("Pregunta al agente Profesor (usa una IA online como Gemini, ChatGPT, Perplexity o Claude si "
                      "un administrador la ha configurado; si no, el modelo local) y guarda la leccion en tu memoria "
                      "para la proxima vez. Para dudas de conocimiento que no sabes resolver bien"),
      "parameters": {"type": "object", "properties": {
          "pregunta": {"type": "string"}, "contexto": {"type": "string", "description": "lo que ya sabes o intentas hacer"},
          "guardar": {"type": "boolean", "default": True}}, "required": ["pregunta"]},
      "risk": "low", "permissions": ["agents"], "timeout": 900}, consultar_profesor),
    ({"name": "list_agents", "description": "Lista los agentes disponibles y su especialidad",
      "parameters": {"type": "object", "properties": {}}, "risk": "low", "permissions": ["agents"]}, list_agents),
    ({"name": "propose_tool", "description": ("Propone una NUEVA herramienta para el repositorio. 'code' debe definir "
                                              "TOOL = {...} y def run(args, ctx). Queda pendiente de revision humana."),
      "parameters": {"type": "object", "properties": {
          "name": {"type": "string"}, "description": {"type": "string"}, "code": {"type": "string"},
          "parameters": {"type": "object"}, "risk": {"type": "string", "enum": ["low", "medium", "high"]},
          "rationale": {"type": "string"}}, "required": ["name", "description", "code"]},
      "risk": "low", "permissions": ["agents"]}, propose_tool),
]
