"""Motor integrado sin modelo: permite usar Nexus-Office (herramientas, memoria, imagen, musica, voz)
antes de descargar un LLM. Detecta intenciones con reglas y emite llamadas a herramientas
con el mismo protocolo que un LLM real, asi todo el pipeline queda ejercitado.
"""
from __future__ import annotations

import json
import re
import time

from .backends import Backend
from .protocol import RESULT_RE

MATH = re.compile(r"^[\d\s\.\,\+\-\*/\^\(\)%a-z_]*\d[\d\s\.\,\+\-\*/\^\(\)%a-z_]*$", re.I)
URL = re.compile(r"https?://\S+")


def _call(name: str, **args) -> str:
    return f'<tool>{json.dumps({"name": name, "args": args}, ensure_ascii=False)}</tool>'


IMG_RE = re.compile(r"\b(?:genera|crea|dibuja|pinta|haz|hazme|draw|generate|create|make)\b.*?\b(?:imagen|dibujo|ilustraci[oó]n|cuadro|foto|image|picture)\b(?:\s+(?:de|del|sobre|of|with|con))?\s*(.*)", re.I | re.S)
MUS_RE = re.compile(r"\b(?:comp[oó]n|componme|crea|genera|haz|hazme|toca|compose|make|generate)\b.*?\b(?:m[uú]sica|canci[oó]n|melod[ií]a|tema musical|music|song|melody|beat)\b\s*(.*)", re.I | re.S)


DOC_RE = re.compile(r"\b(pdf|documento|informe|dossier|extrae|extraer|procesa|procesar|archiva|archivar|guarda|guardar|maqueta|resume|resumir|aprende|estudia)\b", re.I)


CRAWL_RE = re.compile(r"\b(rastrea|rastrear|rastreo|crawl\w*|ara[ñn]a|spider|indexa (?:toda|todo|el sitio|el dominio|la documentaci[oó]n)|"
                      r"todo el (?:dominio|sitio)|toda la documentaci[oó]n|documentaci[oó]n (?:completa|entera)|"
                      r"aprende de (?:la|esta) documentaci[oó]n|lee (?:toda|la) documentaci[oó]n)\b", re.I)


def detect_intent(text: str, tools: set[str]) -> tuple[str, dict] | None:
    """Intenciones inequivocas que se resuelven con una herramienta sin esperar al LLM.
    Precision alta a proposito: ante la duda, decide el modelo."""
    low = text.lower().strip()
    url = URL.search(text)
    if "crawl_docs" in tools and url and CRAWL_RE.search(text):
        args = {"url": url.group(0).rstrip(".,;)"),
                "scope": "domain" if re.search(r"\b(todo el (dominio|sitio|site)|dominio completo|whole (site|domain))\b", low) else "path"}
        if args["scope"] == "domain" or re.search(r"(toda la documentaci[oó]n|documentaci[oó]n (completa|entera)|entera\b|completa\b)", low):
            args.update(max_pages=100_000, max_depth=50, background=True)   # sitio entero: por lotes, en segundo plano
        if re.search(r"\b(sigue|segu[ií]|contin[uú]a|continuar|reanuda|reanudar)\b", low):
            args["continue"] = True
        return "crawl_docs", args
    if "url_to_pdf" in tools and url and DOC_RE.search(text):
        return "url_to_pdf", {"url": url.group(0).rstrip(".,;)")}
    expr = re.sub(r"^(cu[aá]nto es|cu[aá]nto da|calcula|calculate|what is|resuelve)\s*", "", low).strip(" ?¿=")
    if "calculator" in tools and expr and MATH.match(expr) and re.search(r"[\+\-\*/\^%]|sqrt|sin|cos|log", expr):
        return "calculator", {"expression": expr}
    if "datetime_now" in tools and re.search(r"\b(qu[eé] hora es|qu[eé] d[ií]a es hoy|qu[eé] fecha es|what time is it)\b", low):
        return "datetime_now", {}
    m = IMG_RE.search(text)
    if "generate_image" in tools and m:
        return "generate_image", {"prompt": m.group(1).strip(" .") or text}
    m = MUS_RE.search(text)
    if "compose_music" in tools and m:
        return "compose_music", {"prompt": m.group(1).strip(" .") or text}
    return None


class BuiltinBackend(Backend):
    name = "builtin"

    def available(self) -> bool:
        return True

    def models(self) -> list[str]:
        return ["nexus-core (reglas, sin LLM)"]

    def stream_chat(self, messages, model="", **opts):
        reply = self._respond(messages, set(opts.get("tools") or []))

        def gen():
            for word in re.split(r"(\s+)", reply):
                if word:
                    yield word
                    time.sleep(0.004)

        yield from self._timed(gen())

    # ------------------------------------------------------------------ reglas
    def _respond(self, messages: list[dict], tools: set[str]) -> str:
        system = messages[0]["content"] if messages and messages[0]["role"] == "system" else ""
        last = messages[-1]["content"] if messages else ""
        m = RESULT_RE.search(last)
        if m:
            return self._after_tool(m.group(1), m.group(2))
        text = last.strip()
        low = text.lower()
        name = re.search(r"[Uu]suario se llama ([^\n.]+)", system)
        user = name.group(1).strip() if name else ""

        intent = detect_intent(text, tools)
        if intent:
            return _call(intent[0], **intent[1])
        if "datetime_now" in tools and re.search(r"\b(hora|qu[eé] d[ií]a|fecha|what time|date today)\b", low):
            return _call("datetime_now")
        if "web_fetch" in tools and URL.search(text):
            return _call("web_fetch", url=URL.search(text).group(0))
        if "memory_search" in tools and re.search(r"\b(qu[eé] sabes|qu[eé] recuerdas|recuerdas|what do you know|remember about)\b", low):
            q = re.sub(r".*?(sabes|recuerdas|know|remember)\s*(de|sobre|about)?\s*", "", low).strip(" ?¿") or "usuario"
            return _call("memory_search", query="usuario" if q in ("mi", "mí", "me") else q)
        if "system_info" in tools and re.search(r"\b(hardware|rendimiento|sistema|gpu|npu|cpu|memoria ram|performance)\b", low):
            return _call("system_info")
        if "web_search" in tools and re.match(r"^(busca|buscar|search|investiga)\b", low):
            return _call("web_search", query=re.sub(r"^(busca|buscar|search|investiga)\s*(en internet|online)?\s*", "", low))

        if re.match(r"^(hola|buenas|hey|hello|hi|saludos)\b", low):
            who = f", {user}" if user else ""
            return (f"¡Hola{who}! Soy Nexus-Office. Ahora mismo funciono con mi núcleo integrado (sin modelo de lenguaje), "
                    "pero ya puedo calcular, decirte la hora, generar imágenes y música, recordar lo que me cuentas "
                    "y mostrarte mi grafo de conocimiento. Para conversar con libertad, descarga un modelo en "
                    "**Modelos** (te recomiendo uno adaptado a tu hardware).")
        if re.match(r"^(recuerda|apunta|no olvides|remember)\b", low) or re.search(r"\b(me llamo|me gusta|vivo en|trabajo)\b", low):
            return "Entendido, lo he guardado en mi memoria y en mi grafo de conocimiento. 🧠"
        if "Fragmentos de tu biblioteca" in system:  # RAG sin LLM: se citan los fragmentos mas relevantes
            frags = re.findall(r"^\[(\d+)\] «([^»]+)» p\.(\d+): (.+)$", system.split("Fragmentos de tu biblioteca")[-1], re.M)
            if frags:
                return "Esto es lo que dice mi biblioteca de documentos:\n\n" + "\n\n".join(
                    f"> {t[:350]}…\n> — *{title}*, p. {p} [{n}]" for n, title, p, t in frags[:3])
        mems = re.findall(r"^- (.+)$", system.split("Recuerdos relevantes:")[-1], re.M) if "Recuerdos relevantes:" in system else []
        if mems:
            listed = "\n".join(f"- {x}" for x in mems[:4])
            return f"Sin un modelo de lenguaje no puedo razonar libremente, pero esto es lo que recuerdo relacionado:\n{listed}"
        return ("Todavía no tengo un modelo de lenguaje cargado, así que mis respuestas abiertas son limitadas. "
                "Prueba: *«cuánto es 2^10 / 3»*, *«genera una imagen de una ciudad de neón»*, "
                "*«compón una música épica»*, *«qué sabes de mí»* o *«info del sistema»*. "
                "Desde **Modelos** puedes descargar un LLM recomendado para tu equipo.")

    @staticmethod
    def _after_tool(name: str, body: str) -> str:
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            data = body
        if isinstance(data, dict) and data.get("error"):
            return f"La herramienta **{name}** no pudo completarse: {data['error']}"
        if name == "calculator":
            return f"El resultado es **{data.get('result')}**."
        if name == "datetime_now":
            return f"Ahora son las **{data.get('time')}** del {data.get('weekday')} {data.get('date')}."
        if name == "generate_image":
            return f"Aquí tienes la imagen ({data.get('engine')}):\n\n![imagen]({data.get('url')})"
        if name == "compose_music":
            return (f"He compuesto una pieza en **{data.get('key')}**, {data.get('tempo')} BPM, estilo "
                    f"*{data.get('mood')}*:\n\n[audio]({data.get('url')})")
        if name == "memory_search":
            items = data.get("results", []) if isinstance(data, dict) else []
            if not items:
                return "Aún no tengo recuerdos sobre eso. Cuéntame más y lo aprenderé."
            return "Esto es lo que recuerdo:\n" + "\n".join(f"- {i['content']}" for i in items[:6])
        if name == "system_info":
            p = data.get("profile", {})
            return (f"Perfil **{p.get('id')}** ({p.get('tier')}): acelerador `{p.get('accelerator')}` en "
                    f"{p.get('device_name')}. RAM {p.get('ram_gb')} GB. Modelo recomendado ≤ "
                    f"{p.get('llm', {}).get('recommended_model_b')}B, contexto {p.get('llm', {}).get('n_ctx')}, "
                    f"{p.get('llm', {}).get('n_threads')} hilos. NPU: {'sí' if p.get('npu', {}).get('present') else 'no'}.")
        if name == "url_to_pdf":
            pts = "\n".join(f"- {p}" for p in data.get("key_points", [])[:5])
            return (f"He procesado **{data.get('title')}** y lo he maquetado en un PDF de {data.get('pages')} páginas "
                    f"({data.get('sections')} secciones, {data.get('words')} palabras, {data.get('images_embedded')} imágenes).\n\n"
                    f"[📄 {data.get('title')}.pdf]({data.get('url')})\n\n**Resumen:** {data.get('summary')}\n\n"
                    + (f"**Puntos clave:**\n{pts}\n\n" if pts else "")
                    + f"Lo he archivado en mi biblioteca RAG ({data.get('rag_chunks')} fragmentos) y he incorporado "
                      f"{data.get('learned')} elementos a mi conocimiento.")
        if name == "crawl_docs" and data.get("status") == "en curso":
            return ("He empezado a rastrear el sitio completo en segundo plano, por lotes correlativos. Cada lote se "
                    "añade a mi biblioteca RAG en cuanto termina (lo verás en *Biblioteca → Rastreos por lotes*), así que "
                    "ya puedes preguntarme por lo que vaya leyendo.")
        if name == "crawl_docs":
            pts = "\n".join(f"- {p}" for p in data.get("key_points", [])[:5])
            titles = ", ".join(f"*{t}*" for t in data.get("page_titles", [])[:8])
            return (f"He rastreado **{data.get('site')}** desde {data.get('start')} (ámbito `{data.get('prefix')}`): "
                    f"{data.get('pages')} páginas leídas ({data.get('words')} palabras), {data.get('skipped_total')} omitidas"
                    + (f" y {data.get('pending')} pendientes por el límite" if data.get("pending") else "") + ".\n\n"
                    + (f"Páginas: {titles}…\n\n" if titles else "")
                    + f"**Resumen:** {data.get('summary')}\n\n" + (f"**Puntos clave:**\n{pts}\n\n" if pts else "")
                    + (f"Lo he procesado en {data.get('batches')} lotes ordenados"
                       + (f"; quedan páginas pendientes y puedo seguir por el siguiente lote cuando quieras" if data.get("status") == "pausado" else "")
                       + ".\n\n" if (data.get("batches") or 1) > 1 or data.get("status") == "pausado" else "")
                    + f"Está en mi biblioteca RAG como «{data.get('title')}» ({data.get('rag_chunks')} fragmentos) y he "
                      f"incorporado {data.get('learned')} elementos a mi conocimiento. Pregúntame lo que quieras sobre ella.")
        if name == "rag_search":
            items = data.get("results", []) if isinstance(data, dict) else []
            if not items:
                return "No he encontrado nada sobre eso en mi biblioteca de documentos."
            return "En mi biblioteca:\n\n" + "\n\n".join(f"> {i['text'][:300]}…\n> — *{i['document']}*, p. {i['page']}" for i in items[:3])
        if name in ("web_fetch", "web_search"):
            txt = data.get("text") or json.dumps(data.get("results", data), ensure_ascii=False)
            return f"Esto es lo que he encontrado:\n\n{str(txt)[:1200]}"
        return f"Resultado de **{name}**:\n```\n{json.dumps(data, ensure_ascii=False, indent=1)[:1500]}\n```"
