"""Certificado propio y contexto TLS 1.3 del acceso remoto.

Por que TLS 1.3 (y no el AES-GCM con clave fija de la red cooperativa) para el acceso por internet:
  * Intercambio efimero en cada conexion -> secreto hacia adelante: aunque se filtrara algo en el futuro,
    el trafico grabado hoy no se puede descifrar. Saludo de 1 ida y vuelta. Con OpenSSL 3.5+ (el del Python
    portable) se negocia X25519MLKEM768, hibrido post-cuantico (X25519 + ML-KEM-768 del NIST), con los
    clientes que lo admiten (Chrome/Edge 131+, Firefox 132+, WebView actual); el resto usa X25519.
  * Cifrado AEAD AES-GCM (acelerado por hardware en PC) o ChaCha20-Poly1305 (mas rapido en moviles
    sin AES por hardware); con SSL_OP_PRIORITIZE_CHACHA el servidor respeta esa preferencia del cliente.
  * Nativo en navegadores y en el WebView de Android: nada de cifrado en JavaScript.
El servidor se autentica con un certificado ECDSA P-256 propio cuya huella SHA-256 se ancla en el
emparejamiento (la app Android la comprueba siempre; en el navegador se verifica una vez a mano).
"""
from __future__ import annotations

import datetime
import hashlib
import ipaddress
import socket
import ssl
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

OP_PRIORITIZE_CHACHA = 0x00200000   # SSL_OP_PRIORITIZE_CHACHA de OpenSSL (Python no lo exporta)


def fingerprint(cert_pem: bytes) -> str:
    der = x509.load_pem_x509_certificate(cert_pem).public_bytes(serialization.Encoding.DER)
    return hashlib.sha256(der).hexdigest().upper()


def pretty(fp: str) -> str:
    return ":".join(fp[i:i + 2] for i in range(0, len(fp), 2))


def _sans(extra: list[str]) -> list[x509.GeneralName]:
    names: list[x509.GeneralName] = [x509.DNSName("localhost"), x509.DNSName(socket.gethostname() or "nexus")]
    seen = set()
    for item in ["127.0.0.1", *extra]:
        if not item or item in seen:
            continue
        seen.add(item)
        try:
            names.append(x509.IPAddress(ipaddress.ip_address(item)))
        except ValueError:
            names.append(x509.DNSName(item))
    return names


def ensure_cert(folder: Path, extra_names: list[str] | None = None, regenerate: bool = False) -> tuple[Path, Path, str]:
    """Devuelve (cert.pem, key.pem, huella). Lo crea la primera vez y lo conserva (la huella anclada no cambia)."""
    folder.mkdir(parents=True, exist_ok=True)
    cert_p, key_p = folder / "cert.pem", folder / "key.pem"
    if regenerate or not (cert_p.exists() and key_p.exists()):
        key = ec.generate_private_key(ec.SECP256R1())
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Nexus-Office"),
                          x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Nexus-Office (certificado propio)")])
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                .serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=3650))
                .add_extension(x509.SubjectAlternativeName(_sans(extra_names or [])), critical=False)
                .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
                .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
                .sign(key, hashes.SHA256()))
        key_p.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                            serialization.NoEncryption()))
        try:
            key_p.chmod(0o600)
        except OSError:
            pass
        cert_p.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    return cert_p, key_p, fingerprint(cert_p.read_bytes())


def harden(ctx: ssl.SSLContext) -> ssl.SSLContext:
    """Solo TLS 1.3 (X25519 + AES-GCM / ChaCha20-Poly1305), sin compresion ni renegociacion."""
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.options |= ssl.OP_NO_COMPRESSION | OP_PRIORITIZE_CHACHA
    return ctx


def server_context(cert: Path, key: Path) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(cert), str(key))
    return harden(ctx)
