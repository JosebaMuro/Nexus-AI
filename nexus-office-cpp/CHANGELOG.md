# Cambios

## Nexus-Office 2.0.0-beta-cpp (2.0 beta C)

### Nexus-Office reescrito en C++
- **Antes**: Nexus-Office era una aplicación Python que llevaba dentro su propio intérprete y unos 60 paquetes; cada
  instalador ocupaba entre 0,5 y 1,7 GB y tardaba en arrancar.
- **Ahora** hay una edición C++ con la misma interfaz, la misma API, los mismos perfiles, agentes y datos: un único
  programa nativo de unos 15 MB que arranca en 0,13 s y usa 68 MB de memoria en reposo (la edición Python, 1,2 s y
  119 MB en el mismo equipo). Los modelos de IA y los motores (llama-server y stable-diffusion.cpp) son los mismos.
- Las dos ediciones se comparan automáticamente: 39 rutas de la API y 41 flujos completos (administrador, memoria,
  agentes, biblioteca, multimedia, grafo, oficina y chat) dan la misma respuesta.
- Instaladores 100 % offline para Windows (x64 y ARM64), Linux (x86_64 y ARM64: .run, .deb, AppImage y Flatpak),
  macOS (Apple Silicon e Intel) y Android, con servicio del sistema y elección del modelo de IA según el hardware. Se
  instala junto a la edición Python sin pisarla.
- Es una beta: la mesa de bolsa, la red COOP, la VPN, el acceso remoto, importar grafos en otros formatos, el rastreo
  web y las herramientas de GIMP siguen solo en la edición Python. Ver [docs/CPP.md](docs/CPP.md).
