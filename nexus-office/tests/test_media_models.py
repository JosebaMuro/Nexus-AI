import struct
import wave

from nexus.hardware import build_profile
from nexus.media.image import ImageEngine
from nexus.media.music import Composer, detect_mood
from nexus.models import classify, recommend
from nexus.models.gguf import read_metadata, summarize
from test_hardware import hw


def test_music_wav_and_midi():
    res = Composer(bars=4).compose("una música épica de batalla", seed=1)
    assert res["mood"] == "epica"
    with wave.open(res["path"]) as w:
        assert w.getnchannels() == 2 and w.getnframes() > 32000
    midi = res["path"].replace(".wav", ".mid")
    assert open(midi, "rb").read(4) == b"MThd"


def test_mood_detection_handles_accents():
    assert detect_mood("Melodía TRISTE y melancólica") == "triste"
    assert detect_mood("algo cyberpunk futurista") == "electronica"


def test_procedural_image():
    img = ImageEngine.procedural("ciudad de neón azul y rosa", 7, 128)
    assert img.size == (128, 128)
    stats = ImageEngine.describe_stats(img)
    assert len(stats["dominant_colors"]) >= 1


def _gguf(path, kv):
    def s(t):
        b = t.encode()
        return struct.pack("<Q", len(b)) + b
    body = b""
    for k, v in kv.items():
        body += s(k) + (struct.pack("<I", 8) + s(v) if isinstance(v, str) else struct.pack("<II", 4, v))
    path.write_bytes(b"GGUF" + struct.pack("<IQQ", 3, 0, len(kv)) + body)


def test_gguf_reader_and_classify(tmp_path):
    f = tmp_path / "tiny-model.Q4_K_M.gguf"
    _gguf(f, {"general.architecture": "llama", "general.name": "Tiny", "general.file_type": 15, "llama.context_length": 4096})
    meta = read_metadata(f)
    assert meta["general.name"] == "Tiny"
    s = summarize(f)
    assert s["quant"] == "Q4_K_M" and s["context_length"] == 4096
    assert classify(f)["kind"] == "llm"
    proj = tmp_path / "mmproj.gguf"
    _gguf(proj, {"general.architecture": "clip"})
    assert classify(proj)["kind"] == "projector"


def test_recommendations_fit_hardware():
    small = recommend(build_profile(hw(8, cores=2)))
    best = {r["kind"]: r["id"] for r in small if r.get("best")}
    assert best["llm"] == "qwen3.5-2b" and best["image"] == "sdxs-512"
    big = recommend(build_profile(hw(32, gpus=[__import__("nexus.hardware.detect", fromlist=["GPU"]).GPU("nvidia", "RTX", 12, False)])))
    assert next(r for r in big if r["id"] == "qwen3.5-9b")["fit"] in ("ideal", "ok")
    assert next(r for r in big if r.get("best") and r["kind"] == "llm")["id"].startswith("qwen3.5-")
