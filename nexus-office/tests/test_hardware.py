from nexus.hardware import build_profile, detect
from nexus.hardware.detect import GPU, NPU, NPU_PATTERN, HardwareInfo, _is_integrated
from nexus.hardware.tuner import MIN_CTX, tune


def hw(ram=16, cores=8, gpus=(), npus=(), flags=("avx2",), runtimes=None):
    return HardwareInfo(os="Windows", os_version="", arch="AMD64", cpu_name="CPU", cores_physical=cores,
                        cores_logical=cores * 2, cpu_flags=list(flags), ram_gb=ram, disk_free_gb=100,
                        gpus=list(gpus), npus=list(npus), runtimes=runtimes or {})


def test_detect_real_machine():
    info = detect(probe_runtimes=False)
    assert info.ram_gb > 0 and info.cores_logical >= 1
    profile = build_profile(info)
    assert profile["llm"]["n_threads"] >= 1


def test_npu_pattern_word_boundaries():
    assert not NPU_PATTERN.search("Logitech USB Input Device")
    assert NPU_PATTERN.search("Intel(R) AI Boost")
    assert NPU_PATTERN.search("AMD IPU Device")
    assert NPU_PATTERN.search("Qualcomm(R) Hexagon(TM) NPU")


def test_integrated_classification():
    assert _is_integrated("intel", "Intel(R) UHD Graphics")
    assert _is_integrated("intel", "Intel(R) Arc(TM) Graphics")
    assert not _is_integrated("intel", "Intel(R) Arc(TM) A770 Graphics")
    assert _is_integrated("amd", "AMD Radeon(TM) Graphics")
    assert not _is_integrated("amd", "AMD Radeon RX 7800 XT")
    assert not _is_integrated("nvidia", "NVIDIA GeForce RTX 4090")


def test_profiles_per_vendor():
    nv = build_profile(hw(32, gpus=[GPU("nvidia", "RTX 4090", 24, False)]))
    assert nv["accelerator"] == "cuda" and nv["llm"]["gpu_layers"] == -1 and nv["llm"]["max_model_b"] >= 14
    amd = build_profile(hw(16, gpus=[GPU("amd", "Radeon RX 7800 XT", 16, False)]))
    assert amd["accelerator"] in ("vulkan", "rocm") and amd["llm"]["gpu_layers"] == -1
    igpu = build_profile(hw(8, cores=2, gpus=[GPU("intel", "Intel(R) UHD Graphics", 0.1, True)]))
    assert igpu["accelerator"] == "cpu" and igpu["llm"]["max_model_b"] <= 3 and igpu["tier"] == "ligero"
    cpu = build_profile(hw(64, cores=16))
    assert cpu["accelerator"] == "cpu" and cpu["llm"]["max_model_b"] >= 8


def test_npu_profile():
    p = build_profile(hw(16, npus=[NPU("intel", "Intel(R) AI Boost", "openvino")]))
    assert p["npu"]["present"] and not p["npu"]["ready"] and "openvino" in p["npu"]["hint"]
    p2 = build_profile(hw(16, npus=[NPU("intel", "Intel(R) AI Boost", "openvino")], runtimes={"openvino_devices": ["CPU", "NPU"]}))
    assert p2["npu"]["ready"] and "embeddings" in p2["npu"]["offload"]


def base_profile():
    return build_profile(hw(16, cores=4))


def test_tuner_memory_crash_reduces_but_respects_floor():
    p = base_profile()
    tuned, state, reasons = tune(p, [], True, 8, state={}, previous_info={"peak_ram_pct": 97})
    assert tuned["llm"]["n_ctx"] < p["llm"]["n_ctx"] or p["llm"]["n_ctx"] <= MIN_CTX
    assert tuned["llm"]["n_ctx"] >= min(MIN_CTX, p["llm"]["n_ctx"])
    assert reasons


def test_tuner_abrupt_exit_without_memory_pressure_keeps_config():
    p = base_profile()
    tuned, _, reasons = tune(p, [], True, 8, state={}, previous_info={"peak_ram_pct": 40})
    assert tuned["llm"]["n_ctx"] == p["llm"]["n_ctx"]
    assert any("sin presion" in r for r in reasons)


def test_tuner_thread_exploration_then_exploitation():
    p = base_profile()
    state = {}
    hist = []
    seen = []
    for tps in (5.0, 9.0, 6.0):
        settings = {"n_threads": None}
        tuned, state, _ = tune(p, hist, False, 8, state=state)
        seen.append(tuned["llm"]["n_threads"])
        settings["n_threads"] = tuned["llm"]["n_threads"]
        hist.append({"llm_calls": 5, "avg_tps": tps, "peak_ram_pct": 70, "clean_exit": True, "settings": settings})
    tuned, state, _ = tune(p, hist, False, 8, state=state)
    trials = state["thread_trials"]
    best = max(trials, key=lambda k: sum(trials[k]) / len(trials[k]))
    assert tuned["llm"]["n_threads"] == int(best)


def test_tuner_slow_suggests_smaller_model():
    p = base_profile()
    tuned, _, _ = tune(p, [{"llm_calls": 5, "avg_tps": 2.0, "peak_ram_pct": 70, "clean_exit": True, "settings": {}}], False, 8, state={})
    assert tuned["llm"].get("suggest_smaller_model")
