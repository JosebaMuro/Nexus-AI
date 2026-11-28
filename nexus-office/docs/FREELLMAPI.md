# FreeLLMAPI: endpoint con los planes gratis de muchos proveedores

FreeLLMAPI (<https://github.com/tashfeenahmed/freellmapi>, licencia MIT) es la pasarela que usa el proyecto
«9000 tokens». Junta los planes gratis de decenas de proveedores de IA (Groq, Gemini, Mistral, OpenRouter, Cerebras,
NVIDIA…) detrás de un solo endpoint `/v1` compatible con OpenAI. Con el modelo `auto` elige el mejor que tenga
disponible y, cuando uno llega a su límite, pasa al siguiente de la cadena.

Desde la 4.4.0 Agente IA lo lleva dentro y lo puede usar como endpoint LLM, junto a los motores locales.

## Qué va en cada instalador

Los instaladores de escritorio (Windows x64 y ARM64, Linux x86_64 y aarch64, macOS arm64 y x86_64) llevan en
`bundled/runtimes/freellmapi/`:

- `node/`: Node.js 22 oficial de esa plataforma (no hace falta tener Node instalado).
- `app/`: el servidor de FreeLLMAPI ya compilado, su panel web y sus dependencias de producción para ese sistema
  (better-sqlite3 y sharp precompilados).
- `freellmapi.json`: versiones de FreeLLMAPI y de Node, commit y destino.

Se instala sin conexión. Para **responder** necesita internet y las claves gratis de los proveedores, porque las
respuestas vienen de sus servidores. Los modelos locales siguen igual: FreeLLMAPI nunca los sustituye ni entra en el
respaldo automático; solo responde si se elige.

La APK no lo lleva (Node.js no va dentro de la app). Desde el móvil se puede usar el de un PC de la red: en el PC se
activa «accesible en la red local» y en el móvil se pone `http://IP-del-PC:3001/v1`.

## Ponerlo en marcha (Ajustes > FreeLLMAPI)

1. **ARRANCAR LA INCLUIDA**. La primera vez crea su base de datos en `data/freellmapi/` y genera la **clave única**,
   que Agente IA copia sola a sus ajustes.
2. **PANEL WEB**: abre `http://127.0.0.1:3001`. En **Keys** se añaden las claves gratis de los proveedores que se
   quieran usar y en **Fallback Chain** se ordena la cadena de respaldo.
3. **VER MODELOS** y **PROBAR**: lista los modelos disponibles y hace una pregunta corta.
4. **USAR COMO MODELO PRINCIPAL**: a partir de ahí responden los agentes con FreeLLMAPI
   (`llm.default_model = freellmapi:auto`). **VOLVER A LOS MODELOS LOCALES** lo deshace.

Para que solo un agente lo use, en el editor de agentes se pone como modelo `freellmapi:auto` (o
`freellmapi:<modelo>` con uno concreto de la lista). También vale en *Modelos > Modelo externo*.

Opciones:

- **Arrancarla al abrir Agente IA** (`freellmapi.arrancar`).
- **Accesible en la red local** (`freellmapi.red`): escucha en todas las interfaces para otros equipos y móviles.
- **URL** (`freellmapi.url`): por defecto `http://127.0.0.1:3001/v1`. Para usar otra FreeLLMAPI ya en marcha (su
  app de escritorio escucha en `http://127.0.0.1:31415/v1`, Docker, otro equipo) basta con cambiar la URL y poner su
  clave única.
- **Modelo** (`freellmapi.modelo`): `auto` o uno de la lista.

La clave única y la clave de cifrado de la base de datos (`freellmapi.clave_cifrado`, se genera sola) se guardan como
secretos: nunca se muestran ni salen por la API.

## Línea de órdenes

```bash
python -m nexus freellmapi estado            # instalada, en marcha, URL, versiones
python -m nexus freellmapi arrancar          # la deja en marcha (Ctrl+C para parar)
python -m nexus freellmapi probar            # arranca, comprueba /v1 con la clave única y para
```

## API

| Ruta | Qué hace |
|---|---|
| `GET /api/freellmapi` | Estado: instalada, en marcha, responde, URL y panel, clave puesta, modelo, principal, versiones |
| `POST /api/freellmapi/ajustes` (admin) | `{"url", "clave", "modelo", "arrancar", "red"}` |
| `POST /api/freellmapi/arrancar` / `parar` (admin) | Arranca o para la incluida |
| `GET /api/freellmapi/modelos` | Modelos que ofrece |
| `POST /api/freellmapi/probar` (admin) | Pregunta corta con el modelo elegido |
| `POST /api/freellmapi/usar` (admin) | `{"principal": true}` la pone como modelo principal; `false` vuelve a los locales |
| `GET /api/freellmapi/registro` (admin) | Últimas líneas de su registro (`logs/freellmapi.log`) |

## Cómo se construye y se prueba

`packaging/fetch_freellmapi.py --target <destino>` clona FreeLLMAPI, compila el servidor y el panel una vez, instala
para cada destino solo las dependencias de producción del servidor con `npm --os/--cpu`, baja el better-sqlite3
precompilado de ese sistema y esa versión de Node, y añade el Node.js oficial. Funciona desde Linux para los seis
destinos; `entrega.yml` lo hace antes de construir cada instalador.

La prueba de humo de cada instalador (`packaging/smoke_test.py`) instala el paquete en su propio sistema (Windows,
Linux, macOS, x64 y ARM) y ejecuta `python -m nexus freellmapi probar` con el Python del paquete: arranca el Node
incluido, espera la clave única y comprueba que el motor `freellmapi` de los agentes ve el endpoint.
