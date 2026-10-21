"""Gestor de modelos: catalogo local, busqueda en 5 repositorios, descargas reanudables en segundo
plano, importacion de ficheros/carpetas locales y activacion por tipo (LLM, imagen, voz)."""
from __future__ import annotations

import json
import re
import shutil
import threading
import time
import uuid
from pathlib import Path

from .. import log, net, paths
from . import gguf
from .recommended import RECOMMENDED, recommend
from .sources import all_sources

L = log.get("models")
EXT_FORMAT = {".gguf": "gguf", ".safetensors": "safetensors", ".onnx": "onnx", ".ckpt": "ckpt", ".bin": "bin",
              ".pt": "pt", ".pth": "pt", ".xml": "openvino"}
EMBED_HINT = re.compile(r"(^|[\W_])(embed|embedding|bge|e5|gte|minilm|nomic-embed|arctic-embed)([\W_]|$)", re.I)
IMAGE_HINT = re.compile(r"(^|[\W_])(sd|sdxl|sdxs|sd3|flux|diffusion|turbo|dreamshaper|realistic|anime|z-image|lcm)", re.I)


SPLIT_RE = re.compile(r"-\d{5}-of-\d{5}\.gguf$", re.I)
QUANT_RE = re.compile(r"(IQ\d_[A-Z]+|Q\d_K_[SML]|Q\d_K|Q\d_\d|Q\d|F16|BF16|F32|FP16|FP8|INT8|INT4|AWQ|GPTQ|EXL2)", re.I)
REPO_SKIP = re.compile(r"(^|/)(\.gitattributes|README\.md|LICENSE.*|.*\.(png|jpg|jpeg|gif|webp|mp4|md|txt))$", re.I)
QUANT_RANK = ["Q4_K_M", "Q4_K_S", "Q5_K_M", "IQ4_XS", "Q4_0", "Q5_K_S", "Q6_K", "Q8_0", "Q3_K_M", "Q3_K_L", "Q2_K", "F16", "BF16"]


def best_mmproj(listing: list[dict]) -> dict | None:
    mm = [f for f in listing if "mmproj" in f["path"].lower() and f["path"].lower().endswith(".gguf")]
    if not mm:
        return None
    return sorted(mm, key=lambda f: (0 if "q8" in f["path"].lower() else 1 if "f16" in f["path"].lower() else 2, f.get("size", 0)))[0]


def annotate(files: list[dict], profile: dict, source: str = "", repo: str = "") -> list[dict]:
    """Anade a cada fichero: formato, tipo, cuantizacion, motores compatibles, si cabe y cual es el recomendado."""
    mem = max(profile.get("vram_gb", 0) * 0.9 if profile["llm"]["gpu_layers"] != 0 else 0, profile["ram_gb"] * 0.6)
    repo_l = repo.lower()
    has_mm = any("mmproj" in f["path"].lower() for f in files)
    is_repo_model = any(f["path"].endswith(("config.json", "model_index.json")) for f in files)
    for f in files:
        p = f["path"]
        low = p.lower()
        ext = low.rsplit(".", 1)[-1] if "." in low else ""
        size_gb = (f.get("size") or 0) / 1024**3
        q = QUANT_RE.search(Path(p).name)
        f["quant"] = q.group(1).upper() if q else ""
        f["format"] = {"gguf": "gguf", "safetensors": "safetensors", "onnx": "onnx", "bin": "bin", "ckpt": "ckpt",
                       "pt": "pt", "pth": "pt", "xml": "openvino"}.get(ext, ext)
        image_like = bool(IMAGE_HINT.search(f"{repo_l} {low}"))
        if "mmproj" in low:
            f["kind"], f["engines"] = "proyector", ["llama-server (vision)"]
        elif ext == "gguf" and EMBED_HINT.search(f"{repo_l} {low}") and not has_mm:
            f["kind"], f["engines"] = "embed", ["llama-server (--embedding)", "llama-cpp-python"]
        elif ext == "gguf":
            f["kind"] = "image" if image_like and not has_mm else "llm"
            f["engines"] = ["sd.cpp"] if f["kind"] == "image" else ["llama-server", "llama-cpp-python", "Ollama (importar)"]
        elif ext in ("safetensors", "ckpt") and (image_like or source == "civitai"):
            f["kind"], f["engines"] = "image", ["sd.cpp", "diffusers"]
        elif ext == "onnx" and ("piper" in repo_l or low.endswith(".onnx") and any(x["path"] == p + ".json" for x in files)):
            f["kind"], f["engines"] = "tts", ["Piper"]
        elif ext in ("safetensors", "bin", "pt") and is_repo_model:
            f["kind"], f["engines"] = "llm", ["Transformers (repositorio completo)"]
        elif source == "ollama":
            f["kind"], f["engines"] = "llm", ["Ollama", "llama-server (GGUF del registro)"]
        else:
            f["kind"], f["engines"] = "otro", []
        f["split"] = bool(SPLIT_RE.search(low))
        pm = re.search(r"(\d+(?:\.\d+)?)\s*[bB](?![a-zA-Z])", f"{repo} {Path(p).name}")
        if pm and f["kind"] == "llm" and float(pm.group(1)) > profile["llm"]["max_model_b"]:
            f["slow"] = True   # cabe en memoria, pero el procesador/GPU lo movera despacio
        f["fits"] = (size_gb < mem) if size_gb else None
        f["fit"] = "?" if not size_gb else "ideal" if size_gb < mem * 0.6 else "ok" if size_gb < mem else "pesado"
    # cuantizacion recomendada: la mejor en ranking que quepa holgadamente
    ggufs = [f for f in files if f["format"] == "gguf" and f["kind"] == "llm" and f["quant"]]
    for qname in QUANT_RANK:
        cand = [f for f in ggufs if f["quant"] == qname and f["fit"] in ("ideal", "ok")]
        if cand:
            cand[0]["recommended"] = True
            break
    return files


ACTIVE_KEYS = {"llm": "llm.default_model", "image": "media.image_model", "tts": "media.tts_model",
               "embed": "memory.embed_model"}


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:60]


class Catalog:
    def __init__(self) -> None:
        self.file = paths.models_dir() / "catalog.json"
        self._lock = threading.RLock()
        self.items: dict[str, dict] = json.loads(self.file.read_text(encoding="utf-8")) if self.file.exists() else {}

    def save(self) -> None:
        with self._lock:
            self.file.write_text(json.dumps(self.items, indent=1, ensure_ascii=False), encoding="utf-8")

    def get(self, mid: str) -> dict | None:
        item = self.items.get(mid)
        return item if item and Path(item["path"]).exists() else None

    def list(self) -> list[dict]:
        return [m for m in self.items.values() if Path(m["path"]).exists()]

    def add(self, entry: dict) -> dict:
        with self._lock:
            self.items[entry["id"]] = entry
            self.save()
        return entry

    def remove(self, mid: str) -> dict | None:
        with self._lock:
            item = self.items.pop(mid, None)
            self.save()
        return item


def classify(path: Path, hint: str = "") -> dict:
    """Detecta formato y tipo de un fichero/carpeta de modelo."""
    info: dict = {"format": "unknown", "kind": "other"}
    if path.is_dir():
        if (path / "config.json").exists():
            info.update(format="transformers", kind="llm")
        elif (path / "model_index.json").exists():
            info.update(format="diffusers", kind="image")
        elif list(path.glob("*.xml")):
            info.update(format="openvino", kind="llm")
        return info
    info["format"] = EXT_FORMAT.get(path.suffix.lower(), "unknown")
    text = f"{hint} {path.name}"
    if info["format"] == "gguf":
        meta = gguf.summarize(path)
        info["meta"] = meta
        if meta.get("is_projector"):
            info["kind"] = "projector"
        elif meta.get("is_embedding") or EMBED_HINT.search(text):
            info["kind"] = "embed"
        elif meta.get("architecture"):
            info["kind"] = "llm"
        elif IMAGE_HINT.search(text):
            info["kind"] = "image"
    elif info["format"] in ("safetensors", "ckpt") and IMAGE_HINT.search(text):
        info["kind"] = "image"
    elif info["format"] == "onnx" and "piper" in text.lower() or path.name.endswith(".onnx") and (path.with_suffix(".onnx.json")).exists():
        info["kind"] = "tts"
    return info


class ModelManager:
    def __init__(self, config, profile_getter) -> None:
        self.config = config
        self.profile_getter = profile_getter
        self.catalog = Catalog()
        self.sources = all_sources(config)
        self.jobs: dict[str, dict] = {}
        self._cancel: set[str] = set()
        self._files_cache: dict[tuple[str, str], list[dict]] = {}
        self.ollama_pull = None      # se inyecta si hay Ollama local: descarga via 'ollama pull'
        self.on_activate: list = []  # avisos al activar un modelo (core.Services cambia el embedder en caliente)
        self._autoregister()

    # ------------------------------------------------------------ busqueda
    def search(self, source: str, query: str, kind: str = "") -> list[dict]:
        return self.sources[source].search(query, kind)

    def files(self, source: str, repo_id: str) -> list[dict]:
        files = self.sources[source].files(repo_id)
        self._files_cache[(source, repo_id)] = files
        return annotate(files, self.profile_getter(), source, repo_id)

    def recommended(self) -> list[dict]:
        sector = {}
        try:
            from ..office import sector_models
            sector = sector_models(self.config.get("office.profile", "") or "")
        except Exception as e:  # noqa: BLE001
            L.debug("Sin sugerencias de sector: %s", e)
        recs = recommend(self.profile_getter(), sector)
        for r in recs:
            r["installed"] = self.catalog.get(r["id"]) is not None
        return recs

    # ------------------------------------------------------------ descargas
    def _dest(self, source: str, repo_id: str, file: str) -> Path:
        name = Path(file).name if source != "ollama" else slug(file or repo_id) + ".gguf"
        return paths.models_dir() / source / repo_id.replace("/", "__").replace(":", "_") / name

    def _job(self, title: str) -> str:
        jid = uuid.uuid4().hex[:8]
        self.jobs[jid] = {"id": jid, "title": title, "status": "en cola", "done": 0, "total": 0, "started": time.time()}
        return jid

    def _run(self, jid: str, files: list[tuple[str, Path, dict]], on_done) -> None:
        job = self.jobs[jid]
        try:
            job["total"] = sum(f[2].get("size", 0) for f in files)
            base = 0
            for url, dest, extra in files:
                job["status"] = f"descargando {dest.name}"
                if not dest.exists():
                    net.download(url, dest, headers=extra.get("headers"),
                                 progress=lambda d, t, b=base: job.update(done=b + d, total=max(job["total"], b + t)),
                                 cancel=lambda: jid in self._cancel)
                base += dest.stat().st_size
                job["done"] = base
            job["entry"] = on_done()
            job["status"] = "completado"
            L.info("Descarga completada: %s", job["title"])
        except InterruptedError:
            job["status"] = "cancelado"
        except Exception as e:  # noqa: BLE001
            job["status"] = "error"
            job["error"] = str(e)
            L.error("Descarga %s fallida: %s", job["title"], e)

    def download(self, source: str, repo_id: str, file: str, kind: str = "", with_extras: bool = True) -> str:
        """Descarga cualquier fichero. Extras automaticos: todas las partes de un GGUF dividido y el
        proyector de vision (mmproj) del mismo repositorio. Con Ollama local, usa 'ollama pull'."""
        src = self.sources[source]
        jid = self._job(f"{src.name}: {repo_id} / {file}")
        hdr = {"headers": src.headers()}

        def work():
            if source == "ollama" and self.ollama_pull is not None:
                try:
                    return self._pull_ollama(jid, file or repo_id)
                except Exception as e:  # noqa: BLE001
                    L.warning("ollama pull no disponible (%s); descarga directa del registro", e)
            files = []
            mmproj_dest = None
            try:
                if source == "ollama":
                    layers = src.layers(file or repo_id)
                    dest = self._dest(source, repo_id, file)
                    files.append((layers["model"][0], dest, {"size": layers["model"][1]}))
                    if "projector" in layers:
                        mmproj_dest = dest.with_name(dest.stem + "-mmproj.gguf")
                        files.append((layers["projector"][0], mmproj_dest, {"size": layers["projector"][1]}))
                else:
                    listing = self._files_cache.get((source, repo_id)) or []
                    parts = [file]
                    m = SPLIT_RE.search(file)
                    if m and with_extras:  # GGUF dividido: todas las partes (llama.cpp carga desde la primera)
                        stem = file[:m.start()]
                        parts = sorted(f["path"] for f in listing if f["path"].startswith(stem) and SPLIT_RE.search(f["path"])) or [file]
                    for part in parts:
                        size = next((f.get("size", 0) for f in listing if f["path"] == part), 0)
                        files.append((src.download_url(repo_id, part), self._dest(source, repo_id, part), {**hdr, "size": size}))
                    dest = files[0][1]
                    if with_extras and file.lower().endswith(".gguf") and "mmproj" not in file.lower():
                        mm = best_mmproj(listing)
                        if mm:
                            mmproj_dest = self._dest(source, repo_id, mm["path"])
                            files.append((src.download_url(repo_id, mm["path"]), mmproj_dest, {**hdr, "size": mm.get("size", 0)}))
                            self.jobs[jid]["title"] += " + proyector de vision"
            except Exception as e:  # noqa: BLE001
                self.jobs[jid].update(status="error", error=str(e))
                return
            self._run(jid, files, lambda: self.register(dest, source=source, repo=repo_id, kind=kind,
                                                       mmproj=str(mmproj_dest) if mmproj_dest else None))

        threading.Thread(target=work, daemon=True, name=f"dl-{jid}").start()
        return jid

    def download_repo(self, source: str, repo_id: str, kind: str = "") -> str:
        """Descarga un repositorio completo (Transformers, Diffusers, ONNX, OpenVINO...) a una carpeta."""
        src = self.sources[source]
        jid = self._job(f"{src.name}: {repo_id} (repositorio completo)")
        hdr = {"headers": src.headers()}

        def work():
            try:
                listing = self._files_cache.get((source, repo_id)) or src.files(repo_id)
                keep = [f for f in listing if not REPO_SKIP.search(f["path"])]
                # si hay varias variantes de pesos, se evitan duplicados pesados (.bin si existe .safetensors)
                if any(f["path"].endswith(".safetensors") for f in keep):
                    keep = [f for f in keep if not f["path"].endswith((".bin", ".pt", ".pth", ".ckpt", ".msgpack", ".h5"))
                            or "tokenizer" in f["path"]]
                root = paths.models_dir() / source / repo_id.replace("/", "__")
                files = [(src.download_url(repo_id, f["path"]), root / f["path"], {**hdr, "size": f.get("size", 0)}) for f in keep]
            except Exception as e:  # noqa: BLE001
                self.jobs[jid].update(status="error", error=str(e))
                return
            self._run(jid, files, lambda: self.register(root, source=source, repo=repo_id, kind=kind))

        threading.Thread(target=work, daemon=True, name=f"dl-{jid}").start()
        return jid

    def _pull_ollama(self, jid: str, name: str) -> None:
        job = self.jobs[jid]
        job["status"] = "ollama pull"
        for ev in self.ollama_pull(name):
            if jid in self._cancel:
                job["status"] = "cancelado"
                return
            if ev.get("total"):
                job.update(total=ev["total"], done=ev.get("completed", 0))
            job["status"] = f"ollama: {ev.get('status', '')}"[:60]
            if ev.get("error"):
                raise RuntimeError(ev["error"])
        job.update(status="completado", entry={"id": f"ollama:{name}", "kind": "llm", "backend": "ollama"})
        L.info("Modelo de Ollama instalado: %s", name)

    def resolve_recommended(self, rec: dict) -> dict:
        """Comprueba en Hugging Face que los ficheros del catalogo existen; si el repositorio los ha renombrado,
        elige el equivalente (GGUF Q4_K_M y proyector Q8_0/F16). Sin red, deja los nombres del catalogo."""
        if rec["kind"] not in ("llm", "embed") or not rec.get("file", "").endswith(".gguf"):
            return rec
        try:
            names = [f["path"] for f in self.sources["huggingface"].files(rec["repo"])]
        except Exception as e:  # noqa: BLE001
            L.info("No se pudo listar %s (%s): se usan los nombres del catalogo", rec["repo"], e)
            return rec
        if not names or rec["file"] in names and (not rec.get("mmproj") or rec["mmproj"] in names):
            return rec
        ggufs = [n for n in names if n.lower().endswith(".gguf")]
        projs = [n for n in ggufs if "mmproj" in n.lower()]
        mains = [n for n in ggufs if n not in projs and not SPLIT_RE.search(n)]

        def pick(cands: list[str], prefs: tuple[str, ...]) -> str | None:
            for p in prefs:
                hit = sorted(c for c in cands if p in c.upper())
                if hit:
                    return hit[0]
            return sorted(cands)[0] if cands else None
        out = dict(rec)
        if rec["file"] not in names:   # los de embeddings pierden mucho por debajo de Q8_0
            prefs = ("Q8_0", "F16", "Q6_K", "Q5_K_M") if rec["kind"] == "embed" else ("Q4_K_M", "Q4_K_S", "Q4_0", "Q5_K_M", "Q8_0")
            out["file"] = pick(mains, prefs) or rec["file"]
        if rec.get("mmproj") and rec["mmproj"] not in names:
            out["mmproj"] = pick(projs, ("Q8_0", "F16", "BF16", "F32")) or rec["mmproj"]
        L.info("Catalogo %s: ficheros resueltos en el repositorio -> %s / %s", rec["id"], out["file"], out.get("mmproj"))
        return out

    def download_recommended(self, rec_id: str) -> str:
        base_rec = next(r for r in RECOMMENDED if r["id"] == rec_id)
        hf = self.sources["huggingface"]
        jid = self._job(base_rec["name"])

        def work():
            rec = self.resolve_recommended(base_rec)
            files = [(hf.download_url(rec["repo"], rec["file"]), self._dest("huggingface", rec["repo"], rec["file"]), {})]
            for extra in [rec.get("mmproj")] + rec.get("extra", []):
                if extra:
                    files.append((hf.download_url(rec["repo"], extra), self._dest("huggingface", rec["repo"], extra), {}))
            for f in files:
                f[2]["size"] = int(rec["size_mb"] * 1024**2 / len(files))

            def done():
                entry = self.register(files[0][1], source="huggingface", repo=rec["repo"], kind=rec["kind"],
                                      mmproj=str(files[1][1]) if rec.get("mmproj") else None, mid=rec["id"],
                                      name=rec["name"], extra={"gen": rec.get("gen"), "vision": rec.get("vision", False),
                                                               "reasoning": rec.get("reasoning", False)})
                self._auto_activate(entry)
                return entry
            self._run(jid, files, done)

        threading.Thread(target=work, daemon=True).start()
        return jid

    def cancel(self, jid: str) -> None:
        self._cancel.add(jid)

    # ------------------------------------------------------------ registro / importacion
    def register(self, path: Path, source: str = "local", repo: str = "", kind: str = "", mmproj: str | None = None,
                 mid: str = "", name: str = "", extra: dict | None = None) -> dict:
        path = Path(path)
        info = classify(path, repo)
        if kind and info["kind"] in ("other", "llm") and kind != "llm":
            info["kind"] = kind
        if not mmproj and info["format"] == "gguf" and info["kind"] == "llm":
            cands = [p for p in path.parent.glob("*.gguf") if p != path and "mmproj" in p.name.lower()]
            mmproj = str(cands[0]) if cands else None
        size = path.stat().st_size if path.is_file() else sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
        entry = {"id": mid or slug(path.stem if path.is_file() else path.name), "name": name or path.stem,
                 "kind": info["kind"], "format": info["format"], "path": str(path), "mmproj": mmproj,
                 "vision": bool(mmproj), "size": size, "source": source, "repo": repo, "added": time.time(),
                 "meta": info.get("meta", {}), **(extra or {})}
        entry["vision"] = entry.get("vision") or bool(mmproj)
        return self.catalog.add(entry)

    def import_local(self, path: str, copy: bool = False, kind: str = "") -> dict:
        p = Path(path).expanduser()
        if not p.exists():
            raise FileNotFoundError(path)
        if copy:
            dest = paths.models_dir() / "local" / p.name
            dest.parent.mkdir(parents=True, exist_ok=True)
            (shutil.copytree if p.is_dir() else shutil.copy2)(p, dest)
            p = dest
        return self.register(p, source="local", kind=kind)

    def _autoregister(self) -> None:
        """Registra ficheros presentes en data/models que no esten en el catalogo (p.ej. copiados a mano)."""
        known = {m["path"] for m in self.catalog.items.values()} | {m.get("mmproj") for m in self.catalog.items.values()}
        recs = {(r["repo"].replace("/", "__"), Path(r["file"]).name): r for r in RECOMMENDED}
        for f in paths.models_dir().rglob("*"):
            if f.suffix.lower() not in (".gguf", ".safetensors", ".onnx") or str(f) in known or "mmproj" in f.name.lower():
                continue
            rec = recs.get((f.parent.name, f.name))
            try:
                if rec:
                    mm = f.parent / Path(rec["mmproj"]).name if rec.get("mmproj") else None
                    self._auto_activate(self.register(
                        f, source="huggingface", repo=rec["repo"], kind=rec["kind"], mid=rec["id"], name=rec["name"],
                        mmproj=str(mm) if mm and mm.exists() else None,
                        extra={"gen": rec.get("gen"), "vision": rec.get("vision", False),
                               "reasoning": rec.get("reasoning", False)}))
                elif classify(f)["kind"] != "projector":
                    self.register(f, source="local")
            except Exception as e:  # noqa: BLE001
                L.warning("No se pudo registrar %s: %s", f, e)

    def delete(self, mid: str, remove_files: bool = False) -> None:
        item = self.catalog.remove(mid)
        if item and remove_files and str(paths.models_dir()) in item["path"]:
            for p in (item["path"], item.get("mmproj")):
                if p and Path(p).exists():
                    (shutil.rmtree if Path(p).is_dir() else Path(p).unlink)(p)

    # ------------------------------------------------------------ activacion
    def activate(self, mid: str) -> dict:
        entry = self.catalog.get(mid)
        if not entry:
            raise KeyError(mid)
        key = ACTIVE_KEYS.get(entry["kind"])
        if not key:
            raise ValueError(f"Tipo '{entry['kind']}' no activable")
        self.config.set(key, mid)
        self._notify(entry)
        return entry

    def _auto_activate(self, entry: dict) -> None:
        key = ACTIVE_KEYS.get(entry["kind"])
        if key and not self.config.get(key):
            self.config.set(key, entry["id"])
            self._notify(entry)

    def _notify(self, entry: dict) -> None:
        for fn in list(self.on_activate):   # p.ej. el modelo de embeddings se cambia en caliente
            try:
                fn(entry)
            except Exception as e:  # noqa: BLE001
                L.warning("Activacion de %s: %s", entry.get("id"), e)

    def active(self) -> dict:
        return {"llm": self.config.get("llm.default_model"), "image": self.config.get("media.image_model"),
                "tts": self.config.get("media.tts_model", ""), "embed": self.config.get("memory.embed_model", "")}
