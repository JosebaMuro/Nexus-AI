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
