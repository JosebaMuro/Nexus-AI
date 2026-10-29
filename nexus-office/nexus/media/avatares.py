"""Avatares 3D gratuitos desde Internet: buscar, descargar y usar en Nexus-Office (y despues, sin conexion).

Fuentes:

* ``osa``: Open Source Avatars (opensourceavatars.com), registro de cientos de avatares VRM con licencia libre (casi
  todos CC0: 100Avatars de Polygonal Mind, ToxSam, chibis...). El indice esta en GitHub y los modelos en Arweave/IPFS.
* ``muestras``: modelos de referencia con licencia abierta (VRM Consortium, Khronos glTF, three.js).
* ``sketchfab``: busqueda en Sketchfab de modelos descargables con licencia Creative Commons. Descargar desde
  Sketchfab exige la clave de API gratuita de una cuenta (``avatares.sketchfab_token`` en Ajustes).

Lo descargado queda en ``outputs/avatars`` con su miniatura y sus datos (autor, licencia, origen) en
``biblioteca.json``: se puede elegir como avatar en cualquier momento, tambien sin Internet. Solo se descargan URLs
que vienen del propio catalogo (no direcciones arbitrarias).
"""
from __future__ import annotations

import json
import re
import threading
import time
import uuid
from pathlib import Path

import httpx

from .. import log, paths

L = log.get("avatares")
UA = {"User-Agent": "Mozilla/5.0 (compatible; Nexus-Office avatares)"}
OSA = "https://raw.githubusercontent.com/ToxSam/open-source-avatars/main/data/"
MAX_BYTES = 300_000_000
EXT_OK = (".glb", ".vrm", ".gltf")
IPFS_GATEWAYS = ("https://dweb.link/ipfs/", "https://ipfs.io/ipfs/", "https://gateway.pinata.cloud/ipfs/",
                 "https://w3s.link/ipfs/")

_GH = "https://raw.githubusercontent.com/"
_KH = _GH + "KhronosGroup/glTF-Sample-Assets/main/Models/"
_VRMC = _GH + "vrm-c/vrm-specification/master/samples/"
_THREE = _GH + "mrdoob/three.js/r160/examples/models/gltf/"
MUESTRAS = [
    {"id": "seed-san", "nombre": "Seed-san", "formato": "VRM", "autor": "VirtualCast (VRM Consortium)",
     "licencia": "VRM Public License 1.0", "url": _VRMC + "Seed-san/vrm/Seed-san.vrm",
     "miniatura": _VRMC + "Seed-san/screenshot/screenshot.png", "descripcion": "Avatar de referencia de VRM 1.0"},
    {"id": "vrm1-twist", "nombre": "VRM1 Constraint Twist", "formato": "VRM", "autor": "VRM Consortium",
     "licencia": "VRM Public License 1.0", "url": _VRMC + "VRM1_Constraint_Twist_Sample/vrm/VRM1_Constraint_Twist_Sample.vrm",
     "miniatura": _VRMC + "VRM1_Constraint_Twist_Sample/screenshot/screenshot.jpg", "descripcion": "Chica anime (VRM 1.0)"},
    {"id": "robot-expressive", "nombre": "Robot expresivo", "formato": "GLB", "autor": "Tomás Laulhé (three.js)",
     "licencia": "CC0", "url": _THREE + "RobotExpressive/RobotExpressive.glb", "miniatura": "",
     "descripcion": "Robot con 14 animaciones (saludar, bailar, andar...)"},
    {"id": "xbot", "nombre": "X Bot", "formato": "GLB", "autor": "Mixamo (three.js)", "licencia": "Mixamo (uso libre)",
     "url": _THREE + "Xbot.glb", "miniatura": "", "descripcion": "Maniquí humano con animaciones"},
    {"id": "michelle", "nombre": "Michelle", "formato": "GLB", "autor": "Mixamo (three.js)", "licencia": "Mixamo (uso libre)",
     "url": _THREE + "Michelle.glb", "miniatura": "", "descripcion": "Personaje bailando"},
    {"id": "cesium-man", "nombre": "Cesium Man", "formato": "GLB", "autor": "Cesium (Khronos)", "licencia": "CC-BY 4.0",
     "url": _KH + "CesiumMan/glTF-Binary/CesiumMan.glb", "miniatura": _KH + "CesiumMan/screenshot/screenshot.gif",
     "descripcion": "Hombre caminando (muestra de glTF)"},
    {"id": "brainstem", "nombre": "BrainStem", "formato": "GLB", "autor": "Keith Hunter (Khronos)", "licencia": "CC-BY 4.0",
     "url": _KH + "BrainStem/glTF-Binary/BrainStem.glb", "miniatura": _KH + "BrainStem/screenshot/screenshot.gif",
     "descripcion": "Robot bailando"},
    {"id": "rigged-figure", "nombre": "Rigged Figure", "formato": "GLB", "autor": "Cesium (Khronos)", "licencia": "CC-BY 4.0",
     "url": _KH + "RiggedFigure/glTF-Binary/RiggedFigure.glb", "miniatura": _KH + "RiggedFigure/screenshot/screenshot.gif",
     "descripcion": "Figura articulada sencilla"},
    {"id": "fox", "nombre": "Zorro", "formato": "GLB", "autor": "PixelMannen, tomkranis (Khronos)", "licencia": "CC-BY 4.0",
     "url": _KH + "Fox/glTF-Binary/Fox.glb", "miniatura": _KH + "Fox/screenshot/screenshot.jpg",
     "descripcion": "Zorro low-poly con animaciones"},
]
FUENTES = {
    "osa": {"nombre": "Open Source Avatars", "web": "https://opensourceavatars.com",
            "descripcion": "Cientos de avatares VRM gratuitos, casi todos CC0"},
    "muestras": {"nombre": "Modelos de referencia", "web": "https://github.com/KhronosGroup/glTF-Sample-Assets",
                 "descripcion": "VRM Consortium, Khronos glTF y three.js con licencia abierta"},
    "sketchfab": {"nombre": "Sketchfab (Creative Commons)", "web": "https://sketchfab.com",
                  "descripcion": "Buscar modelos descargables con licencia CC; descargar necesita tu clave de API gratuita"},
}


def _dir() -> Path:
    return paths.sub("outputs", "avatars")


def _safe(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("._")[:80] or "avatar"


class Avatares:
    def __init__(self, cfg=None, client: httpx.Client | None = None) -> None:
        self.cfg = cfg
        self.http = client or httpx.Client(headers=UA, timeout=30, follow_redirects=True)
        self._lock = threading.Lock()
        self.trabajos: dict[str, dict] = {}
        self._osa: list[dict] | None = None
        self._osa_t = 0.0

    # ------------------------------------------------------------ biblioteca local
    @property
    def _bib(self) -> Path:
        return _dir() / "biblioteca.json"

    def biblioteca(self) -> list[dict]:
        try:
            items = json.loads(self._bib.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            items = []
        return [i for i in items if (_dir() / i["archivo"]).exists()]

    def _guardar(self, items: list[dict]) -> None:
        tmp = self._bib.with_suffix(".tmp")
        tmp.write_text(json.dumps(items, ensure_ascii=False, indent=1), encoding="utf-8")
        tmp.replace(self._bib)

    def borrar(self, aid: str) -> bool:
        with self._lock:
            items = self.biblioteca()
            item = next((i for i in items if i["id"] == aid), None)
            if not item:
                return False
            for k in ("archivo", "miniatura_local"):
                if item.get(k):
                    (_dir() / item[k]).unlink(missing_ok=True)
            self._guardar([i for i in items if i["id"] != aid])
        return True

    # ------------------------------------------------------------ catalogos
    def _osa_indice(self) -> list[dict]:
        """Indice de Open Source Avatars (se guarda en disco: sin conexion se usa el ultimo)."""
        cache = _dir() / "osa-indice.json"
        if self._osa is not None and time.time() - self._osa_t < 86400:
            return self._osa
        try:
            projects = self.http.get(OSA + "projects.json").json()
            out: list[dict] = []
            for p in projects:
                if not p.get("is_public", True) or not p.get("avatar_data_file"):
                    continue
                try:
                    avs = self.http.get(OSA + p["avatar_data_file"]).json()
                except (httpx.HTTPError, ValueError) as e:
                    L.warning("osa %s: %s", p.get("id"), e)
                    continue
                for a in avs:
                    if a.get("is_public") is False or a.get("is_draft") or not a.get("model_file_url"):
                        continue
                    attrs = (a.get("metadata") or {}).get("attributes") or []
                    out.append({"id": f"osa:{a['id']}", "nombre": a.get("name") or "Avatar",
                                "descripcion": a.get("description") or "", "formato": (a.get("format") or "VRM").upper(),
                                "url": a["model_file_url"], "miniatura": a.get("thumbnail_url") or "",
                                "coleccion": p.get("name") or p.get("id"), "autor": p.get("creator_id") or "",
                                "licencia": p.get("license") or "", "fuente": "osa",
                                "etiquetas": " ".join(str(t.get("value", "")) for t in attrs if isinstance(t, dict))})
            if out:
                cache.write_text(json.dumps(out, ensure_ascii=False), encoding="utf-8")
                self._osa, self._osa_t = out, time.time()
                return out
        except (httpx.HTTPError, ValueError) as e:
            L.info("Open Source Avatars sin conexion (%s): uso el indice guardado", e)
        try:
            self._osa = json.loads(cache.read_text(encoding="utf-8"))
            self._osa_t = time.time() - 86400 + 600   # reintentar en 10 min
        except (OSError, ValueError):
            self._osa = []
        return self._osa

    def _sketchfab(self, q: str, pagina: int, por_pagina: int) -> dict:
        params = {"type": "models", "downloadable": "true", "q": q or "avatar character",
                  "count": por_pagina, "cursor": (pagina - 1) * por_pagina, "sort_by": "-likeCount"}
        r = self.http.get("https://api.sketchfab.com/v3/search", params=params)
        r.raise_for_status()
        try:
            d = r.json()
        except ValueError as e:   # Sketchfab responde vacio a algunas redes (centros de datos, proxies)
            raise RuntimeError(f"Sketchfab no ha devuelto resultados desde esta red (HTTP {r.status_code}); "
                               "prueba más tarde o usa Open Source Avatars") from e
        items = []
        for m in d.get("results", []):
            lic = (m.get("license") or {}).get("label") or (m.get("license") or {}).get("slug") or ""
            thumbs = sorted(((m.get("thumbnails") or {}).get("images") or []), key=lambda t: abs((t.get("width") or 0) - 256))
            items.append({"id": f"sketchfab:{m['uid']}", "nombre": m.get("name") or "Modelo", "formato": "GLB",
                          "descripcion": (m.get("description") or "")[:200], "autor": (m.get("user") or {}).get("displayName", ""),
                          "licencia": lic, "miniatura": thumbs[0]["url"] if thumbs else "", "url": m.get("viewerUrl", ""),
                          "fuente": "sketchfab", "coleccion": "Sketchfab"})
        return {"resultados": items, "total": d.get("totalCount") or len(items), "pagina": pagina,
                "necesita_clave": not self._token()}

    def _token(self) -> str:
        return (self.cfg.get("avatares.sketchfab_token", "") if self.cfg else "") or ""

    def buscar(self, fuente: str = "osa", q: str = "", pagina: int = 1, por_pagina: int = 24) -> dict:
        pagina, por_pagina = max(1, int(pagina)), max(1, min(60, int(por_pagina)))
        if fuente == "sketchfab":
            return self._sketchfab(q, pagina, por_pagina)
        items = MUESTRAS if fuente == "muestras" else self._osa_indice()
        items = [{**i, "fuente": fuente, "id": i["id"] if ":" in i["id"] else f"{fuente}:{i['id']}"} for i in items]
        if q:
            words = q.lower().split()
            items = [i for i in items if all(w in " ".join(str(i.get(k, "")) for k in
                                                           ("nombre", "descripcion", "coleccion", "autor", "licencia", "etiquetas")).lower()
                                             for w in words)]
        tengo = {i.get("origen_id") for i in self.biblioteca()}
        ini = (pagina - 1) * por_pagina
        page = [{**i, "descargado": i["id"] in tengo} for i in items[ini:ini + por_pagina]]
        return {"resultados": page, "total": len(items), "pagina": pagina,
                "colecciones": sorted({i.get("coleccion", "") for i in items if i.get("coleccion")})}

    def _item(self, cid: str) -> dict:
        fuente, _, rid = cid.partition(":")
        if fuente == "muestras":
            it = next((m for m in MUESTRAS if m["id"] == rid), None)
            return {**it, "id": cid, "fuente": fuente} if it else {}
        if fuente == "osa":
            return next((i for i in self._osa_indice() if i["id"] == cid), {})
        if fuente == "sketchfab":
            tok = self._token()
            if not tok:
                raise PermissionError("Sketchfab pide la clave de API de tu cuenta (gratis): ponla en Ajustes > Avatar")
            info = self.http.get(f"https://api.sketchfab.com/v3/models/{rid}").json()
            r = self.http.get(f"https://api.sketchfab.com/v3/models/{rid}/download", headers={"Authorization": f"Token {tok}"})
            if r.status_code in (401, 403):
                raise PermissionError("Sketchfab ha rechazado la clave de API")
            r.raise_for_status()
            d = r.json()
            url = (d.get("glb") or {}).get("url")
            if not url:
                raise LookupError("Sketchfab no ofrece este modelo en GLB")
            lic = (info.get("license") or {}).get("label", "")
            thumbs = sorted(((info.get("thumbnails") or {}).get("images") or []), key=lambda t: abs((t.get("width") or 0) - 256))
            return {"id": cid, "nombre": info.get("name") or rid, "formato": "GLB", "url": url, "fuente": fuente,
                    "autor": (info.get("user") or {}).get("displayName", ""), "licencia": lic,
                    "miniatura": thumbs[0]["url"] if thumbs else "", "web": info.get("viewerUrl", ""), "ext": ".glb"}
        return {}

    # ------------------------------------------------------------ descarga
    def _urls(self, url: str) -> list[str]:
        """La URL y, si es de IPFS, la misma por otras pasarelas (alguna suele estar caida)."""
        m = re.search(r"/ipfs/(.+)$", url)
        if not m:
            return [url]
        return [url] + [g + m.group(1) for g in IPFS_GATEWAYS if not url.startswith(g)]

    def _bajar(self, url: str, dest: Path, job: dict | None = None, limite: int = MAX_BYTES) -> int:
        err: Exception | None = None
        for u in self._urls(url):
            try:
                with self.http.stream("GET", u, timeout=httpx.Timeout(30, read=120)) as r:
                    r.raise_for_status()
                    total = int(r.headers.get("content-length") or 0)
                    if job is not None:
                        job["total"] = total
                    n = 0
                    with open(dest, "wb") as f:
                        for chunk in r.iter_bytes(1 << 16):
                            n += len(chunk)
                            if n > limite:
                                raise ValueError("el archivo supera el limite de tamano")
                            f.write(chunk)
                            if job is not None:
                                job["bytes"] = n
                return n
            except (httpx.HTTPError, ValueError, OSError) as e:
                err = e
                L.info("avatar: %s fallo (%s)", u, e)
        raise RuntimeError(f"no se pudo descargar: {err}")

    def descargar(self, cid: str) -> dict:
        job = {"id": uuid.uuid4().hex[:8], "origen_id": cid, "estado": "en_curso", "bytes": 0, "total": 0,
               "inicio": time.time(), "error": "", "avatar": None}
        self.trabajos[job["id"]] = job

        def run() -> None:
            try:
                job["avatar"] = self._descargar(cid, job)
                job["estado"] = "hecho"
            except Exception as e:   # noqa: BLE001
                job.update(estado="error", error=str(e))
                L.warning("avatar %s: %s", cid, e)

        threading.Thread(target=run, daemon=True, name=f"avatar-{job['id']}").start()
        return job

    def _descargar(self, cid: str, job: dict | None = None) -> dict:
        it = self._item(cid)
        if not it or not it.get("url"):
            raise LookupError("avatar desconocido en el catalogo")
        ya = next((i for i in self.biblioteca() if i.get("origen_id") == cid), None)
        if ya:
            return ya
        ext = it.get("ext") or next((e for e in EXT_OK if it["url"].lower().split("?")[0].endswith(e)), None) \
            or (".vrm" if it.get("formato") == "VRM" else ".glb")
        aid = uuid.uuid4().hex[:10]
        base = f"{aid}_{_safe(it['nombre'])}"
        dest = _dir() / (base + ext)
        part = dest.with_suffix(ext + ".part")
        try:
            self._bajar(it["url"], part, job)
            with open(part, "rb") as f:
                head = f.read(4)
            if head != b"glTF" and ext != ".gltf":
                raise ValueError("lo descargado no es un modelo glTF/VRM")
            part.replace(dest)
        finally:
            part.unlink(missing_ok=True)
        mini = ""
        if it.get("miniatura"):
            try:
                mext = Path(it["miniatura"].split("?")[0]).suffix.lower()
                mext = mext if mext in (".png", ".jpg", ".jpeg", ".gif", ".webp") else ".png"
                self._bajar(it["miniatura"], _dir() / (base + "_mini" + mext), limite=20_000_000)
                mini = base + "_mini" + mext
            except RuntimeError:
                pass
        item = {"id": aid, "origen_id": cid, "nombre": it["nombre"], "formato": it.get("formato", ""),
                "autor": it.get("autor", ""), "licencia": it.get("licencia", ""), "coleccion": it.get("coleccion", ""),
                "fuente": it.get("fuente", ""), "archivo": dest.name, "miniatura_local": mini,
                "url": f"/outputs/avatars/{dest.name}", "miniatura_url": f"/outputs/avatars/{mini}" if mini else "",
                "web": it.get("web") or FUENTES.get(it.get("fuente", ""), {}).get("web", ""),
                "bytes": dest.stat().st_size, "fecha": time.time()}
        with self._lock:
            items = self.biblioteca()
            items.append(item)
            self._guardar(items)
        return item


_A: Avatares | None = None


def get(cfg=None) -> Avatares:
    global _A
    if _A is None:
        _A = Avatares(cfg)
    elif cfg is not None:
        _A.cfg = cfg
    return _A


def reset() -> None:
    global _A
    _A = None
