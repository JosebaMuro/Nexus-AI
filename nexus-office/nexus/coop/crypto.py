"""Cifrado de la LAN virtual: AES-256-GCM con clave derivada (scrypt) de la frase de red.

Sin la frase, un nodo no puede leer ni inyectar trafico. Cada trama lleva nonce aleatorio,
marca de tiempo e id unico (proteccion anti-repeticion en ``node.py``).
"""
from __future__ import annotations

import base64
import hashlib
import json
import os

from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def derive(network_id: str, passphrase: str) -> tuple[bytes, str]:
    key = hashlib.scrypt(passphrase.encode(), salt=("nexus-lan:" + network_id).encode(), n=2**14, r=8, p=1, dklen=32)
    tag = hashlib.sha256(b"tag" + key).hexdigest()[:16]   # identificador publico de la red (no revela la clave)
    return key, tag


class Cipher:
    def __init__(self, network_id: str, passphrase: str) -> None:
        self.key, self.tag = derive(network_id, passphrase)
        self.aead = AESGCM(self.key)

    def seal(self, msg: dict) -> str:
        nonce = os.urandom(12)
        ct = self.aead.encrypt(nonce, json.dumps(msg, ensure_ascii=False).encode(), self.tag.encode())
        return json.dumps({"v": 1, "net": self.tag, "n": base64.b64encode(nonce).decode(),
                           "c": base64.b64encode(ct).decode()})

    def open(self, frame: str) -> dict:
        env = json.loads(frame)
        if env.get("v") != 1 or env.get("net") != self.tag:
            raise ValueError("trama de otra red")
        pt = self.aead.decrypt(base64.b64decode(env["n"]), base64.b64decode(env["c"]), self.tag.encode())
        return json.loads(pt)


def make_invite(network_id: str, passphrase: str, peers: list[str]) -> str:
    raw = json.dumps({"net": network_id, "key": passphrase, "peers": peers}, separators=(",", ":")).encode()
    return "NEXUS1:" + base64.urlsafe_b64encode(raw).decode().rstrip("=")


def read_invite(code: str) -> dict:
    code = code.strip()
    if not code.startswith("NEXUS1:"):
        raise ValueError("codigo de invitacion no valido")
    body = code[7:]
    return json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
