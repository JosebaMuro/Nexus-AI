"""Deteccion de GPUs/NPUs en Linux sobre un /sys simulado (no depende del hardware de la maquina de tests)."""
import importlib
import os

import pytest

from nexus.hardware import build_profile
from nexus.hardware.detect import GPU, HardwareInfo, _parse_nvidia_smi

det = importlib.import_module("nexus.hardware.detect")   # el paquete tapa el modulo con la funcion detect()
# el /sys simulado usa ':' en los nombres y enlaces simbolicos, que Windows no permite
sysfs = pytest.mark.skipif(os.name == "nt", reason="el /sys simulado necesita un sistema de archivos POSIX")


def _card(sysroot, card, slot, vendor, driver, vram_bytes=None, bar_bytes=None):
    pci = sysroot / "devices" / "pci0000:00" / slot
    pci.mkdir(parents=True, exist_ok=True)
    (pci / "vendor").write_text(f"0x{vendor}\n")
    drv = sysroot / "bus" / "pci" / "drivers" / driver
    drv.mkdir(parents=True, exist_ok=True)
    if not (pci / "driver").exists():
        os.symlink(drv, pci / "driver")
    if vram_bytes is not None:
        (pci / "mem_info_vram_total").write_text(str(vram_bytes))
    if bar_bytes is not None:
        (pci / "resource").write_text(f"0x0000006000000000 0x{0x6000000000 + bar_bytes - 1:016x} 0x000000000014220c\n")
    drm = sysroot / "class" / "drm"
    drm.mkdir(parents=True, exist_ok=True)
    (drm / card).mkdir()
    os.symlink(pci, drm / card / "device")
    return pci


def _info():
    return HardwareInfo(os="Linux", os_version="", arch="x86_64", cpu_name="CPU", cores_physical=8, cores_logical=16,
                        cpu_flags=["avx2"], ram_gb=32, disk_free_gb=100)


def _no_tools(monkeypatch, lspci=""):
    monkeypatch.setattr(det, "_which", lambda tool, extra=(): "/usr/bin/lspci" if tool == "lspci" and lspci else None)
    monkeypatch.setattr(det, "_run", lambda cmd, timeout=15: lspci)


@sysfs
def test_linux_amd_igpu_and_dgpu_with_lspci(tmp_path, monkeypatch):
    lspci = ('0000:03:00.0 "VGA compatible controller" "Advanced Micro Devices, Inc. [AMD/ATI]" '
             '"Navi 31 [Radeon RX 7900 XT/7900 XTX]" -rc8 "Sapphire" "Nitro+"\n'
             '0000:7a:00.0 "Display controller" "Advanced Micro Devices, Inc. [AMD/ATI]" "Phoenix1" -rc4 "Lenovo" "x"\n')
    _no_tools(monkeypatch, lspci)
    _card(tmp_path, "card1", "0000:03:00.0", "1002", "amdgpu", vram_bytes=24 * 1024**3)
    _card(tmp_path, "card2", "0000:7a:00.0", "1002", "amdgpu", vram_bytes=4 * 1024**3)   # APU con 4 GB de UMA
    (tmp_path / "class" / "drm" / "card1-DP-1").mkdir()                                   # conector: se ignora
    info = _info()
    det._detect_linux(info, tmp_path)
    assert [(g.name.split()[-1], g.integrated, g.vram_gb) for g in info.gpus] == [
        ("XTX]", False, 24.0), ("Phoenix1", True, 4.0)]
    assert build_profile(info)["accelerator"] == "rocm"


@sysfs
def test_linux_without_lspci_and_card10(tmp_path, monkeypatch):
    _no_tools(monkeypatch)
    _card(tmp_path, "card10", "0000:00:02.0", "8086", "i915")
    info = _info()
    det._detect_linux(info, tmp_path)
    assert len(info.gpus) == 1 and info.gpus[0].vendor == "intel" and info.gpus[0].integrated


@sysfs
def test_linux_intel_arc_vram_from_bar(tmp_path, monkeypatch):
    _no_tools(monkeypatch, '0000:03:00.0 "VGA compatible controller" "Intel Corporation" "DG2 [Arc A770]" -r08 "x" "y"\n')
    _card(tmp_path, "card0", "0000:03:00.0", "8086", "i915", bar_bytes=16 * 1024**3)
    info = _info()
    det._detect_linux(info, tmp_path)
    g = info.gpus[0]
    assert not g.integrated and g.vram_gb == 16.0
    assert build_profile(info)["accelerator"] == "vulkan"


@sysfs
def test_linux_nvidia_without_nvidia_smi_is_listed_but_not_cuda(tmp_path, monkeypatch):
    _no_tools(monkeypatch)
    _card(tmp_path, "card0", "0000:01:00.0", "10de", "nouveau")
    info = _info()
    det._detect_linux(info, tmp_path)
    det._detect_nvidia(info)
    assert [(g.vendor, g.driver) for g in info.gpus] == [("nvidia", "nouveau")]
    assert build_profile(info)["accelerator"] == "cpu"


def test_nvidia_smi_old_driver_without_compute_cap(monkeypatch):
    calls = []

    def fake_run(cmd, timeout=15):
        calls.append(cmd)
        return "" if "compute_cap" in cmd[1] else "NVIDIA GeForce GTX 1060 6GB, 6144, 470.256.02\n"
    monkeypatch.setattr(det, "find_nvidia_smi", lambda: "/usr/bin/nvidia-smi")
    monkeypatch.setattr(det, "_run", fake_run)
    info = _info()
    info.gpus.append(GPU("nvidia", "NVIDIA GPU", 0, False, "nvidia", source="sysfs"))
    det._detect_nvidia(info)
    assert len(calls) == 2 and len(info.gpus) == 1
    assert info.gpus[0].vram_gb == 6.0 and info.gpus[0].driver == "470.256.02"
    assert build_profile(info)["accelerator"] == "cuda"


def test_nvidia_smi_na_memory():
    gpus = _parse_nvidia_smi("NVIDIA GB10, [N/A], 580.95.05, 12.1\n")
    assert gpus[0].name == "NVIDIA GB10" and gpus[0].vram_gb == 0.0


@sysfs
def test_linux_npu_accel(tmp_path, monkeypatch):
    _no_tools(monkeypatch)
    dev = tmp_path / "devices" / "npu"
    dev.mkdir(parents=True)
    drv = tmp_path / "drivers" / "intel_vpu"
    drv.mkdir(parents=True)
    os.symlink(drv, dev / "driver")
    (tmp_path / "class" / "accel" / "accel0").mkdir(parents=True)
    os.symlink(dev, tmp_path / "class" / "accel" / "accel0" / "device")
    info = _info()
    det._detect_linux(info, tmp_path)
    assert info.npus[0].vendor == "intel" and info.npus[0].runtime_hint == "openvino"
