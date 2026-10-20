"""Voz: texto->voz (Piper neural offline > pyttsx3/SAPI > navegador) y voz->texto
(faster-whisper > navegador). Todo con degradacion: si no hay motor local, la UI usa la Web Speech API."""
from __future__ import annotations

import importlib.util
import io
import os
import tempfile
import threading
import wave

from .. import log, paths

L = log.get("voice")


class VoiceEngine:
    def __init__(self, config, catalog, profile_getter) -> None:
        self.config, self.catalog, self.profile_getter = config, catalog, profile_getter
        self._piper = None
        self._piper_key = None
        self._whisper = None
        self._lock = threading.Lock()

    # ------------------------------------------------------------------ estado
    def _piper_model(self) -> dict | None:
        mid = self.config.get("media.tts_model", "")
        entry = self.catalog.get(mid) if mid else None
        return entry or next((m for m in self.catalog.list() if m["kind"] == "tts"), None)

    def tts_engines(self) -> list[str]:
        out = []
        if importlib.util.find_spec("piper") and self._piper_model():
            out.append("piper")
        if importlib.util.find_spec("pyttsx3"):
            out.append("pyttsx3")
        out.append("browser")
        return out

    def stt_engines(self) -> list[str]:
        return (["faster-whisper"] if importlib.util.find_spec("faster_whisper") else []) + ["browser"]

    def info(self) -> dict:
        tts = self.tts_engines()
        stt = self.stt_engines()
        pref_t, pref_s = self.config.get("media.tts_engine", "auto"), self.config.get("media.stt_engine", "auto")
        return {"tts": tts, "stt": stt,
                "tts_active": tts[0] if pref_t == "auto" else (pref_t if pref_t in tts else "browser"),
                "stt_active": stt[0] if pref_s == "auto" else (pref_s if pref_s in stt else "browser"),
                "stt_model": self.profile_getter()["stt"]["model"]}

    # ------------------------------------------------------------------ TTS
    def tts(self, text: str, rate: float = 1.0) -> tuple[bytes, str] | None:
        engine = self.info()["tts_active"]
        text = text.strip()[:2000]
        if not text or engine == "browser":
            return None
        try:
            if engine == "piper":
                return self._tts_piper(text, rate), "piper"
            if engine == "pyttsx3":
                return self._tts_pyttsx3(text, rate), "pyttsx3"
        except Exception as e:  # noqa: BLE001
            L.warning("TTS %s fallo: %s", engine, e)
        return None

    def _tts_piper(self, text: str, rate: float) -> bytes:
        from piper import PiperVoice
        entry = self._piper_model()
        with self._lock:
            if self._piper_key != entry["path"]:
                self._piper = PiperVoice.load(entry["path"])
                self._piper_key = entry["path"]
            buf = io.BytesIO()
            with wave.open(buf, "wb") as wf:
                try:
                    from piper import SynthesisConfig
                    self._piper.synthesize_wav(text, wf, syn_config=SynthesisConfig(length_scale=1.0 / max(0.5, rate)))
                except ImportError:
                    self._piper.synthesize(text, wf, length_scale=1.0 / max(0.5, rate))
            return buf.getvalue()

    def _tts_pyttsx3(self, text: str, rate: float) -> bytes:
        import pyttsx3
        with self._lock:
            if os.name == "nt":
                import comtypes
                comtypes.CoInitialize()
            try:
                eng = pyttsx3.init()
                eng.setProperty("rate", int(180 * rate))
                lang = self.config.get("user.language", "es")
                wanted = self.config.get("user.voice_name", "")
                for v in eng.getProperty("voices"):
                    label = f"{v.name} {v.id} {getattr(v, 'languages', '')}".lower()
                    if (wanted and wanted.lower() in label) or (not wanted and (lang in label or "spanish" in label
                                                                              or "helena" in label or "sabina" in label)):
                        eng.setProperty("voice", v.id)
                        break
                fd, path = tempfile.mkstemp(suffix=".wav", dir=paths.sub("cache"))
                os.close(fd)
                eng.save_to_file(text, path)
                eng.runAndWait()
                eng.stop()
                with open(path, "rb") as f:
                    data = f.read()
                os.unlink(path)
                return data
            finally:
                if os.name == "nt":
                    comtypes.CoUninitialize()

    # ------------------------------------------------------------------ STT
    def stt(self, audio: bytes, suffix: str = ".webm") -> dict:
        if "faster-whisper" not in self.stt_engines():
            return {"error": "No hay motor de voz local; usa el reconocimiento del navegador", "engine": "browser"}
        from faster_whisper import WhisperModel
        prof = self.profile_getter()["stt"]
        with self._lock:
            if self._whisper is None:
                L.info("Cargando faster-whisper '%s' (%s, %s)", prof["model"], prof["device"], prof["compute_type"])
                self._whisper = WhisperModel(prof["model"], device=prof["device"], compute_type=prof["compute_type"],
                                             download_root=str(paths.models_dir() / "whisper"),
                                             cpu_threads=self.profile_getter()["llm"]["n_threads"])
        fd, path = tempfile.mkstemp(suffix=suffix, dir=paths.sub("cache"))
        with os.fdopen(fd, "wb") as f:
            f.write(audio)
        try:
            segments, info = self._whisper.transcribe(path, language=self.config.get("user.language", "es") or None,
                                                      vad_filter=True, beam_size=1)
            text = " ".join(s.text.strip() for s in segments)
            return {"text": text, "language": info.language, "engine": "faster-whisper"}
        finally:
            os.unlink(path)
