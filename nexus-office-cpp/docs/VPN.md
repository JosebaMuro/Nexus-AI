# VPN cifrada para la app Nexus-Office VPN (WireGuard)

La app **Nexus-Office VPN** para Android (`Nexus-Office-2.2.0-android-vpn.apk`) llega al Nexus-Office de tu PC por un túnel **WireGuard**
cifrado. Funciona igual en Windows, Linux, macOS y WSL, en casa y fuera de ella (datos móviles), sin publicar Nexus-Office
en internet.

| Aspecto | Cómo es |
|---|---|
| Cifrado | WireGuard: Curve25519, ChaCha20‑Poly1305 y BLAKE2s, más una **clave precompartida distinta para cada móvil** |
| Qué viaja por el túnel | Solo el tráfico de la app Nexus-Office VPN y solo hacia el PC (`10.78.0.1/32`). El resto del móvil sigue por su red normal. |
| Red del túnel | `10.78.0.0/24`. El PC es `10.78.0.1` y cada móvil recibe la siguiente IP libre. |
| Puerto | UDP 51820, configurable. Es el único que hay que abrir en el router. |
| Claves | Las del PC están en `data/vpn/` (permisos 600). La clave privada de cada móvil solo aparece en su código y Nexus-Office no la guarda. |
| Acceso a Nexus-Office | Dentro del túnel, Nexus-Office sigue pidiendo la **clave de red**, y el código la incluye. |

## Puesta en marcha

Todo está en *Red COOP → VPN cifrada para la app Android*, en modo administrador.

1. **Activar VPN**. Elige el puerto UDP y activa el interruptor. Nexus-Office pasa a escuchar en todas las interfaces, así que **reinícialo**.
2. **Levantar el túnel en el PC**. Pulsa **LEVANTAR / RECARGAR TÚNEL**:
   - Si Nexus-Office tiene permisos de administrador o root, lo hace solo.
   - Si no, muestra los comandos de tu sistema con botón COPIAR. Con **⤓ DESCARGAR nexusvpn.conf** obtienes la configuración del PC.

   | Sistema | Instalar | Levantar |
   |---|---|---|
   | Windows 10/11 | `winget install WireGuard.WireGuard` | `wireguard.exe /installtunnelservice nexusvpn.conf` (PowerShell como administrador) y reglas de firewall UDP |
   | Linux | `sudo apt install wireguard-tools` (dnf o pacman) | `sudo systemctl enable --now wg-quick@nexusvpn` |
   | macOS | `brew install wireguard-tools` o la app WireGuard de la App Store | `sudo wg-quick up nexusvpn.conf` |
   | WSL | `networkingMode=mirrored` en `%UserProfile%\.wslconfig` y `sudo apt install wireguard-tools` | `sudo wg-quick up nexusvpn` y una regla de firewall de Hyper‑V para el UDP |

3. **Fuera de casa**: abre el puerto UDP en el router. El botón **ABRIR UDP … EN EL ROUTER (UPnP)** lo intenta automáticamente; si no funciona, redirige el puerto a mano hacia el PC.
4. **Añadir un móvil**. Escribe un nombre y pulsa **GENERAR CÓDIGO VPN**. Sale un código `NEXUSV1:…`, que puedes copiar o descargar.
   - El código contiene la clave del móvil, por eso **solo se muestra una vez**.
   - También puedes descargar el `.conf` para la app oficial de WireGuard.
   - El túnel del PC se recarga para aceptar el móvil nuevo.
5. **En el móvil**: abre la app Nexus-Office VPN, entra en *Conectar por VPN* y pega el código. Android pide permiso para crear la VPN solo la primera vez.

El código lleva todas las direcciones del PC: IP de la Wi‑Fi y, si lo marcas, la IP pública. La app prueba primero la última que funcionó, así que el mismo código sirve en casa y fuera.

Para retirar un móvil, pulsa **QUITAR** en la lista y recarga el túnel.

## API (solo administrador)

| Método | Ruta | Uso |
|---|---|---|
| GET | `/api/vpn` | Estado, móviles, plataforma e instrucciones por sistema |
| POST | `/api/vpn/enable` | `{enabled, port}`. Devuelve `restart_required` si Nexus-Office debe reiniciarse. |
| POST | `/api/vpn/peers` | `{name, lookup_public_ip}`. Devuelve `code` (`NEXUSV1:`), `client_conf` y `endpoints`. |
| DELETE | `/api/vpn/peers/{id}` | Quita un móvil |
| GET | `/api/vpn/server.conf` | Configuración del PC en formato `wg-quick` |
| POST | `/api/vpn/apply` | Levanta o recarga el túnel, o devuelve los comandos si faltan permisos |
| POST | `/api/vpn/upnp` | Abre el puerto UDP en el router |

## Comprobado

`packaging/test_vpn_e2e.sh`, ejecutado como root en Linux, monta un «PC» y un «móvil» en espacios de red separados:

- El PC levanta el túnel con la `nexusvpn.conf` que genera Nexus-Office.
- El móvil configura WireGuard con el texto exacto que produce la app (`WgConfig.java`).

La prueba comprueba la negociación WireGuard, que `/api/status` responde por el túnel y que sin la clave de red Nexus-Office devuelve 401.

## Problemas frecuentes

- **«Sin respuesta del PC» fuera de casa**: el puerto UDP no está abierto en el router, o tu operador usa CG‑NAT (IP pública compartida). Con CG‑NAT usa Tailscale o ZeroTier, o el acceso remoto HTTPS a través de otro equipo.
- **WSL**: sin `networkingMode=mirrored`, el móvil no puede llegar a la VPN dentro de WSL.
- **Tras añadir o quitar móviles**, recarga el túnel del PC.
