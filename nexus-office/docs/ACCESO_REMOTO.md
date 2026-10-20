# Acceso remoto por internet

Usa tu Nexus-Office desde cualquier lugar (datos móviles, otra Wi‑Fi, otro país) con el móvil, la tablet o un navegador, cifrado de extremo a extremo y solo para los dispositivos que tú emparejes.

> **Alternativa con la app Android**: la **VPN cifrada WireGuard** (app Nexus-Office VPN). Solo hay que abrir un puerto UDP, Nexus-Office no queda publicado en internet y funciona igual con el PC en Windows, Linux o macOS. Ver [VPN.md](VPN.md).

## Cómo funciona

| | Puerto local (sin cambios) | Puerto remoto (nuevo) |
|---|---|---|
| Dirección | `http://IP:8775` | `https://IP:8443` (configurable) |
| Para | este PC y la red de casa (con la clave de red) | internet |
| Cifrado | — (red de confianza) | **TLS 1.3** |
| Acceso | clave de red | **dispositivo emparejado** (llave propia, revocable) |

El puerto remoto exige la llave de un dispositivo emparejado **en cada petición y en cada WebSocket**, venga de donde venga, también desde el propio PC. Así, ni un túnel ni un proxy pueden saltarse la protección. Sin emparejar solo se ve la página de emparejamiento, `/api/ping` y los ficheros de la interfaz, que son código público. Datos, memoria, imágenes generadas y API quedan cerrados.

## El cifrado elegido y por qué

La red cooperativa usa AES‑256‑GCM con una clave fija derivada de la frase de red (scrypt). Para el acceso por internet desde móviles y navegadores hay una combinación mejor equilibrada entre rapidez y seguridad:

| Pieza | Qué se usa | Por qué |
|---|---|---|
| Protocolo | **TLS 1.3 únicamente** (TLS 1.2 y anteriores rechazados) | Saludo de un solo viaje de ida y vuelta, sin algoritmos antiguos. Nativo en navegadores y en la app Android, así que no hace falta cifrado en JavaScript. |
| Intercambio de claves | **X25519MLKEM768**: híbrido post‑cuántico (X25519 + ML‑KEM‑768 del NIST). Clientes antiguos: **X25519** | Claves nuevas en cada conexión (**secreto hacia adelante**): aunque algo se filtrara en el futuro, el tráfico grabado hoy no se puede descifrar, ni siquiera con un ordenador cuántico. |
| Cifrado de datos | **AES‑GCM** (PC, con aceleración AES por hardware) o **ChaCha20‑Poly1305** (móviles sin AES por hardware) | Cada dispositivo usa el más rápido para él: el servidor respeta la preferencia ChaCha20 del cliente. |
| Identidad del servidor | Certificado propio **ECDSA P‑256**, **anclado por su huella SHA‑256** | La app Android solo acepta tu servidor. No hace falta dominio ni autoridad de certificación. |
| Identidad del dispositivo | **Llave aleatoria de 256 bits** por dispositivo, obtenida con un **código de un solo uso** (60 bits, caduca en 10 min). El servidor solo guarda su huella | Revocable dispositivo a dispositivo. Una copia de los datos del servidor no permite entrar. |
| Fuerza bruta | 8 fallos de código o de PIN en 15 minutos bloquean esa IP (no se aplica en el propio PC) | Adivinar un código o un PIN deja de ser viable. |

Medido en el equipo de desarrollo, una vez abierta la conexión: **1,4 ms por petición**. El chat va por un WebSocket cifrado que permanece abierto, así que el cifrado no se nota.

Otros detalles:
- La cookie del dispositivo es `HttpOnly`, `Secure` y `SameSite=Strict`.
- Las respuestas incluyen `X-Frame-Options: DENY`, `nosniff`, `no-referrer` y `no-store`.
- Al apagar, las conexiones se cierran en un máximo de 3 segundos, aunque un móvil se haya quedado sin cobertura.

## Asistente del router (lo más fácil)

*Ajustes → Asistente del router* (o el botón **ASISTENTE DEL ROUTER** en *Red COOP → Acceso remoto*) te guía en cinco pasos:

1. **Acceso remoto en Nexus-Office**: lo activa si hace falta y te avisa si falta reiniciar.
2. **Tu router**:
   - Detecta la puerta de enlace, la IP y el adaptador de este equipo.
   - Identifica la marca cuando puede: Movistar, Vodafone, Orange/Jazztel, Digi, FRITZ!Box, TP‑Link, ASUS, Netgear, Huawei, ZTE, Xiaomi, MikroTik, D‑Link, Linksys o Sagemcom.
   - **Abre la configuración del router** en el navegador. Inicias sesión tú, con la contraseña de la pegatina.
3. **Abrir el puerto**:
   - Si el router admite UPnP, un clic.
   - Si no, el asistente indica dónde está la opción en el menú de tu marca (orientativo; varía según el firmware) y la **regla exacta** con botones para copiar: nombre, protocolo TCP, puerto externo, IP interna de este equipo y puerto interno.
   - También recomienda **reservar la IP** de este equipo en el DHCP del router (te da su MAC), para que la regla no deje de valer.
4. **Comprobar desde internet**:
   - Consulta tu IP pública (ipify) e intenta conectar a ella. También detecta si tu operador usa CGNAT.
   - Muchos routers no permiten esta prueba desde dentro de casa (*hairpin NAT*), así que la prueba definitiva es el móvil con datos móviles.
5. **Conectar la app Android**: genera el código `NEXUSR1:…` para pegarlo en *Conectar desde internet*.

El asistente solo **lee**: el título de la página del router y su descripción UPnP. Nunca inicia sesión ni cambia nada en el router. Los routers que responden con cabeceras no estándar o con TLS antiguo se detectan igualmente.

## Puesta en marcha (modo administrador)

1. **Red COOP → Acceso remoto por internet**: activa *Permitir acceso desde internet* (puerto 8443 o el que elijas) y **reinicia Nexus-Office**. La etiqueta pasa a «escuchando en el puerto 8443».
2. **Haz que llegue desde internet** (una de estas opciones):
   - **Abrir puerto en el router (UPnP)**: Nexus-Office pide al router que redirija el puerto TCP a este PC y muestra la IP pública. *Cerrar puerto* lo deshace.
   - **A mano**: en el router, redirige el puerto TCP 8443 a la IP local de este PC.
   - **Tailscale o ZeroTier**: si ya los usas, no hace falta abrir nada. Nexus-Office muestra su dirección.
   - **Túnel** (Cloudflare Tunnel o similar): apunta el túnel a `https://localhost:8443`, nunca al puerto 8775.
3. Si el panel avisa de **CGNAT** (tu operador comparte la IP pública), abrir puertos no basta: usa Tailscale, ZeroTier o un túnel.
4. **Generar código**, con un nombre opcional para el dispositivo. El código sirve una sola vez y caduca a los 10 minutos.

## Emparejar un dispositivo

- **App Android** (recomendado): *Conectar desde internet* → pega el código de acceso `NEXUSR1:…` (botón COPIAR del panel).
  - El código incluye las direcciones, la huella del certificado y el código de un solo uso.
  - La app ancla la huella, prueba las direcciones (internet, casa o Tailscale) y se empareja sola.
  - Después recuerda las direcciones: con datos móviles usa la de internet y en casa la local.
- **Navegador**: abre el enlace `https://IP:8443/?pair=CÓDIGO`, o la dirección a secas y escribe el código.
  - La primera vez el navegador avisará de que el certificado no es de confianza, porque es tu certificado propio.
  - Antes de continuar, comprueba que la **huella SHA‑256** que muestra el navegador (candado o aviso → certificado) coincide con la del panel. Si no coincide, no continúes.

Los dispositivos emparejados aparecen en el panel con su último acceso e IP. **Revocar** los desconecta al momento.

## Avanzado

- *Regenerar certificado* cambia la huella: las apps tendrán que volver a emparejarse. Úsalo si sospechas que la clave privada se ha copiado (`data/remote/key.pem`).
- Configuración: `remote.enabled`, `remote.port`, `remote.host` (por defecto `0.0.0.0`).
- Datos: `data/remote/` (`cert.pem`, `key.pem`, `devices.json`, este último solo con huellas de las llaves).
- API: ver [API](API.md#acceso-remoto).
