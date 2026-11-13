"""Supervision de herramientas: politica por riesgo, aprobaciones humanas, limites de tiempo y registro de
auditoria. Todas las herramientas (integradas y de la comunidad) se ejecutan en este proceso, sin aislamiento.

Politicas por nivel de riesgo: ``auto`` (se ejecuta), ``ask`` (el usuario aprueba en la UI),
``admin`` (solo un administrador puede aprobar) y ``deny`` (bloqueada).
"""
from __future__ import annotations

import concurrent.futures
import json
import threading
import time
import uuid
from .. import log
from .base import ToolContext, validate_args
from .registry import ToolRegistry

L = log.get("supervisor")
_POOL = concurrent.futures.ThreadPoolExecutor(max_workers=4, thread_name_prefix="nexus-tool")
STRICTER = {"auto": "ask", "ask": "admin", "admin": "admin", "deny": "deny"}


class Supervisor:
    def __init__(self, registry: ToolRegistry, db, config) -> None:
        self.registry, self.db, self.config = registry, db, config
        self.pending: dict[str, dict] = {}
        self._lock = threading.Lock()
        self.listeners: list = []      # callbacks(event) para notificar a todas las UIs
        self.assist = None             # IA de apoyo (nexus.memory.assist): corrige nombres y valores mal escritos

    def decision_for(self, risk: str, remote: bool) -> str:
        policy = self.config.get("tools.policy", {}).get(risk, "admin")
        if remote and risk != "low":
            policy = STRICTER[policy]   # peticiones de otros nodos: un escalon mas estricto
        return policy

    # ------------------------------------------------------------ aprobaciones
    def _await_approval(self, call: dict, ctx: ToolContext, level: str, timeout: float = 180) -> tuple[bool, str]:
        pid = uuid.uuid4().hex[:10]
        ev = threading.Event()
        item = {"id": pid, "tool": call["tool"], "args": call["args"], "risk": call["risk"], "agent": ctx.agent,
                "session_id": ctx.session_id, "level": level, "remote": ctx.remote, "created": time.time(),
                "event": ev, "approved": None, "approver": ""}
        with self._lock:
            self.pending[pid] = item
        event = {"type": "approval_required", "approval": self.public(item)}
        if ctx.emit:
            ctx.emit(event)
        for cb in list(self.listeners):
            try:
                cb(event)
            except Exception:  # noqa: BLE001
                pass
        ev.wait(timeout)
        with self._lock:
            self.pending.pop(pid, None)
        if item["approved"] is None:
            return False, "timeout"
        return bool(item["approved"]), item["approver"]

    def resolve(self, pid: str, approve: bool, is_admin: bool) -> bool:
        with self._lock:
            item = self.pending.get(pid)
        if not item:
            return False
        if item["level"] == "admin" and not is_admin:
            raise PermissionError("Esta accion requiere aprobacion de un administrador")
        item["approved"] = approve
        item["approver"] = "admin" if is_admin else "usuario"
        item["event"].set()
        return True

    @staticmethod
    def public(item: dict) -> dict:
        return {k: v for k, v in item.items() if k != "event"}

    def list_pending(self) -> list[dict]:
        with self._lock:
            return [self.public(i) for i in self.pending.values()]

    # ------------------------------------------------------------ ejecucion
    def execute(self, name: str, args: dict, ctx: ToolContext) -> dict:
        spec = self.registry.get(name)
        t0 = time.time()
        fixed_note = []
        if (not spec or not spec.enabled) and self.assist is not None:
            # el modelo pidio una herramienta que no existe ("search_web"): la equivalente si esta clara
            enabled = [t for t in self.registry.tools.values() if t.enabled]
            alt = self.assist.fix_tool_name(name, enabled)
            if alt:
                fixed_note.append(f"herramienta {name} -> {alt}")
                name, spec = alt, self.registry.get(alt)
        if not spec or not spec.enabled:
            return {"error": f"herramienta desconocida o desactivada: {name}"}
        if self.assist is not None:
            args, changes = self.assist.repair_args(spec.parameters, args or {})
            fixed_note += changes
        clean, errors = validate_args(spec.parameters, args or {})
        if errors:
            return {"error": "; ".join(errors)}
        decision = self.decision_for(spec.risk, ctx.remote)
        approver = "politica"
        call = {"tool": name, "args": clean, "risk": spec.risk}
        if decision == "deny":
            self._audit(ctx, call, "deny", approver, "bloqueada", "", 0)
            return {"error": f"'{name}' esta bloqueada por la politica de seguridad"}
        if decision in ("ask", "admin"):
            ok, approver = self._await_approval(call, ctx, decision)
            if not ok:
                self._audit(ctx, call, decision, approver, "rechazada", "", 0)
                return {"error": f"El uso de '{name}' no fue aprobado ({approver or 'rechazado'})"}
        timeout = spec.timeout or float(self.config.get("tools.timeout_s", 30))
        try:
            if spec.func is None:
                raise RuntimeError(f"la herramienta {name} no tiene funcion run")
            result = self.run_in_process(spec.func, clean, ctx, timeout)
            status = "ok"
        except concurrent.futures.TimeoutError:
            result, status = {"error": f"tiempo agotado ({timeout:.0f}s)"}, "timeout"
        except Exception as e:  # noqa: BLE001
            result, status = {"error": f"{type(e).__name__}: {e}"}, "error"
            L.exception("Herramienta %s fallo", name)
        if isinstance(result, dict) and result.get("error") and status == "ok":
            status = "error"
        self._audit(ctx, call, decision, approver, status, result, time.time() - t0)
        result = result if isinstance(result, dict) else {"result": result}
        if fixed_note:
            result = {**result, "_corregido": "; ".join(fixed_note)}
        return result

    @staticmethod
    def run_in_process(func, args: dict, ctx, timeout: float):
        """Ejecuta una funcion de herramienta en el grupo de hilos con limite de tiempo."""
        return _POOL.submit(func, args, ctx).result(timeout=timeout)

    def _audit(self, ctx: ToolContext, call: dict, decision: str, approver: str, status: str, result, dur: float):
        preview = json.dumps(result, ensure_ascii=False, default=str)[:500] if result else ""
        self.db.execute("INSERT INTO tool_calls(ts, agent, session_id, tool, args, risk, decision, approver, status, "
                        "result, duration) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                        (time.time(), ctx.agent + (" (remoto)" if ctx.remote else ""), ctx.session_id, call["tool"],
                         json.dumps(call["args"], ensure_ascii=False)[:1000], call["risk"], decision, approver,
                         status, preview, round(dur, 3)))

    def audit_log(self, limit: int = 100) -> list[dict]:
        return self.db.query("SELECT * FROM tool_calls ORDER BY id DESC LIMIT ?", (limit,))
