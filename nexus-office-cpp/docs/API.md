# API

Base: `http://127.0.0.1:8775`. Los endpoints de administración requieren la cabecera `X-Admin-Token` (obtenida con `/api/admin/setup` o `/api/admin/login`). Documentación interactiva generada por FastAPI en `/docs`.

## WebSocket `/ws`
Enviar: `{"type": "chat", "text": "...", "agent": "auto", "session_id": "...", "images": ["data:image/jpeg;base64,..."]}`

Eventos recibidos: `session`, `agent`, `state` (`thinking|tool|idle`), `token`, `tool_call`, `tool_result`, `approval_required`, `approval_resolved`, `learned`, `media`, `graph_update`, `done` (`message_id`, `text`), `error`, `lounge`, `coop`.

## REST principal
| Método y ruta | Descripción |
|---|---|
| `GET /api/status` | Estado general (perfil, LLM, voz, memoria, red, pendientes). |
| `GET/PUT /api/config` | Configuración (claves `user.*` en modo usuario; todas en admin). |
| `POST /api/admin/setup` · `login` · `logout` · `pin` · `shutdown`, `GET /api/admin/logs` | Administración. |
| `GET /api/hardware`, `GET /api/monitor`, `POST /api/hardware/redetect` | Hardware, monitor en vivo, re-detección. |
| `GET /api/runtimes`, `POST /api/runtimes/install` | Runtimes nativos. |
| `GET /api/sessions`, `GET/DELETE /api/sessions/{id}` | Conversaciones. |
| `GET/PUT /api/sessions/{id}/hilo` | Hilo de una conversación larga: resumen, notas fijas (`{"notas", "resumen"}`) y contadores. |
| `POST /api/sessions/{id}/hilo/resumir` | Pliega ya en el resumen lo antiguo (`{"todo": true}`: todo menos los dos últimos mensajes). |
| `POST /api/feedback` | `{message_id, score: 1|-1, comment}`. |
| `GET/POST /api/memory`, `GET /api/memory/search?q=`, `DELETE /api/memory/{id}` | Memoria. |
| `GET /api/graph?scope=` (`*` = general con el subgrafo de cada agente, por defecto; vacío = solo el general; id de agente = solo su grafo individual), `GET /api/graph/scopes` (tamaño y documentos de cada grafo), `DELETE /api/graph/{id}` | Grafo de conocimiento. |
| `POST /api/image/generate`, `POST /api/image/analyze` (multipart), `POST /api/music/generate`, `GET /api/outputs` | Multimedia. |
| `GET /api/voice`, `POST /api/voice/tts` (WAV o 204 = usar navegador), `POST /api/voice/stt` (multipart) | Voz. |
| `GET /api/rag`, `GET /api/rag/search?q=`, `GET /api/rag/formats` (formatos admitidos y herramientas de descompresión disponibles), `POST /api/rag/upload` (multipart; documentos y archivos ZIP/RAR/7z/TAR), `POST /api/rag/import` (multipart, varios `files`; devuelve `{results, ok, failed, seconds}`), `POST /api/rag/url` `{url}`, `POST /api/rag/crawl` `{url, max_pages, max_depth, scope}` (rastreo de documentación), `GET /api/rag/{id}/file`, `DELETE /api/rag/{id}`. Destino: campo `agent` en upload, import, url y crawl (vacío = grafo general; id = solo ese agente); `PATCH /api/rag/{id}` `{agent}` lo mueve; `GET /api/rag?agent=` y `/api/rag/search?agent=` filtran como lo vería ese agente | Biblioteca RAG y URL → PDF. |
| `POST /api/avatar/upload` (GLB/GLTF/VRM) | Modelo 3D propio para el avatar. |
| `GET/PUT /api/agents`, `DELETE /api/agents/{id}` | Agentes. |
| `GET /api/lounge`, `POST /api/lounge/meet`, `POST /api/lounge/say`, `POST /api/lounge/proposals/{id}` | Salón. |
| `GET /api/tools`, `POST /api/approvals/{id}`, `GET /api/tools/audit`, `GET/POST/PUT /api/proposals…` | Herramientas. |
| `GET /api/models`, `/recommended`, `/search?source=&q=`, `/files?source=&repo=`; `POST /api/models/download`, `/recommended/{id}`, `/import`, `/activate/{id}`, `/activate-spec` | Modelos. |
| `GET /api/fs/listar?ruta=&todos=`; `POST /api/fs/dialogo {carpeta}` (Admin) | Explorador para importar modelos: carpetas y ficheros de modelo con su formato, y el selector de archivos del sistema (solo desde el mismo equipo). |
| `GET /api/coop`, `POST /api/coop/create|join|start|stop|chat|ping/{n}|agent`, `GET /api/coop/invite` | Red cooperativa. |
| `GET /api/vpn`, `POST /api/vpn/enable` `{enabled, port}`, `POST /api/vpn/peers` `{name, lookup_public_ip}`, `DELETE /api/vpn/peers/{id}`, `GET /api/vpn/server.conf`, `POST /api/vpn/apply`, `POST /api/vpn/upnp` *(admin)* | VPN WireGuard para la app Nexus-Office VPN (ver [VPN.md](VPN.md)). |
| `GET /api/office/profiles`, `POST /api/office/profile` `{id, reset_agents}` *(admin; `id` vacío quita el perfil)* | Perfiles de oficina para pymes (ver [OFICINA.md](OFICINA.md)). |

## Servicio y descargas
| Método y ruta | Descripción |
|---|---|
| `GET /api/service` | Arranque automático: `{kind, registered, supported, admin_os, port}`. `kind`: `systemd`, `cron`, `launchd`, `windows` o `termux`; `registered`: dónde está registrado (vacío si no lo está); `admin_os`: si Nexus-Office corre con permisos de administrador del sistema (con ellos arranca con el equipo, sin ellos al iniciar sesión). En la app Android autónoma devuelve `{kind: "android", supported: false}`: allí lo gestiona la propia app. |
| `POST /api/service` `{action: "instalar"\|"quitar", red}` *(admin)* | Instala o quita el servicio con el puerto de `server.port`. `red: true` escucha en `0.0.0.0`; si no, usa `server.host`. Devuelve `{ok, detail}`; `400` con el motivo si falla. Ver [SERVICIO.md](SERVICIO.md). |
| `GET /api/downloads` | Instaladores de `<datos>/descargas/`: `{folder, host, host_label, files: [{name, path, size, platform, label}]}`. `platform`: `windows-x64`, `windows-arm64`, `linux-x86_64`, `linux-aarch64`, `macos-arm64`, `macos-x86_64`, `android`, `codigo`, `docs` u `otros`; `host` es la del equipo donde corre Nexus-Office. |
| `GET /descargas/{path}` | Descarga un fichero de `<datos>/descargas/` (`path` como lo da `/api/downloads`; las APK con su tipo MIME de Android). `404` si no existe o sale de la carpeta. |
| `GET /api/package/termux` | Solo desde la app Nexus-Office autónomo: `tar.gz` con su `nexus-office` para Android, sus bibliotecas, los recursos e `install-termux.sh`, para instalarlo en Termux. En un PC no hay binario de Android que dar. |

Desde otro dispositivo de la red, estas rutas exigen la clave de red, como todas salvo `/api/ping`.

## Gestor de conocimiento
`scope`: vacío o `general` = grafo general; id de agente = su subgrafo; `*` o sin indicar = todos. Las escrituras exigen modo administrador. Ver [MEMORIA.md](MEMORIA.md).

| Método y ruta | Descripción |
|---|---|
| `GET /api/kg/overview` | Categorías, tamaño de cada grafo por categoría, recuerdos por ámbito, especialidades, embeddings y trabajos. |
| `GET /api/kg/nodes?scope=&q=&category=&type=&limit=&offset=&semantic=` | Buscar nodos. `semantic=true` busca por significado con el modelo de embeddings. |
| `GET /api/kg/nodes/{id}` | Un nodo con sus relaciones. |
| `POST /api/kg/nodes` `{name, type, category, scope, note}` | Crear un nodo. |
| `PATCH /api/kg/nodes/{id}` `{name, type, category, weight, note, scope}` | Editar. Con `scope`, lo mueve a otro grafo; si ya existe allí, se fusionan. |
| `DELETE /api/kg/nodes/{id}` | Borrar un nodo y sus relaciones. |
| `POST /api/kg/merge` `{src, dst}` | Fusionar `src` en `dst` (mismo grafo). |
| `POST /api/kg/relations` `{src, dst, rel, weight}` (ids de nodo), `DELETE /api/kg/relations/{id}` | Relaciones. No se permiten entre dos agentes distintos. |
| `POST /api/kg/import` (multipart `file` o varios `files`, `target`: vacío, id de agente o `auto`) | Insertar un grafo de cualquier formato (se detecta por el contenido); varios ficheros se unen en uno, p. ej. `nodes.csv` + `edges.csv`. Trabajo en segundo plano; el informe trae `format`, `nodes`, `new`, `merged`, `relations` y `by_scope`. Ver [GRAFOS.md](GRAFOS.md). |
| `GET /api/kg/formats` | Formatos de grafo admitidos y sus extensiones. |
| `POST /api/kg/archive` (multipart `file`, `target`=`auto`, `reason`, `rag`, `max_reason`) | Leer, razonar y procesar un ZIP, RAR, 7z o TAR. Trabajo en segundo plano. |
| `GET /api/kg/export?scope=&format=json\|graphml\|csv` | Exportar un grafo. |
| `POST /api/kg/reorganize` `{apply}` | Propuesta de reorganización; con `apply: true`, la aplica. |
| `POST /api/kg/duplicates` `{scope}` | Buscar nodos duplicados. |
| `GET /api/kg/jobs`, `GET /api/kg/jobs/{id}` | Trabajos y su informe. El WebSocket emite `{"type": "kg_job", "job": …}` con cada avance. |
| `GET /api/kg/memories?scope=&kind=`, `PATCH /api/kg/memories/{id}` `{scope}` | Recuerdos por ámbito y moverlos. |
| `GET /api/kg/embeddings`, `POST /api/kg/embeddings/model` `{model}`, `POST /api/kg/embeddings/reindex` | Modelo de embeddings, progreso del indexado, activar y reindexar. |

## Acceso remoto
Puerto remoto: `https://IP:8443` (TLS 1.3). Ahí todas las rutas, salvo `/`, `/api/ping`, `/api/remote/pair` y `/static/…`, exigen la llave del dispositivo, en la cookie `nexus_dev` o en la cabecera `X-Nexus-Device`. El WebSocket `/ws` también la exige. Sin llave se recibe `401`.

| Método y ruta | Descripción |
|---|---|
| `POST /api/remote/pair` `{code, name}` | Canjea un código de un solo uso. Devuelve `{device, token}` y fija la cookie segura. Tras 8 fallos en 15 minutos responde `429`. |
| `GET /api/remote` *(admin)* | Estado: activado, escuchando, huella, direcciones, UPnP, CGNAT, dispositivos. |
| `POST /api/remote/enable` `{enabled, port}` *(admin)* | Activa o desactiva el puerto remoto (requiere reiniciar). |
| `POST /api/remote/pairing` `{name}` *(admin)* | Código de un solo uso (10 minutos), `invite` `NEXUSR1:…` para la app y enlaces `?pair=`. |
| `DELETE /api/remote/devices/{id}` *(admin)* | Revoca un dispositivo y corta sus conexiones. |
| `POST /api/remote/upnp` `{open}` *(admin)* | Abre o cierra el puerto en el router por UPnP y lee la IP pública. |
| `POST /api/remote/public-ip` *(admin)* | IP pública vista desde internet (ipify) y detección de CGNAT. |
| `POST /api/remote/certificate` *(admin)* | Certificado nuevo (cambia la huella anclada). |
| `GET /api/remote/router` *(admin)* | Asistente del router: puerta de enlace, IP y MAC de este equipo, URLs de configuración, marca y ruta del menú, UPnP‑IGD y la regla exacta. |
| `POST /api/remote/check` *(admin)* | IP pública, CGNAT y si el puerto responde en la IP pública. |
