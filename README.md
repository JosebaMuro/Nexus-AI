# Agente IA

Oficina de IA local para pymes y particulares: agentes por gremio, mesa de bolsa con funciones de terminal
financiero, información del mundo por categorías y países, un terminal de IA para programar, memoria, documentos,
imagen, voz y red cooperativa entre equipos. Todo funciona en el propio equipo, sin enviar nada fuera.

**Descargas:** https://josebamuro.github.io/Nexus-AI/ · [última Release](https://github.com/JosebaMuro/Nexus-AI/releases/latest)

- Instaladores **100 % offline** (Python, dependencias y motores de IA dentro) para Windows x64 y ARM64, Linux
  x86_64 y ARM64/Raspberry Pi (`.run`, `.deb`, AppImage y Flatpak), macOS y Android (APK autónoma y APK cliente).
  Agente IA detecta el hardware y activa los motores que funcionan en él: ONNX Runtime GenAI, OpenVINO GenAI,
  LiteRT-LM, ExecuTorch, Genie (Snapdragon) y llama.cpp de respaldo, en CPU, GPU o NPU.
- Script para rehacer una **Ubuntu Server mínima** con Agente IA (o cualquier proyecto Python) preinstalado como
  servicio: [`nexus-office/packaging/ubuntu/`](nexus-office/packaging/ubuntu/LEEME.md).
- Instrucciones por plataforma en [LEEME.txt](LEEME.txt). Manuales en [`nexus-office/docs/`](nexus-office/docs/)
  ([manual de usuario](nexus-office/docs/USUARIO.md), [arquitectura](nexus-office/docs/ARQUITECTURA.md),
  [novedades de la 4.2](nexus-office/docs/NOVEDADES-4.2.md)) y en PDF en cada Release.

| Carpeta | Qué es |
|---|---|
| [`nexus-office/`](nexus-office/) | Agente IA (Python): paquete `nexus`, interfaz web, empaquetado, apps Android y pruebas |
| [`firmware/`](firmware/) | Firmware de las placas NexusLED, NexusWatch y NexusPico, y el grabador `nexus-flash` |

La edición C++ 3.x salió del repositorio en la 4.2.0: su código está en el tag
[`v3.2.3-cpp`](https://github.com/JosebaMuro/Nexus-AI/tree/v3.2.3-cpp), completo en el commit
[`59fa50d`](https://github.com/JosebaMuro/Nexus-AI/tree/59fa50d65d8596fb72236debb35f040783c1b0d7/nexus-office-cpp) y
como zip en la Release v4.2.0.
