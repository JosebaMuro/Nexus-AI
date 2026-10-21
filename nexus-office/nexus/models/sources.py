"""Los 5 grandes repositorios de modelos: Hugging Face, Ollama, ModelScope, Civitai y Kaggle.

Cada fuente implementa ``search(query, kind)``, ``files(repo_id)`` y ``download_url(repo_id, file)``.
"""
from __future__ import annotations

import base64
import re

import httpx

from ..net import UA, get_json

KIND_HINTS = {"llm": "gguf", "image": "text-to-image", "stt": "automatic-speech-recognition", "tts": "text-to-speech"}


class Source:
    id = "base"
    name = "base"
    url = ""

    def __init__(self, config) -> None:
        self.config = config

    def headers(self) -> dict:
        return {}

    def search(self, query: str, kind: str = "") -> list[dict]:
        raise NotImplementedError

    def files(self, repo_id: str) -> list[dict]:
        raise NotImplementedError

    def download_url(self, repo_id: str, file: str) -> str:
        raise NotImplementedError


class HuggingFace(Source):
    id, name, url = "huggingface", "Hugging Face", "https://huggingface.co"

    def headers(self):
        tok = self.config.get("models.huggingface_token")
        return {"Authorization": f"Bearer {tok}"} if tok else {}

    def search(self, query, kind=""):
        params = {"search": query, "limit": 30, "sort": "downloads", "direction": -1}
        if kind == "llm":
            params["filter"] = "gguf"
        elif kind in KIND_HINTS:
            params["pipeline_tag"] = KIND_HINTS[kind]
        data = get_json(f"{self.url}/api/models", self.headers(), params)
        return [{"id": m["id"], "name": m["id"], "downloads": m.get("downloads", 0), "likes": m.get("likes", 0),
                 "tags": [t for t in m.get("tags", []) if ":" not in t][:8], "task": m.get("pipeline_tag", "")} for m in data]

    def files(self, repo_id):
        data = get_json(f"{self.url}/api/models/{repo_id}/tree/main", self.headers(), {"recursive": "true"})
        return [{"path": f["path"], "size": (f.get("lfs") or {}).get("size", f.get("size", 0))}
                for f in data if f.get("type") == "file"]

    def download_url(self, repo_id, file):
        return f"{self.url}/{repo_id}/resolve/main/{file}"


class Ollama(Source):
    """Busqueda en ollama.com y descarga directa de capas del registro (GGUF), sin necesitar Ollama."""
    id, name, url = "ollama", "Ollama", "https://ollama.com"
    REG = "https://registry.ollama.ai/v2"

    def search(self, query, kind=""):
        html = httpx.get(f"{self.url}/search", params={"q": query}, headers=UA, timeout=20).text
        names = list(dict.fromkeys(re.findall(r'href="/library/([\w.\-]+)"', html)))
        return [{"id": n, "name": n, "downloads": 0, "tags": [], "task": "llm"} for n in names[:30]]

    def files(self, repo_id):
        name = repo_id.split(":")[0]
        html = httpx.get(f"{self.url}/library/{name}/tags", headers=UA, timeout=20).text
        tags = list(dict.fromkeys(re.findall(rf"/library/{re.escape(name)}:([\w.\-]+)", html)))
        return [{"path": f"{name}:{t}", "size": 0} for t in tags]

    def manifest(self, ref: str) -> dict:
        name, _, tag = ref.partition(":")
        ns = name if "/" in name else f"library/{name}"
        return get_json(f"{self.REG}/{ns}/manifests/{tag or 'latest'}",
                        {"Accept": "application/vnd.docker.distribution.manifest.v2+json"})

    def layers(self, ref: str) -> dict:
        """{'model': (url, size), 'projector': (url, size)?}"""
        name = ref.split(":")[0]
        ns = name if "/" in name else f"library/{name}"
        out = {}
        for layer in self.manifest(ref).get("layers", []):
            mt = layer["mediaType"].rsplit(".", 1)[-1]
            if mt in ("model", "projector"):
                out[mt] = (f"{self.REG}/{ns}/blobs/{layer['digest']}", layer["size"])
        return out

    def download_url(self, repo_id, file):
        return self.layers(file or repo_id)["model"][0]


class ModelScope(Source):
    id, name, url = "modelscope", "ModelScope", "https://www.modelscope.cn"

    def headers(self):
        tok = self.config.get("models.modelscope_token")
        return {"Authorization": f"Bearer {tok}"} if tok else {}

    def search(self, query, kind=""):
        body = {"PageSize": 30, "PageNumber": 1, "SortBy": "Default", "Target": "", "Criterion": [], "Name": query}
        r = httpx.put(f"{self.url}/api/v1/dolphin/models", json=body, headers={**UA, **self.headers()}, timeout=20)
        r.raise_for_status()
        models = r.json().get("Data", {}).get("Model", {}).get("Models", []) or []
        return [{"id": f"{m['Path']}/{m['Name']}", "name": f"{m['Path']}/{m['Name']}", "downloads": m.get("Downloads", 0),
                 "tags": (m.get("Tags") or [])[:8], "task": ", ".join(t.get("Name", "") for t in (m.get("Tasks") or []))[:60]}
                for m in models]

    def files(self, repo_id):
        data = get_json(f"{self.url}/api/v1/models/{repo_id}/repo/files", self.headers(),
                        {"Revision": "master", "Recursive": "true"})
        return [{"path": f["Path"], "size": f.get("Size", 0)} for f in data.get("Data", {}).get("Files", [])
                if f.get("Type") != "tree"]

    def download_url(self, repo_id, file):
        return f"{self.url}/models/{repo_id}/resolve/master/{file}"


class Civitai(Source):
    id, name, url = "civitai", "Civitai", "https://civitai.com"

    def headers(self):
        tok = self.config.get("models.civitai_token")
        return {"Authorization": f"Bearer {tok}"} if tok else {}

    def search(self, query, kind=""):
        params = {"query": query, "limit": 30}
        if kind in ("", "image"):
            params["types"] = "Checkpoint"
        data = get_json(f"{self.url}/api/v1/models", self.headers(), params)
        return [{"id": str(m["id"]), "name": m["name"], "downloads": m.get("stats", {}).get("downloadCount", 0),
                 "tags": m.get("tags", [])[:8], "task": m.get("type", "")} for m in data.get("items", [])]

    def files(self, repo_id):
        m = get_json(f"{self.url}/api/v1/models/{repo_id}", self.headers())
        out = []
        for v in m.get("modelVersions", [])[:5]:
            for f in v.get("files", []):
                out.append({"path": f["name"], "size": int(f.get("sizeKB", 0) * 1024), "url": f["downloadUrl"],
                            "version": v.get("name"), "base_model": v.get("baseModel")})
        return out

    def download_url(self, repo_id, file):
        for f in self.files(repo_id):
            if f["path"] == file:
                return f["url"]
        raise FileNotFoundError(file)


class Kaggle(Source):
    id, name, url = "kaggle", "Kaggle Models", "https://www.kaggle.com"

    def headers(self):
        user, key = self.config.get("models.kaggle_username"), self.config.get("models.kaggle_key")
        if user and key:
            return {"Authorization": "Basic " + base64.b64encode(f"{user}:{key}".encode()).decode()}
        return {}

    def search(self, query, kind=""):
        data = get_json(f"{self.url}/api/v1/models/list", self.headers(), {"search": query, "pageSize": 30})
        return [{"id": m["ref"], "name": m.get("title", m["ref"]), "downloads": m.get("voteCountNullable", 0),
                 "tags": [], "task": (m.get("subtitle") or "")[:80]} for m in data.get("models", [])]

    def files(self, repo_id):
        owner, slug = repo_id.split("/", 1)
        m = get_json(f"{self.url}/api/v1/models/{owner}/{slug}/get", self.headers())
        out = []
        for inst in m.get("instances", []):
            ref = f"{inst.get('framework')}/{inst.get('slug')}/{inst.get('versionNumber')}"
            out.append({"path": ref, "size": inst.get("totalUncompressedBytes", 0) or 0,
                        "note": "archivo .tar.gz (requiere usuario/clave de Kaggle y aceptar licencia)"})
        return out

    def download_url(self, repo_id, file):
        return f"{self.url}/api/v1/models/{repo_id}/{file}/download"


def all_sources(config) -> dict[str, Source]:
    return {s.id: s for s in (HuggingFace(config), Ollama(config), ModelScope(config), Civitai(config), Kaggle(config))}
