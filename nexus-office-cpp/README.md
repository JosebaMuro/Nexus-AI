# Nexus-Office 2.1 (edición C++)

**La oficina de IA local para pymes, reescrita en C++ y con todas las funciones de la edición Python.** Misma interfaz,
mismos agentes por gremio, misma memoria, biblioteca, voz, imagen y música, mesa de bolsa simulada, red COOP, VPN,
importación de grafos, edición de imágenes como en GIMP y el agente Profesor con IA online, en un único programa
nativo sin Python: arranca en una décima de segundo y gasta menos memoria. Funciona entero en tu equipo, sin internet
salvo para descargar los modelos (y, si lo activas, para el Profesor).

Se adapta a cada equipo: elige el modelo y los hilos según la CPU y la RAM, usa la GPU con CUDA, Vulkan o Metal y la
NPU de Intel con OpenVINO, y en Android la NPU Hexagon de los Snapdragon recientes o la GPU (OpenCL en Adreno, Vulkan en
Mali, Immortalis y Xclipse), con autoajuste en vivo. Detalles en [docs/CPP.md](docs/CPP.md).

Trae dos ejemplos en `ejemplos/`: un **grafo multicapa de la Wikipedia en español** para importar como conocimiento y
un **resumen del proyecto** para subirlo a la biblioteca.

## Instalación

Todos los instaladores llevan dentro el programa y los motores de IA (llama-server y sd-cli en sus variantes de CPU,
Vulkan, CUDA o Metal, Piper y Whisper) y **se instalan sin internet**. Al instalar, Nexus-Office evalúa el hardware y
elige el modelo de IA adecuado; lo descarga si hay conexión y, si no, lo deja recomendado para *Modelos → Recomendados*.

| Equipo | Fichero |
|---|---|
| Windows 10/11 x64 | `Nexus-Office-Setup-2.1.0-cpp-windows-x64.exe` |
| Windows 11 ARM (Snapdragon X) | `Nexus-Office-Setup-2.1.0-cpp-windows-arm64.exe` |
| Linux x86_64 | `Nexus-Office-2.1.0-cpp-linux-x86_64.run`, `.AppImage`, `.flatpak` o `nexus-office-c_2.1.0-cpp_amd64.deb` |
| Linux ARM64, Raspberry Pi 4/5 (64 bits) | `Nexus-Office-2.1.0-cpp-linux-aarch64.run`, `.AppImage`, `.flatpak` o `nexus-office-c_2.1.0-cpp_arm64.deb` |
| macOS Apple Silicon / Intel | `Nexus-Office-2.1.0-cpp-macos-arm64.tar.gz` / `-macos-x86_64.tar.gz` (abre `Nexus-Office.command`) |
| Android, Nexus-Office dentro del móvil | `Nexus-Office-2.1.0-cpp-android-autonomo.apk` |
| Android, cliente del PC de la oficina | `Nexus-Office-2.1.0-cpp-android-vpn.apk` |
| Código fuente | `Nexus-Office-2.1.0-cpp-codigo-fuente.zip` |
| Ejemplos | `Nexus-Office-2.1.0-cpp-grafo-wikipedia-es.zip`, `Nexus-Office-resumen.md` |

```bash
Nexus-Office-Setup-2.1.0-cpp-windows-x64.exe /S /OFICINA=herreria /SERVICIO /RED   # Windows, desatendido
sh Nexus-Office-2.1.0-cpp-linux-x86_64.run --oficina=herreria --servicio --red      # Linux y Raspberry Pi
sudo apt install ./nexus-office-c_2.1.0-cpp_amd64.deb && nexus-office-c              # Debian, Ubuntu, Raspberry Pi OS
```

Se instala junto a la edición Python sin pisarla. Después, `nexus-office` (o el acceso directo) abre
`http://127.0.0.1:8775`:

```bash
./nexus-office modelos --descargar        # elegir y descargar el modelo de IA para este equipo
./nexus-office servicio instalar --red    # arrancar con el equipo y aceptar móviles de la red local
./nexus-office oficina herreria           # configurar la oficina de una pyme
./nexus-office --help
```

## Compilar

C++20 y CMake 3.20 (GCC 12, Clang 15, MSVC 2022 o Apple Clang 15). Usa los recursos compartidos de
`../nexus-office/nexus` (interfaz web, perfiles y agentes): son los mismos ficheros en las dos ediciones.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/nexus-office run
```

Instaladores: [packaging/build.py](packaging/build.py), que lanza `.github/workflows/cpp-entrega.yml` en GitHub
Actions para todas las plataformas.

## Documentación

- [Edición C++](docs/CPP.md): qué está igual, cómo está hecha y cómo se prueba.
- [Guía de usuario](docs/USUARIO.md): instalación y uso diario.
- El resto de guías (oficina, administrador, servicio, memoria, RAG, avatar, herramientas, API) son las de la edición
  Python y valen igual: van en la carpeta `docs/` de cada instalador.
