# Red cooperativa (LAN virtual por internet)

Varias instancias de Nexus-Office forman una **malla cifrada**: se ven como si estuvieran en la misma red local, aunque estén en casas o países distintos.

> Para usar **tu** Nexus-Office desde el móvil o el navegador fuera de casa, consulta [Acceso remoto](ACCESO_REMOTO.md) (TLS 1.3 y dispositivos emparejados). Esta red sirve para conectar **varias instancias de Nexus-Office** entre sí.

## Cómo funciona
- **Transporte**: WebSocket en un puerto propio (8766 por defecto), separado de la interfaz web, que sigue escuchando solo en `127.0.0.1`.
- **Cifrado**: AES‑256‑GCM con una clave derivada por *scrypt* de la frase de red. Sin la frase no se puede leer ni inyectar tráfico; las conexiones que envían tramas inválidas se cierran.
- **Anti‑repetición**: cada trama lleva id único y marca de tiempo (ventana de 5 minutos).
- **Malla**: los mensajes se reenvían con TTL y deduplicación, así que un nodo detrás de NAT solo necesita conectar **hacia fuera** a cualquier nodo accesible; a partir de ahí ve a todos.
- **IP virtual**: cada nodo obtiene una dirección estable `10.77.x.y` derivada de su id y de la red.

## Servicios
| Servicio | Control |
|---|---|
| Presencia (nombre, IP virtual, hardware, modelo, agentes) | siempre |
| Chat de la LAN | siempre |
| Inferencia remota: usar el LLM de otro nodo (`peer:NOMBRE`) | `coop.allow_remote_inference` |
| Agentes remotos | `coop.allow_remote_agents` (desactivado por defecto); sus herramientas usan supervisión reforzada |
| Memoria compartida (hechos) | `coop.share_memory` (desactivado por defecto) |
| Herramientas compartidas | `coop.share_tools`; llegan como **propuestas** que hay que revisar |

## Puesta en marcha
1. **Nodo anfitrión** (admin): *Red → Crear red*. Activa *Aceptar conexiones entrantes* y haz el puerto 8766 accesible (ver abajo). Escribe sus URLs públicas (`ws://IP:8766`) y pulsa *Invitación*.
2. **Resto de nodos**: pegan el código `NEXUS1:…` en *Unirse* y pulsan *Activar*.
3. En la lista de nodos: *Ping* y *Usar su IA* (por ejemplo, un portátil sin GPU usando el modelo grande del PC de sobremesa).

> El código de invitación **contiene la clave de cifrado**: compártelo solo con quien deba entrar en tu red.

## Hacer accesible un nodo
- **Tailscale** o **ZeroTier** (recomendado): Nexus-Office detecta sus direcciones (100.64.0.0/10 y redes ZeroTier) y las muestra; usa `ws://100.x.y.z:8766`.
- Redirección del puerto 8766 en el router hacia el PC anfitrión.
- Cualquier túnel TCP/WebSocket (por ejemplo cloudflared) que apunte al puerto 8766.
Solo hace falta **un** nodo accesible; los demás pueden estar detrás de NAT.
