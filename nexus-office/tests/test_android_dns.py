"""Resolvedor DNS propio de la app Android autonoma (Android no tiene /etc/resolv.conf) y eleccion de llama-server."""
import socket
import struct
import threading

import pytest

from nexus import android_dns, android_main


def _fake_dns(answers: dict[tuple[str, int], list[bytes]]):
    """Servidor DNS UDP minimo en 127.0.0.1: responde con CNAME + A/AAAA usando compresion de nombres."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    stop = threading.Event()

    def serve():
        sock.settimeout(0.2)
        while not stop.is_set():
            try:
                q, addr = sock.recvfrom(512)
            except OSError:
                continue
            qid = q[:2]
            i, labels = 12, []
            while q[i]:
                labels.append(q[i + 1:i + 1 + q[i]].decode())
                i += q[i] + 1
            qtype = struct.unpack("!H", q[i + 1:i + 3])[0]
            question = q[12:i + 5]
            name = ".".join(labels)
            recs = answers.get((name, qtype))
            if recs is None:
                sock.sendto(qid + struct.pack("!HHHHH", 0x8183, 1, 0, 0, 0) + question, addr)
                continue
            body = b""
            # un CNAME delante (como hacen las CDN): el cliente debe quedarse solo con las direcciones
            cname = b"\x03cdn\xc0\x0c"
            body += b"\xc0\x0c" + struct.pack("!HHIH", 5, 1, 60, len(cname)) + cname
            for r in recs:
                body += b"\xc0\x0c" + struct.pack("!HHIH", qtype, 1, 120, len(r)) + r
            sock.sendto(qid + struct.pack("!HHHHH", 0x8180, 1, 1 + len(recs), 0, 0) + question + body, addr)

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    return sock.getsockname()[1], lambda: (stop.set(), t.join(), sock.close())


@pytest.fixture
def dns(monkeypatch):
    port, close = _fake_dns({
        ("huggingface.co", 1): [socket.inet_aton("18.0.0.7")],
        ("huggingface.co", 28): [socket.inet_pton(socket.AF_INET6, "2600::7")],
        ("solo4.example", 1): [socket.inet_aton("10.1.2.3"), socket.inet_aton("10.1.2.4")],
    })
    monkeypatch.setenv("NEXUS_DNS", "127.0.0.1")
    monkeypatch.setattr(android_dns, "FALLBACK", ())
    monkeypatch.setattr(android_dns, "TIMEOUT", 0.5)
    android_dns._cache.clear()
    real_ask = android_dns._ask
    monkeypatch.setattr(android_dns, "_ask", lambda srv, name, qtype, p=53: real_ask(srv, name, qtype, port))
    yield port
    close()
    android_dns._cache.clear()


def test_resolve_follows_cname_and_both_families(dns):
    assert android_dns.resolve("huggingface.co") == ["18.0.0.7", "2600::7"]
    assert android_dns.resolve("solo4.example", socket.AF_INET) == ["10.1.2.3", "10.1.2.4"]
    assert android_dns.resolve("no-existe.example") == []


def test_getaddrinfo_replacement(dns):
    res = android_dns.getaddrinfo("huggingface.co", 443, socket.AF_INET, socket.SOCK_STREAM)
    assert [r[4] for r in res] == [("18.0.0.7", 443)]
    # IP literales y localhost no consultan ningun DNS
    assert android_dns.getaddrinfo("127.0.0.1", 80, socket.AF_INET, socket.SOCK_STREAM)[0][4] == ("127.0.0.1", 80)
    assert ("127.0.0.1", 8775) in [r[4][:2] for r in android_dns.getaddrinfo("localhost", 8775, 0, socket.SOCK_STREAM)]
    with pytest.raises(socket.gaierror):
        android_dns.getaddrinfo("no-existe.example", 80)


def test_install_only_without_resolv_conf(monkeypatch):
    orig = socket.getaddrinfo
    monkeypatch.setattr(android_dns, "needed", lambda: False)
    assert android_dns.install() is False and socket.getaddrinfo is orig
    monkeypatch.setattr(socket, "getaddrinfo", orig)
    monkeypatch.setattr(socket, "gethostbyname", socket.gethostbyname)
    monkeypatch.setattr(android_dns, "needed", lambda: True)
    assert android_dns.install() is True and socket.getaddrinfo is android_dns.getaddrinfo


def test_llama_variant_matches_cpu(tmp_path):
    for n in ("libllama-server.so", "libllama-server-dotprod.so", "libllama-server-i8mm.so"):
        (tmp_path / n).write_bytes(b"\x7fELF")
    pick = lambda feats: android_main.llama_binary(str(tmp_path), set(feats)).name  # noqa: E731
    assert pick(["fp", "asimd"]) == "libllama-server.so"
    assert pick(["asimd", "asimddp", "asimdhp"]) == "libllama-server-dotprod.so"
    assert pick(["asimd", "asimddp", "i8mm"]) == "libllama-server-i8mm.so"
    (tmp_path / "libllama-server-i8mm.so").unlink()
    assert pick(["asimddp", "i8mm"]) == "libllama-server-dotprod.so"
    assert android_main.llama_binary("", {"asimddp"}) is None


def test_cpu_features_parse(tmp_path):
    f = tmp_path / "cpuinfo"
    f.write_text("processor\t: 0\nFeatures\t: fp asimd evtstrm aes asimddp i8mm\nCPU part\t: 0xd41\n")
    assert {"asimddp", "i8mm", "fp"} <= android_main.cpu_features(str(f))
