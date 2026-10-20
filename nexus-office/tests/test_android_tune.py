"""Autoajuste en vivo de la app Android autonoma (nexus/android_tune.py)."""
import json
import os
import time
from types import SimpleNamespace

import pytest

from nexus import android_tune as at


def _cpuinfo(tmp_path, parts):
    text = "".join(f"processor\t: {i}\nCPU part\t: {p}\n\n" for i, p in enumerate(parts))
    f = tmp_path / "cpuinfo"
    f.write_text(text)
    return str(f)


def _sysfs(tmp_path, freqs):
    for i, f in enumerate(freqs):
        d = tmp_path / "cpu" / f"cpu{i}" / "cpufreq"
        d.mkdir(parents=True)
        (d / "cpuinfo_max_freq").write_text(str(f))
    return str(tmp_path / "cpu")


def test_core_layout_por_modelo_de_cpu(tmp_path):
    # 1 X3 + 3 A715 + 4 A510 (Dimensity / Snapdragon tipicos): 4 nucleos rapidos
    info = _cpuinfo(tmp_path, ["0xd46"] * 4 + ["0xd4d"] * 3 + ["0xd4e"])
    assert at.core_layout(info, str(tmp_path / "nada")) == {"total": 8, "fast": 4, "source": "cpuinfo"}


def test_core_layout_por_frecuencia(tmp_path):
    sysfs = _sysfs(tmp_path, [1800000] * 6 + [2200000] * 2)   # 2 A76 + 6 A55
    assert at.core_layout(str(tmp_path / "nada"), sysfs) == {"total": 8, "fast": 2, "source": "cpufreq"}


def test_core_layout_todos_iguales(tmp_path):
    sysfs = _sysfs(tmp_path, [2000000] * 8)
    assert at.core_layout(str(tmp_path / "nada"), sysfs)["fast"] == 4


def test_candidatos():
    assert at.thread_candidates({"fast": 4, "total": 8}) == [3, 4, 6]
    assert at.thread_candidates({"fast": 2, "total": 8}) == [2, 4]
    assert at.thread_candidates({"fast": 8, "total": 8}) == [7, 8]


def test_throttle():
    assert at.throttle(4, {}) == (4, "")
    assert at.throttle(4, {"thermal": 3})[0] == 2
    assert at.throttle(4, {"thermal": 2})[0] == 3
    assert at.throttle(4, {"battery": 10, "charging": False})[0] == 2
    assert at.throttle(4, {"battery": 10, "charging": True}) == (4, "")
    assert at.throttle(4, {"power_save": True})[0] == 2


def test_estado_del_movil_caducado(tmp_path):
    f = tmp_path / "st.json"
    f.write_text(json.dumps({"thermal": 3, "t": int(time.time() * 1000)}))
    assert at.device_state(f)["thermal"] == 3
    f.write_text(json.dumps({"thermal": 3, "t": int((time.time() - 3600) * 1000)}))
    assert at.device_state(f) == {}


def test_consejo_de_modelo():
    cat = [{"id": "qwen3-vl-2b-thinking", "min_ram_gb": 3, "name": "Qwen 2B"}, {"id": "qwen3-vl-4b-thinking", "min_ram_gb": 10}]
    assert at.model_advice("qwen3-vl-4b-thinking", 2.5, 12, cat)["id"] == "qwen3-vl-2b-thinking"
    assert at.model_advice("qwen3-vl-2b-thinking", 20, 12, cat)["id"] == "qwen3-vl-4b-thinking"
    assert at.model_advice("qwen3-vl-2b-thinking", 20, 8, cat) is None      # no hay RAM para el 4B
    assert at.model_advice("qwen3-vl-2b-thinking", 8, 12, cat) is None
    # modelos sin razonamiento de versiones anteriores: se propone su equivalente con razonamiento
    legacy = at.model_advice("qwen3-vl-2b", 8, 12, cat)
    assert legacy["id"] == "qwen3-vl-2b-thinking" and "razona" in legacy["reason"]
    assert at.model_advice("smolvlm-500m", 20, 12, cat)["id"] == "qwen3-vl-2b-thinking"


class _Monitor:
    def __init__(self):
        self.llm_calls, self.settings = [], {"n_threads": 4}


def _services(tmp_path, monkeypatch, layout):
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "nexus"))
    monkeypatch.setenv("NEXUS_DEVICE_STATE", str(tmp_path / "device_state.json"))
    monkeypatch.setenv("NEXUS_RUNTIME_LLAMA", "/libs/libllama-server-i8mm.so")
    s = SimpleNamespace(profile={"llm": {"n_threads": layout["fast"], "n_ctx": 4096}, "ram_gb": 8},
                        monitor=_Monitor(), llm=SimpleNamespace(inflight=0), tuning_reasons=[])
    s.models = SimpleNamespace(active=lambda: {"llm": "qwen3-vl-4b-thinking"}, catalog=SimpleNamespace(get=lambda k: None),
                               recommended=lambda: [{"id": "qwen3-vl-2b-thinking", "min_ram_gb": 3, "installed": False}])
    return s, at.LiveTuner(s, layout=layout)


def _answer(s, tps, n=at.MIN_SAMPLES):
    for _ in range(n):
        s.monitor.llm_calls.append({"backend": "llamaserver", "tokens": 100, "seconds": 100 / tps + 1, "ttft": 1.0,
                                    "tps": 0})


def test_mide_hilos_y_se_queda_con_el_mejor(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 4, "total": 8})
    t.step()
    assert s.profile["llm"]["n_threads"] == 3          # empieza midiendo el primer candidato
    _answer(s, 6.0)
    t.step()
    assert s.profile["llm"]["n_threads"] == 4
    _answer(s, 9.0)
    t.step()
    assert s.profile["llm"]["n_threads"] == 6
    _answer(s, 7.0)
    r = t.step()
    assert s.profile["llm"]["n_threads"] == 4 and r["best"] == 4 and not r["exploring"]
    assert r["tps"] == {"3": 6.0, "4": 9.0, "6": 7.0}
    # lo aprendido se conserva en el siguiente arranque
    s2, t2 = _services(tmp_path, monkeypatch, {"fast": 4, "total": 8})
    s2.profile["llm"]["n_threads"] = 4
    t2.step()
    assert s2.profile["llm"]["n_threads"] == 4


def test_no_cambia_hilos_durante_una_respuesta(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 4, "total": 8})
    s.llm.inflight = 1
    t.step()
    assert s.profile["llm"]["n_threads"] == 4
    s.llm.inflight = 0
    t.step()
    assert s.profile["llm"]["n_threads"] == 3


def test_calor_reduce_hilos_y_no_mide(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 4, "total": 4})
    for n, tps in ((3, 6.0), (4, 9.0)):
        t.step()
        _answer(s, tps)
    t.step()
    assert s.profile["llm"]["n_threads"] == 4
    (tmp_path / "device_state.json").write_text(json.dumps({"thermal": 3, "t": time.time()}))
    r = t.step()
    assert s.profile["llm"]["n_threads"] == 2 and "caliente" in r["throttled"]
    _answer(s, 1.0)            # con el movil caliente no cuenta como medida
    t.step()
    (tmp_path / "device_state.json").write_text(json.dumps({"thermal": 0, "t": time.time()}))
    r = t.step()
    assert s.profile["llm"]["n_threads"] == 4 and r["tps"]["4"] == 9.0
    assert any("normales" in x for x in s.tuning_reasons)


def test_consejo_si_va_lento(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 2, "total": 2})
    t.step()
    _answer(s, 2.0)
    r = t.step()
    assert r["advice"]["id"] == "qwen3-vl-2b-thinking"


def test_vuelve_a_medir_al_cambiar_de_modelo(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 2, "total": 2})
    t.step()
    _answer(s, 5.0)
    assert t.step()["tps"] == {"2": 5.0}
    s.models.active = lambda: {"llm": "qwen3-vl-2b-thinking"}
    assert t.step()["tps"] == {}


@pytest.mark.parametrize("ram,ctx", [(4, 4096), (12, 4096)])
def test_perfil_android_contexto_moderado(monkeypatch, ram, ctx):
    from nexus.hardware.detect import HardwareInfo
    from nexus.hardware.profiles import build_profile
    monkeypatch.setenv("NEXUS_ANDROID", "1")
    monkeypatch.setattr(at, "core_layout", lambda *a: {"total": 8, "fast": 4, "source": "cpuinfo"})
    hw = HardwareInfo.from_dict({"os": "Linux", "os_version": "", "disk_free_gb": 50, "arch": "aarch64", "cpu_name": "arm", "cores_physical": 8,
                                 "cores_logical": 8, "ram_gb": ram, "cpu_flags": [], "gpus": [], "npus": []})
    llm = build_profile(hw)["llm"]
    assert llm["n_threads"] == 4 and llm["live_threads"] and llm["thread_candidates"] == [3, 4, 6]
    assert llm["n_ctx"] <= ctx and llm["max_ctx"] <= 8192


def test_aceptar_consejo_descarga_y_activa(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 2, "total": 2})
    catalog, events = {}, []
    s.models.catalog = SimpleNamespace(get=catalog.get)
    s.models.download_recommended = lambda mid: events.append(("descarga", mid)) or "j1"
    s.models.activate = lambda mid: events.append(("activa", mid))
    s.llm.rebuild = lambda: events.append(("rebuild",))
    assert t.switch_model("qwen3-vl-2b-thinking") == {"job": "j1"}
    t.step()
    assert events == [("descarga", "qwen3-vl-2b-thinking")]          # aun descargando
    catalog["qwen3-vl-2b-thinking"] = {"id": "qwen3-vl-2b-thinking"}
    r = t.step()
    assert ("activa", "qwen3-vl-2b-thinking") in events and r["pending_switch"] is None
    with pytest.raises(ValueError):
        t.switch_model("otro")


def test_throttle_redondea_hacia_arriba():
    assert at.throttle(3, {"thermal": 4})[0] == 2
    assert at.throttle(1, {"thermal": 4})[0] == 1


def test_precarga_solo_si_no_gasta_de_mas():
    assert at.warmup_ok({}, 4, 1.5) == (True, "")
    assert not at.warmup_ok({"thermal": 2}, 4, 1.5)[0]
    assert not at.warmup_ok({"battery": 20, "charging": False}, 4, 1.5)[0]
    assert at.warmup_ok({"battery": 20, "charging": True}, 4, 1.5)[0]
    assert not at.warmup_ok({"power_save": True}, 4, 1.5)[0]
    assert not at.warmup_ok({}, 1.5, 1.5)[0]


def test_tras_el_arranque(tmp_path, monkeypatch):
    s, t = _services(tmp_path, monkeypatch, {"fast": 4, "total": 8})
    monkeypatch.setenv("TMPDIR", str(tmp_path / "tmp"))
    old = tmp_path / "tmp" / "viejo.part"
    old.parent.mkdir()
    old.write_text("x")
    os.utime(old, (time.time() - 3 * 86400,) * 2)
    (tmp_path / "tmp" / "nuevo.part").write_text("x")
    sql = []
    s.db = SimpleNamespace(execute=lambda q: sql.append(q))
    s.llm.select = lambda: ("builtin", "")
    done = at.after_start(s, t)
    assert not old.exists() and (tmp_path / "tmp" / "nuevo.part").exists()
    assert "PRAGMA optimize" in sql and any("hilos" in d for d in done)
    assert any("Optimizacion tras el arranque" in r for r in s.tuning_reasons)
