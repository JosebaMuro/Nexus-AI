"""Compositor musical.

* ``procedural`` (siempre disponible, CPU, sin modelos): teoria musical + sintesis con numpy.
  Del prompt deduce estado de animo -> tonalidad, modo, tempo, progresion, instrumentos y bateria.
  Exporta WAV estereo y MIDI.
* ``musicgen`` (opcional, GPU recomendada): facebook/musicgen-small via transformers.
"""
from __future__ import annotations

import importlib.util
import re
import struct
import time
import wave
import zlib

import numpy as np

from .. import log, paths

L = log.get("music")
SR = 32000
NOTE_NAMES = ["Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si"]
SCALES = {"major": [0, 2, 4, 5, 7, 9, 11], "minor": [0, 2, 3, 5, 7, 8, 10], "dorian": [0, 2, 3, 5, 7, 9, 10],
          "phrygian": [0, 1, 3, 5, 7, 8, 10]}
MOODS = {
    "feliz": {"mode": "major", "tempo": (112, 128), "prog": [0, 4, 5, 3], "lead": "pluck", "pad": "soft", "drums": "pop", "arp": False},
    "triste": {"mode": "minor", "tempo": (64, 78), "prog": [0, 5, 2, 6], "lead": "sine", "pad": "warm", "drums": "none", "arp": False},
    "epica": {"mode": "minor", "tempo": (88, 100), "prog": [0, 5, 2, 6], "lead": "brass", "pad": "strings", "drums": "epic", "arp": True},
    "calma": {"mode": "major", "tempo": (70, 84), "prog": [0, 5, 1, 4], "lead": "sine", "pad": "warm", "drums": "soft", "arp": False},
    "misterio": {"mode": "phrygian", "tempo": (80, 92), "prog": [0, 1, 0, 6], "lead": "sine", "pad": "dark", "drums": "soft", "arp": True},
    "electronica": {"mode": "minor", "tempo": (120, 128), "prog": [0, 5, 3, 4], "lead": "saw", "pad": "soft", "drums": "edm", "arp": True},
    "jazz": {"mode": "dorian", "tempo": (96, 116), "prog": [1, 4, 0, 5], "lead": "sine", "pad": "warm", "drums": "soft", "arp": False},
}
KEYWORDS = {
    "feliz": "feliz alegre happy alegria fiesta verano sol positiva upbeat divertida pop",
    "triste": "triste sad melancolica melancolia lluvia nostalgia llanto lenta",
    "epica": "epica epic heroica batalla cine cinematografica orquestal aventura poderosa trailer",
    "calma": "calma relajante tranquila relax chill paz meditacion dormir lofi suave ambient",
    "misterio": "misterio misteriosa oscura terror suspense tensa dark horror noche",
    "electronica": "electronica cyberpunk futurista synthwave techno edm dance robot neon espacial",
    "jazz": "jazz swing blues bar cafe",
}


def _plain(text: str) -> str:
    import unicodedata
    return "".join(c for c in unicodedata.normalize("NFKD", text.lower()) if not unicodedata.combining(c))


def detect_mood(prompt: str) -> str:
    p = _plain(prompt)
    scores = {m: sum(1 for w in kw.split() if w in p) for m, kw in KEYWORDS.items()}
    best = max(scores, key=scores.get)
    return best if scores[best] else "calma"


def midi_freq(n: float) -> float:
    return 440.0 * 2 ** ((n - 69) / 12)


def adsr(n: int, a=0.01, d=0.1, s=0.7, r=0.2) -> np.ndarray:
    a_n, d_n, r_n = int(a * SR), int(d * SR), int(r * SR)
    s_n = max(0, n - a_n - d_n - r_n)
    env = np.concatenate([np.linspace(0, 1, a_n, False), np.linspace(1, s, d_n, False), np.full(s_n, s),
                          np.linspace(s, 0, r_n)])
    return np.pad(env, (0, max(0, n - len(env))))[:n]


def osc(kind: str, freq: float, n: int, rng) -> np.ndarray:
    t = np.arange(n) / SR
    ph = 2 * np.pi * freq * t
    if kind == "saw":
        return sum(np.sin(k * ph) / k for k in range(1, 9)) * 0.5
    if kind == "square":
        return sum(np.sin(k * ph) / k for k in range(1, 10, 2)) * 0.6
    if kind == "brass":
        return (np.sin(ph) + 0.5 * np.sin(2 * ph) + 0.3 * np.sin(3 * ph) + 0.15 * np.sin(4 * ph)) * 0.5
    if kind == "strings":
        det = sum(np.sin(2 * np.pi * freq * (1 + d) * t + rng.uniform(0, 6)) for d in (-0.004, 0, 0.005))
        return det / 3
    if kind == "pluck":
        return (np.sin(ph) + 0.4 * np.sin(2 * ph)) * np.exp(-t * 6)
    if kind == "dark":
        return (np.sin(ph) + 0.3 * np.sin(1.5 * ph)) * 0.7
    return np.sin(ph) + 0.15 * np.sin(2 * ph)


def lowpass(x: np.ndarray, alpha: float) -> np.ndarray:
    """Filtro de un polo vectorizado aproximado (media movil exponencial por bloques)."""
    k = max(1, int(1 / alpha))
    kernel = np.exp(-np.arange(k * 4) / k)
    kernel /= kernel.sum()
    return np.convolve(x, kernel, mode="same")


class Composer:
    def __init__(self, bars: int = 16) -> None:
        self.bars = bars

    def compose(self, prompt: str, seconds: float | None = None, seed: int | None = None) -> dict:
        seed = seed if seed is not None else zlib.crc32(prompt.encode()) ^ int(time.time()) & 0xFFFF
        rng = np.random.default_rng(seed)
        mood = detect_mood(prompt)
        m = MOODS[mood]
        tempo = int(rng.integers(*m["tempo"]))
        root = int(rng.choice([57, 60, 62, 64, 65, 67]))  # La3..Sol4
        scale = SCALES[m["mode"]]
        beat = 60 / tempo
        bars = self.bars if not seconds else max(4, int(seconds / (beat * 4)))
        total = int(bars * 4 * beat * SR) + SR * 2
        left, right = np.zeros(total), np.zeros(total)
        notes_midi: list[tuple[float, float, int, int]] = []   # (inicio_beats, duracion_beats, nota, canal)

        def deg(d: int, octave: int = 0) -> int:
            return root + scale[d % 7] + 12 * (d // 7 + octave)

        def put(sig: np.ndarray, start_beats: float, pan: float = 0.0, gain: float = 1.0):
            i = int(start_beats * beat * SR)
            j = min(total, i + len(sig))
            left[i:j] += sig[: j - i] * gain * (1 - max(0, pan))
            right[i:j] += sig[: j - i] * gain * (1 + min(0, pan))

        prog = m["prog"]
        prev = 7
        for bar in range(bars):
            chord_deg = prog[bar % len(prog)]
            chord = [deg(chord_deg), deg(chord_deg + 2), deg(chord_deg + 4)]
            b0 = bar * 4
            # pad / acordes
            n = int(4 * beat * SR)
            for note in chord:
                sig = osc("strings" if m["pad"] == "strings" else "dark" if m["pad"] == "dark" else "sine",
                          midi_freq(note - 12), n, rng) * adsr(n, 0.3, 0.5, 0.6, 0.6)
                put(sig, b0, rng.uniform(-0.4, 0.4), 0.10)
                notes_midi.append((b0, 4, note - 12, 1))
            # bajo
            for q in range(0, 4, 2 if tempo < 90 else 1):
                n = int(beat * SR * (2 if tempo < 90 else 1) * 0.9)
                put(osc("square", midi_freq(chord[0] - 24), n, rng) * adsr(n, 0.005, 0.1, 0.5, 0.08), b0 + q, 0, 0.16)
                notes_midi.append((b0 + q, 0.9, chord[0] - 24, 2))
            # arpegio
            if m["arp"]:
                for s in range(8):
                    note = chord[s % 3] + (12 if s >= 4 else 0)
                    n = int(beat * SR * 0.45)
                    put(osc("pluck", midi_freq(note), n, rng) * adsr(n, 0.002, 0.05, 0.3, 0.1), b0 + s * 0.5,
                        0.5 if s % 2 else -0.5, 0.08)
            # melodia: paseo aleatorio por la escala, tonos del acorde en tiempos fuertes
            if bar % 8 < 6 or bars < 8:
                t_beat = 0.0
                while t_beat < 4:
                    dur = float(rng.choice([0.5, 1, 1, 1.5, 2] if tempo < 100 else [0.5, 0.5, 1, 1]))
                    if rng.random() < 0.15:
                        t_beat += dur
                        continue
                    if t_beat in (0, 2):
                        target = chord_deg + int(rng.choice([0, 2, 4])) + 7
                    else:
                        target = prev + int(rng.choice([-2, -1, 1, 2, 0]))
                    prev = int(np.clip(target, 4, 13))
                    note = deg(prev)
                    n = int(dur * beat * SR)
                    sig = osc(m["lead"], midi_freq(note), n, rng) * adsr(n, 0.01, 0.12, 0.6, min(0.25, dur * beat * 0.4))
                    put(sig, b0 + t_beat, 0.1, 0.22)
                    notes_midi.append((b0 + t_beat, dur, note, 0))
                    t_beat += dur
            # bateria
            self._drums(m["drums"], b0, put, rng, bar)

        mix = np.stack([left, right], axis=1)
        mix[:, 0] = lowpass(mix[:, 0], 0.5) * 0.5 + mix[:, 0] * 0.5
        mix[:, 1] = lowpass(mix[:, 1], 0.5) * 0.5 + mix[:, 1] * 0.5
        fade = int(1.5 * SR)
        mix[-fade:] *= np.linspace(1, 0, fade)[:, None]
        mix /= max(1e-6, np.abs(mix).max()) / 0.9
        name = f"{int(time.time())}_{re.sub(r'[^a-z0-9]+', '-', _plain(prompt))[:30].strip('-') or 'tema'}"
        out_dir = paths.sub("outputs", "music")
        wav_path = out_dir / f"{name}.wav"
        with wave.open(str(wav_path), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes((mix * 32767).astype("<i2").tobytes())
        midi_path = out_dir / f"{name}.mid"
        write_midi(midi_path, notes_midi, tempo)
        key = f"{NOTE_NAMES[root % 12]} {'mayor' if m['mode'] == 'major' else 'menor' if m['mode'] == 'minor' else m['mode']}"
        return {"engine": "procedural", "path": str(wav_path), "url": f"/outputs/music/{wav_path.name}",
                "midi_url": f"/outputs/music/{midi_path.name}", "mood": mood, "tempo": tempo, "key": key,
                "duration": round(len(mix) / SR, 1), "bars": bars, "seed": int(seed)}

    @staticmethod
    def _drums(style: str, b0: int, put, rng, bar: int) -> None:
        if style == "none":
            return

        def kick():
            n = int(0.35 * SR)
            t = np.arange(n) / SR
            return np.sin(2 * np.pi * (50 + 120 * np.exp(-t * 30)) * t) * np.exp(-t * 9)

        def snare():
            n = int(0.2 * SR)
            t = np.arange(n) / SR
            return (rng.standard_normal(n) * 0.6 + np.sin(2 * np.pi * 190 * t) * 0.4) * np.exp(-t * 18)

        def hat():
            n = int(0.05 * SR)
            x = rng.standard_normal(n)
            return (x - np.convolve(x, np.ones(4) / 4, "same")) * np.exp(-np.arange(n) / SR * 80)

        g = {"soft": 0.35, "pop": 0.6, "edm": 0.75, "epic": 0.8}.get(style, 0.5)
        for q in range(4):
            if style == "edm" or q in (0, 2) or (style == "epic" and q == 3):
                put(kick(), b0 + q, 0, g)
            if q in (1, 3) and style != "soft":
                put(snare(), b0 + q, 0, g * 0.5)
            if style in ("pop", "edm", "soft"):
                put(hat(), b0 + q + 0.5, 0.3, g * 0.25)
        if style == "epic" and bar % 4 == 3:
            for i in range(4):
                put(snare(), b0 + 3 + i * 0.25, 0, g * 0.35)


def write_midi(path, notes: list[tuple[float, float, int, int]], tempo: int, tpq: int = 480) -> None:
    events = []
    for start, dur, note, ch in notes:
        events.append((int(start * tpq), 0x90 | ch, note, 90))
        events.append((int((start + dur) * tpq), 0x80 | ch, note, 0))
    events.sort(key=lambda e: (e[0], e[1] & 0xF0 == 0x90))

    def vlq(v: int) -> bytes:
        out = [v & 0x7F]
        while v > 0x7F:
            v >>= 7
            out.insert(0, (v & 0x7F) | 0x80)
        return bytes(out)

    us = int(60_000_000 / tempo)
    track = b"\x00\xff\x51\x03" + us.to_bytes(3, "big")
    last = 0
    for t, status, note, vel in events:
        track += vlq(t - last) + bytes([status, note & 0x7F, vel])
        last = t
    track += b"\x00\xff\x2f\x00"
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, tpq))
        f.write(b"MTrk" + struct.pack(">I", len(track)) + track)


class MusicEngine:
    def __init__(self, config, profile_getter) -> None:
        self.config, self.profile_getter = config, profile_getter
        self.composer = Composer()
        self._musicgen = None

    def engines(self) -> list[str]:
        out = ["procedural"]
        if importlib.util.find_spec("transformers") and importlib.util.find_spec("torch"):
            out.append("musicgen")
        return out

    def generate(self, prompt: str, seconds: float = 20, engine: str = "") -> dict:
        engine = engine or self.config.get("media.music_engine", "auto")
        if engine == "auto":
            engine = "musicgen" if "musicgen" in self.engines() and self.profile_getter()["music"]["neural"] else "procedural"
        if engine == "musicgen":
            try:
                return self._gen_musicgen(prompt, seconds)
            except Exception as e:  # noqa: BLE001
                L.warning("MusicGen fallo (%s); usando compositor procedural", e)
        return self.composer.compose(prompt, seconds)

    def _gen_musicgen(self, prompt: str, seconds: float) -> dict:
        import scipy.io.wavfile  # noqa: F401  (dependencia de transformers para audio)
        import torch
        from transformers import AutoProcessor, MusicgenForConditionalGeneration
        dev = self.profile_getter()["music"]["device"]
        dev = dev if dev in ("cuda", "mps", "xpu") else "cpu"
        if self._musicgen is None:
            name = self.profile_getter()["music"]["suggested_model"]
            cache = str(paths.models_dir() / "musicgen")
            self._musicgen = (AutoProcessor.from_pretrained(name, cache_dir=cache),
                              MusicgenForConditionalGeneration.from_pretrained(name, cache_dir=cache).to(dev))
        proc, model = self._musicgen
        inputs = proc(text=[prompt], padding=True, return_tensors="pt").to(dev)
        with torch.no_grad():
            audio = model.generate(**inputs, max_new_tokens=int(seconds * 50))
        sr = model.config.audio_encoder.sampling_rate
        data = audio[0, 0].float().cpu().numpy()
        path = paths.sub("outputs", "music") / f"{int(time.time())}_musicgen.wav"
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes((data / max(1e-6, abs(data).max()) * 0.9 * 32767).astype("<i2").tobytes())
        return {"engine": "musicgen", "path": str(path), "url": f"/outputs/music/{path.name}", "mood": detect_mood(prompt),
                "tempo": "-", "key": "-", "duration": round(len(data) / sr, 1)}
