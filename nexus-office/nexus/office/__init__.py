"""Perfiles de oficina: Nexus-Office configurado como la oficina de una pyme con agentes de su gremio.

Un perfil (``perfiles/<id>.yaml``) describe el objetivo de la instalacion -un taller de herreria, una calderería,
una empresa de reformas...- y elige del catalogo (``agentes/<id>.yaml``) los agentes que forman su oficina:
agentes de oficina (recepcion, administracion, presupuestos, compras, prevencion, personal, calidad, comercial) y
agentes especificos del gremio (herrero, calderero, soldador, albanil, fontanero, electricista...).

Los agentes del catalogo usan el mismo formato que ``nexus/agents/defaults`` (ver nexus/agents/manager.py) mas una
clave ``grafo``: el saber hacer inicial del agente como tripletas ``"sujeto | relacion | objeto [| categoria]"``.
Cada agente, incluidos los del gremio, aprende en su propio subgrafo aislado (``scope=<id>``, nexus/memory/graph.py):
al aplicar un perfil ese subgrafo se siembra con su ``grafo`` y a partir de ahi crece con sus conversaciones y los
documentos importados solo para el. El coordinador (el agente ``nexus``) pasa a ser la oficina de la empresa y su
conocimiento es el grafo general, comun a todos.

Aplicar un perfil no borra nada: los agentes que no forman parte de la oficina se desactivan (``enabled: false``)
y su memoria se conserva; antes de sobrescribir un YAML se guarda una copia en ``data/agents/respaldo``.
Todo es local (YAML + SQLite): funciona sin internet en Windows, macOS, Linux y Android.

    python -m nexus oficina                 lista los perfiles
    python -m nexus oficina herreria        aplica un perfil (los grafos se siembran al arrancar)
    python -m nexus oficina --quitar        vuelve a los agentes por defecto de Nexus-Office
"""
from __future__ import annotations

import copy
import datetime as dt
import re
import shutil
from pathlib import Path

import yaml

from .. import log, paths
from ..agents.manager import DEFAULTS_DIR, AgentSpec

L = log.get("office")
HERE = Path(__file__).resolve().parent
CATALOG_DIR = HERE / "agentes"
PROFILES_DIR = HERE / "perfiles"
OFFICE_ROLES = ("recepcion", "administracion", "presupuestos", "compras", "prl", "rrhh", "calidad", "comercial",
                "agenda", "proteccion-datos", "medioambiente")
# herramientas del coordinador: todas las que funcionan sin internet (las web quedan para quien las active a mano)
COORDINATOR_TOOLS = ["delegate", "list_agents", "memory_search", "memory_save", "knowledge_graph", "rag_search",
                     "rag_list", "calculator", "datetime_now", "file_read", "file_write", "file_list", "system_info"]
COORDINATOR_PROMPT = """Eres {{assistant_name}}, la oficina virtual de una pyme y coordinas a su equipo de agentes.
{context}
Atiendes al usuario (el dueño o el personal de la empresa) con trato cercano y profesional. Lo que es de un gremio
o de un departamento lo encargas con delegate al agente que corresponde y luego das una respuesta final ordenada;
lo sencillo lo resuelves tú. Calcula siempre con calculator. Si falta un dato (medidas, dirección, plazos), pídelo.
No inventes precios, normas ni plazos: si algo depende de la normativa vigente o de la gestoría, dilo."""


def _read(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def catalog() -> dict[str, dict]:
    """Agentes disponibles para las oficinas (id -> YAML con su clave ``grafo``)."""
    out = {}
    for f in sorted(CATALOG_DIR.glob("*.yaml")):
        d = _read(f)
        out[d["id"]] = d
    return out


def profiles() -> dict[str, dict]:
    out = {}
    for f in sorted(PROFILES_DIR.glob("*.yaml")):
        d = _read(f)
        out[d["id"]] = d
    return out


def seeds(agent: dict) -> list[tuple[str, str, str, str]]:
    """Tripletas del grafo inicial de un agente: (sujeto, relacion, objeto, categoria)."""
    out = []
    for item in agent.get("grafo") or []:
        parts = [p.strip() for p in (item.split("|") if isinstance(item, str) else item)]
        if len(parts) < 3 or not all(parts[:3]):
            continue
        rel = re.sub(r"\s+", "_", parts[1].lower())
        out.append((parts[0], rel, parts[2], parts[3] if len(parts) > 3 and parts[3] else "especialidad"))
    return out


def is_trade(aid: str) -> bool:
    return aid not in OFFICE_ROLES


# Modelos de IA especificos que conviene sugerir a cada sector (ademas del general elegido por hardware).
# Un perfil puede fijar los suyos con la clave «modelos: [{id: ..., motivo: ...}]». Los id son de
# nexus/models/recommended.py; la app los muestra en Modelos > Recomendados como «para tu oficina».
_HEALTH = [("medgemma-4b", "Terminología y documentación sanitaria (apoyo al profesional, no diagnostica)"),
           ("qwen3-embedding-0.6b", "Buscar en protocolos, prospectos y fichas de tu biblioteca")]
_DOCS = [("qwen3-vl-8b-thinking", "Razona mejor sobre normativa, contratos y expedientes largos"),
         ("qwen3-embedding-0.6b", "Buscar en tu archivo de documentos, leyes y plantillas")]
_IMAGE = [("sdxl-turbo", "Bocetos, maquetas y propuestas visuales para clientes"),
          ("sd-turbo", "Bocetos y propuestas visuales (equipos sin GPU potente)")]
SECTOR_MODELS: dict[str, list[tuple[str, str]]] = {
    **{pid: _HEALTH for pid in ("clinica-dental", "fisioterapia", "farmacia", "optica", "ayuda-domicilio-residencia",
                                "veterinaria")},
    **{pid: _DOCS for pid in ("asesoria-gestoria", "despacho-abogados", "inmobiliaria", "arquitectura-ingenieria",
                              "academia-formacion", "autoescuela")},
    **{pid: _IMAGE for pid in ("agencia-marketing", "imprenta-artes-graficas", "estudio-fotografia", "floristeria",
                               "pintura", "carpinteria", "textil-confeccion", "catering-eventos")},
    "servicios-informaticos": [("qwen2.5-coder-7b", "Programación, scripts, redes y configuración de equipos"),
                               ("qwen3-embedding-0.6b", "Buscar en manuales y documentación técnica")],
}


def sector_models(pid: str) -> dict[str, str]:
    """{id de modelo: motivo} sugeridos para el perfil ``pid`` ('' = ninguno)."""
    if not pid:
        return {}
    p = profiles().get(pid, {})
    own = [(m["id"], m.get("motivo", "")) for m in p.get("modelos", []) if isinstance(m, dict) and m.get("id")]
    return dict(own or SECTOR_MODELS.get(pid, []))


def describe(pid: str, active: str = "") -> dict:
    p, cat = profiles()[pid], catalog()
    agents = [{"id": a, "name": cat[a]["name"], "icon": cat[a].get("icon", "◆"), "color": cat[a].get("color", ""),
               "gremio": is_trade(a), "description": cat[a].get("description", "")} for a in p["agents"] if a in cat]
    return {"id": p["id"], "name": p["name"], "sector": p.get("sector", ""), "description": p.get("description", ""),
            "icon": p.get("icon", "◆"), "assistant_name": p.get("assistant_name", ""), "agents": agents,
            "active": pid == active, "models": sector_models(pid)}


def coordinator(profile: dict, agent_names: dict[str, str]) -> dict:
    base = _read(DEFAULTS_DIR / "nexus.yaml")
    ctx = (profile.get("coordinator") or {}).get("context", "").strip()
    team = "; ".join(f"{a} ({agent_names.get(a, a)})" for a in profile["agents"])
    base.update({
        "name": profile.get("assistant_name") or base["name"],
        "description": f"Oficina de la empresa ({profile['name']}); coordina a su equipo",
        "system_prompt": COORDINATOR_PROMPT.format(context=ctx) + f"\nTu equipo: {team}.\n",
        "temperature": 0.5,
        "tools": list(COORDINATOR_TOOLS),
        "can_delegate_to": list(profile["agents"]),
        "icon": profile.get("icon", base.get("icon", "◆")),
        "enabled": True,
    })
    return base


class Office:
    """Aplica perfiles sobre ``data/agents`` y ``data/config.yaml`` y siembra el grafo de cada agente."""

    def __init__(self, config, agents_dir: Path | None = None) -> None:
        self.config = config
        self.dir = agents_dir or paths.agents_dir()

    # ------------------------------------------------------------ consulta
    def active(self) -> str:
        return self.config.get("office.profile", "") or ""

    def list(self) -> list[dict]:
        return [describe(pid, self.active()) for pid in profiles()]

    # ------------------------------------------------------------ escritura
    def _backup(self, f: Path) -> None:
        if f.exists():
            dst = self.dir / "respaldo"
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy(f, dst / f"{f.stem}-{dt.datetime.now():%Y%m%d-%H%M%S}.yaml")

    def _write(self, data: dict) -> None:
        f = self.dir / f"{data['id']}.yaml"
        spec = AgentSpec.from_dict(data)       # mismo formato y validacion que AgentManager.save
        from dataclasses import asdict
        txt = yaml.safe_dump(asdict(spec), allow_unicode=True, sort_keys=False)
        if f.exists() and f.read_text(encoding="utf-8") == txt:
            return
        self._backup(f)
        f.write_text(txt, encoding="utf-8")

    def _set_enabled(self, f: Path, enabled: bool) -> None:
        d = _read(f)
        if d.get("enabled", True) != enabled:
            d["enabled"] = enabled
            f.write_text(yaml.safe_dump(d, allow_unicode=True, sort_keys=False), encoding="utf-8")

    def apply(self, pid: str, reset_agents: bool = False) -> dict:
        """Configura la oficina del perfil. ``reset_agents``: reescribe tambien los agentes ya personalizados."""
        allp = profiles()
        if pid not in allp:
            raise ValueError(f"perfil desconocido: {pid}. Disponibles: {', '.join(allp)}")
        profile, cat = allp[pid], catalog()
        missing = [a for a in profile["agents"] if a not in cat]
        if missing:
            raise ValueError(f"el perfil {pid} usa agentes que no estan en el catalogo: {', '.join(missing)}")
        team = set(profile["agents"])
        for f in DEFAULTS_DIR.glob("*.yaml"):   # instalacion nueva: se siembran ya para poder desactivarlos
            if not (self.dir / f.name).exists():
                shutil.copy(f, self.dir / f.name)
        for f in self.dir.glob("*.yaml"):   # fuera de la oficina: se desactivan (se conservan con su memoria)
            if f.stem not in team and f.stem != "nexus":
                self._set_enabled(f, False)
        for aid in profile["agents"]:
            f = self.dir / f"{aid}.yaml"
            if f.exists() and not reset_agents:
                self._set_enabled(f, True)   # el usuario lo pudo personalizar: solo se activa
            else:
                self._write({k: v for k, v in copy.deepcopy(cat[aid]).items() if k != "grafo"} | {"enabled": True})
        self._write(coordinator(profile, {a: cat[a]["name"] for a in team}))
        self.config.set("user.assistant_name", profile.get("assistant_name") or "Nexus-Office", save=False)
        self.config.set("user.default_agent", "auto", save=False)
        self.config.set("office.profile", pid)
        L.info("Perfil de oficina aplicado: %s (%d agentes)", pid, len(team))
        return describe(pid, pid)

    def clear(self) -> None:
        """Quita el perfil: vuelven los agentes por defecto de Nexus-Office y se desactivan los de la oficina."""
        cat = catalog()
        for f in self.dir.glob("*.yaml"):
            if f.stem in cat:
                self._set_enabled(f, False)
        for f in DEFAULTS_DIR.glob("*.yaml"):
            dst = self.dir / f.name
            if f.stem == "nexus" or not dst.exists():
                if dst.exists() and dst.read_text(encoding="utf-8") != f.read_text(encoding="utf-8"):
                    self._backup(dst)
                shutil.copy(f, dst)
            else:
                self._set_enabled(dst, True)
        self.config.set("user.assistant_name", "Nexus-Office", save=False)
        self.config.set("office.profile", "")

    # ------------------------------------------------------------ grafos
    def seed_graphs(self, graph, agent_ids: list[str]) -> dict[str, int]:
        """Siembra el subgrafo propio de cada agente del catalogo que aun no lo tenga (idempotente: la marca es la
        relacion «domina» que une el ancla del agente con su saber hacer)."""
        cat, out = catalog(), {}
        for aid in agent_ids:
            if aid not in cat or graph.db.one("SELECT 1 FROM relations WHERE scope=? AND rel='domina' LIMIT 1", (aid,)):
                continue
            g = graph.for_agent(aid)
            n = 0
            with graph.db.transaction():
                for s, rel, o, category in seeds(cat[aid]):
                    if g.relate(s, rel, o, category=category, src_category=category, weight=1.5):
                        g.relate(g.self_name, "domina", s, src_type="agente", category=category, weight=0.5)
                        n += 1
            out[aid] = n
        if out:
            L.info("Grafos sembrados: %s", ", ".join(f"{a} ({n})" for a, n in out.items()))
        return out

def ensure(services) -> dict[str, int]:
    """Al arrancar: siembra el grafo de los agentes de la oficina activa que aun no se hayan sembrado."""
    office = Office(services.config)
    if not office.active():
        return {}
    return office.seed_graphs(services.graph, services.agents.ids())
