# Guía de administrador / desarrollador

Pulsa **ADMIN**. La primera vez se crea un PIN (mínimo 4 caracteres, guardado como hash PBKDF2‑SHA256 con sal). La sesión de administrador dura 8 horas o hasta volver a USUARIO.

## Qué desbloquea
| Vista | Funciones de administrador |
|---|---|
| Agentes | Crear, editar y borrar agentes: prompt, modelo (`default`, `ollama:…`, `openai:…`, `peer:NODO`), temperatura, tokens, herramientas permitidas, memoria, delegación, palabras clave, color e icono. |
| Salón | Aprobar o rechazar los **agentes propuestos** por el salón. |
| Modelos | Buscar en los 5 repositorios, listar ficheros, descargar cualquier modelo, importar ficheros/carpetas locales, usar modelos externos (Ollama, OpenAI‑compatible, nodo de la red). |
| Herramientas | Política por riesgo, activar/desactivar herramientas, revisar/editar/probar/aprobar/rechazar propuestas de los agentes, compartirlas con la red, auditoría completa. |
| Red | Crear red, generar invitaciones, conexiones entrantes, qué se ofrece a la red, nodos y URLs públicas; **permitir acceso desde la red local** (móvil, tablet, consola) con clave de red. |
| Sistema | Instalar variantes de runtime, **arranque automático** (instalar o quitar el servicio), **actualizar drivers de la GPU** (abre el actualizador oficial), re-detectar hardware, historial de sesiones, registro, **apagado limpio**. El panel **Descargas** (instaladores para la red) lo ven todos. |
| Ajustes | Toda la configuración (`llm`, `memory`, `tools`, `hardware`, `models`, `media`, `coop`, `lounge`, `server`) y cambio de PIN. |

## Configuración destacada (`data/config.yaml`)
```yaml
llm:
  backend_order: [llamaserver, llamacpp, ollama, openai, transformers, builtin]
  default_model: qwen3-vl-2b-thinking      # id del catálogo o "ollama:modelo", "openai:modelo", "peer:nodo"
  openai_base_url: http://127.0.0.1:1234/v1   # LM Studio, vLLM, llama-server externo, OpenRouter...
  reasoning: auto           # auto | off (modelos Thinking: razonan antes de contestar)
  reasoning_budget: 2048    # tokens de razonamiento en el chat (se suman a max_tokens)
  reasoning_budget_tasks: 768   # en tareas internas (resúmenes, extracción)
memory:
  embed_model: qwen3-embedding-0.6b   # embeddings GGUF en CPU (ver MEMORIA.md)
  embed_dims: 0             # recorte Matryoshka para ahorrar RAM (p.ej. 512)
  organizer: {min_affinity: 0.35, margin: 0.03}   # que va al especialista y que al grafo general
  zip_reason_docs: 12       # documentos de un ZIP que razona el LLM
rag:
  crawl_batch_pages: 0      # páginas por lote al rastrear dominios (0 = según la RAM)
  allow_private_urls: false # permitir rastrear la red local / localhost
tools:
  policy: {low: auto, medium: ask, high: admin}   # auto | ask | admin | deny
  sandbox_community_tools: true
hardware:
  profile_override: ""      # cuda | rocm | vulkan | xpu | openvino | metal | cpu
  llama_variant: ""         # forzar variante del runtime llama.cpp
  adaptive_tuning: true
models:
  huggingface_token: ""     # modelos con acceso restringido
  civitai_token: ""
  kaggle_username: ""
  kaggle_key: ""
server: {host: 127.0.0.1, port: 8775}
```

## Acceso remoto por internet
*Red COOP → Acceso remoto*:
- Activar el puerto HTTPS (TLS 1.3).
- Abrir el puerto en el router por UPnP.
- Generar códigos de emparejamiento de un solo uso.
- Revocar dispositivos.

*Ajustes → Asistente del router*: detecta el router, abre su configuración y da la regla exacta para abrir el puerto.

Guía completa: [Acceso remoto](ACCESO_REMOTO.md).

## Autoajuste
En **Sistema → Autoajuste adaptativo** verás qué cambió en cada arranque y por qué, y las pruebas de hilos (tokens/s por número de hilos). Desactívalo con `hardware.adaptive_tuning: false`. Usa **Apagar Nexus-Office** para que la sesión cuente como cierre limpio.

## Runtimes y aceleración
**Sistema → Runtimes nativos** lista las variantes recomendadas (★) y alternativas. Por ejemplo, con iGPU Intel puedes instalar `openvino` o `vulkan` y compararlas: el autoajuste registra el rendimiento de cada sesión. Los instaladores traen llama-server y sd-cli precompilados en `bundled/runtimes/` para CPU, Vulkan y, según la plataforma, CUDA o Metal (marcados «✓ incluido»); Nexus-Office prueba primero la variante de GPU de tu hardware y cae a la CPU si falta el driver. Ver [Plataformas](PLATAFORMAS.md#aceleración-incluida-en-los-instaladores). `python -m nexus setup --auto --all` instala también PyTorch acelerado (MusicGen) si hay GPU.

**App Android autónoma**: no hay que instalar nada. La APK trae llama-server de CPU y otro con GPU y NPU (Hexagon para la NPU de los Snapdragon 8 Gen 2 o posteriores, OpenCL para Adreno, Vulkan para el resto); Nexus-Office detecta CPU, GPU y NPU (`GET /api/hardware`), pone el modelo en la NPU o en la GPU si llama.cpp puede usarlas y el autoajuste (`GET /api/android/autoajuste`, campos `gpu` y `accel`) compara GPU y CPU con respuestas reales y se queda con lo más rápido. Si la NPU falla pasa a la GPU, y si falla la GPU sigue en CPU; lo anota en `data/android_accel.json` (`npu_broken`, `gpu_broken`; para volver a probarlas, borra ese fichero y `data/android_tune.json`). Ver [la app autónoma](../android-autonomo/README.md#cpu-gpu-y-npu).

## Línea de comandos
```bash
python -m nexus run [--host 0.0.0.0] [--port 8775] [--no-browser]
python -m nexus detect                 # informe de hardware + perfil
python -m nexus setup --auto [--all]   # extras según hardware
python -m nexus pull auto|<id>         # modelos recomendados
python -m nexus modelos [--descargar] [--todos]   # evalúa el hardware, elige el modelo de cada tipo y lo descarga si hay internet
python -m nexus servicio instalar|quitar|estado [--red] [--puerto P]   # arranque automático (ver SERVICIO.md)
python -m nexus oficina [<perfil>|--quitar]       # perfil de oficina (ver OFICINA.md)
python -m nexus chat [--agent id]      # chat en terminal
python -m nexus wheels --check         # comprueba que wheelhouse/ instala sin internet
python -m nexus cleanup [--dry-run] [--no-keep]   # quita del instalador lo que este equipo no usa (ver PLATAFORMAS.md)
```

## Modelo de IA elegido al instalar
`python -m nexus modelos` (lo llaman todos los instaladores) lee el perfil de hardware y, de `nexus/models/recommended.py`, toma el modelo marcado como mejor (`best`) de cada tipo: chat y visión (Qwen3‑VL Thinking 2B, 4B u 8B), documentos (Qwen3‑Embedding 0.6B), voz (Piper) e imagen (solo con `--todos`). Imprime el equipo (RAM, VRAM, acelerador, gama), lo elegido y los modelos que caben pero irían lentos. Con `--descargar` comprueba la conexión con Hugging Face y los baja; sin internet solo recomienda. Los instaladores lo controlan con `--modelos=auto|todos|no` / `-Modelos` / `/MODELOS=`. Los modelos descargados se activan solos si no había otro del mismo tipo.

## Servicio (arranque automático)
`nexus/service.py` registra `python -m nexus run --no-browser --host H --port P` en el gestor de servicios del sistema: systemd (del sistema como root, con `User=` el dueño de la carpeta o `SUDO_USER`; de usuario con `loginctl enable-linger` si no), cron `@reboot` sin systemd, launchd (LaunchDaemon como root, LaunchAgent si no), el Programador de tareas de Windows (tarea «Nexus-Office»: al arrancar el equipo como SYSTEM si eres administrador, al iniciar sesión si no; `pythonw.exe`, reinicio ante fallos, sin límite de tiempo y regla de firewall con `--red`) o termux-services + Termux:Boot. Con `sudo`, si `NEXUS_DATA_DIR` apunta a `/root`, el servicio usa los datos del usuario real. Si Nexus-Office ya responde en el puerto, `nexus run` solo abre el navegador.

En la interfaz: *Sistema → Arranque automático* (`GET/POST /api/service`, cambiarlo exige modo administrador). El servicio se instala con los permisos del proceso de Nexus-Office: para que arranque con el equipo, instálalo desde el instalador, con `sudo` o desde una consola de administrador. Todo en [SERVICIO.md](SERVICIO.md).

## Descargas para la oficina
Copia los instaladores (las carpetas `windows/`, `linux/`, `macos/`, `android/` de una entrega) en `data/descargas/` y aparecen en *Sistema → Descargas* (`GET /api/downloads`; ficheros en `/descargas/<ruta>`). `nexus/downloads.py` los clasifica por plataforma según el nombre, marca el del sistema anfitrión y la página marca el del dispositivo que la mira. Desde la red hace falta la clave de red, como en el resto de rutas.

## Construir los instaladores
`python packaging/build.py [--targets …] [--apk …] [--docs …] [--keep]` genera `dist/{windows,linux,macos,android}`, el zip de código fuente y `SHA256SUMS.txt`. Antes hay que preparar `.cache/desktop-runtimes` con `build_python_runtimes.py`, `build_desktop_runtimes.sh` y `build_gpu_runtimes.py`, y tener `makensis` para el `.exe`. Paso a paso en [Plataformas → Empaquetado](PLATAFORMAS.md#empaquetado).

## Datos
Todo vive en `data/` (con el `.deb`, en `~/.local/share/nexus-office`; en general, donde indique `NEXUS_DATA_DIR`): `config.yaml`, `nexus.db` (memoria, grafo, salón, auditoría), `models/`, `runtimes/`, `outputs/`, `agents/*.yaml`, `tools/community|proposals/`, `logs/` (con `servicio.log` del servicio en Windows, macOS y cron), `descargas/` (instaladores para la red), `perf_history.json`, `tuning.json`. Haz copia de esa carpeta para respaldar a Nexus-Office.
