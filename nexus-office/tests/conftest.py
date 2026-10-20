import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))


@pytest.fixture(autouse=True)
def data_dir(tmp_path, monkeypatch):
    """Cada test usa un directorio de datos aislado."""
    monkeypatch.setenv("NEXUS_DATA_DIR", str(tmp_path / "data"))
    from nexus import config
    config.reset_config()
    yield tmp_path / "data"
    config.reset_config()


@pytest.fixture
def services(data_dir):
    from nexus.core import Services
    s = Services(detect_hardware=False)
    yield s
    s.monitor.stop()
    s.db.close()
