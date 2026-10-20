import socket
import time
from types import SimpleNamespace

import pytest

from nexus.config import Config
from nexus.coop import Cipher, CoopNode, make_invite, read_invite, virtual_ip


def test_cipher_roundtrip_and_wrong_key():
    a, b = Cipher("red", "clave-secreta"), Cipher("red", "otra")
    frame = a.seal({"hola": "mundo"})
    assert a.open(frame) == {"hola": "mundo"}
    with pytest.raises(Exception):
        b.open(frame)


def test_invite_and_virtual_ip():
    code = make_invite("red", "k", ["ws://1.2.3.4:8766"])
    assert read_invite(code) == {"net": "red", "key": "k", "peers": ["ws://1.2.3.4:8766"]}
    ip = virtual_ip("red", "nodo1")
    assert ip.startswith("10.77.") and ip == virtual_ip("red", "nodo1") != virtual_ip("red", "nodo2")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FakeLLM:
    def status(self):
        return {"backend": "builtin", "model": "fake"}

    def stream(self, messages, spec="default", **opts):
        for w in ("respuesta ", "desde ", "el ", "nodo"):
            yield w


def fake_services(name, data_dir, **coop):
    cfg = Config.__new__(Config)
    import threading
    cfg._lock = threading.RLock()
    from nexus.config import DEFAULTS, _deep_merge
    cfg.data = _deep_merge(DEFAULTS, {"coop": {"network_id": "test-net", "passphrase": "s3cr3t", "node_name": name, **coop}})
    cfg.save = lambda: None
    profile = {"tier": "ligero", "accelerator": "cpu", "device_name": "cpu", "ram_gb": 8, "vram_gb": 0}
    return SimpleNamespace(config=cfg, profile=profile, llm=FakeLLM(), agents=SimpleNamespace(list=lambda: []))


def wait(cond, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if cond():
            return True
        time.sleep(0.1)
    return False


def test_two_nodes_mesh_chat_and_remote_inference(data_dir, monkeypatch):
    port = free_port()
    a = CoopNode(fake_services("alfa", data_dir, listen=True, listen_host="127.0.0.1", listen_port=port))
    monkeypatch.setattr("nexus.coop.node.node_identity", lambda: {"node_id": "beta000000000000"})
    b = CoopNode(fake_services("beta", data_dir, peers=[f"ws://127.0.0.1:{port}"]))
    events = []
    a.listeners.append(events.append)
    a.start()
    time.sleep(0.5)
    b.start()
    try:
        assert wait(lambda: a.peers and b.peers), "los nodos no se descubrieron"
        assert b.peers[a.node_id]["name"] == "alfa" and b.peers[a.node_id]["vip"].startswith("10.77.")
        b.send_chat("hola red")
        assert wait(lambda: any(e["event"]["type"] == "lan_chat" for e in events))
        text = "".join(b.remote_llm("alfa", [{"role": "user", "content": "hola"}], {}))
        assert text == "respuesta desde el nodo"
        assert b.ping("alfa") >= 0
    finally:
        b.stop()
        a.stop()


def test_intruder_without_key_is_rejected(data_dir):
    port = free_port()
    a = CoopNode(fake_services("alfa", data_dir, listen=True, listen_host="127.0.0.1", listen_port=port))
    a.start()
    time.sleep(0.5)
    try:
        import asyncio

        from websockets.asyncio.client import connect

        async def intrude():
            async with connect(f"ws://127.0.0.1:{port}") as ws:
                await ws.recv()  # hello cifrado del nodo
                await ws.send(Cipher("test-net", "clave-falsa").seal({"type": "chat"}))
                try:
                    await asyncio.wait_for(ws.recv(), 3)
                except Exception as e:  # noqa: BLE001
                    return type(e).__name__
            return "abierto"

        assert asyncio.run(intrude()) != "abierto"
        assert not a.peers
    finally:
        a.stop()
