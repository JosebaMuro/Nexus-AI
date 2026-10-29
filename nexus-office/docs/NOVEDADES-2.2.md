# Nexus-Office 2.2: la versión de referencia

La 2.2 junta en una sola versión todo lo que se ha hecho desde la 1.8.1 (GIMP para los agentes) en Windows, Linux,
macOS y Android: lo de la 1.9 y la 2.0, lo que se hizo para la edición C++ 2.1 y lo nuevo de la 2.2. Esta página
compara la 2.2 con la 1.8.1, apartado por apartado.

**No se ha quitado nada de la 1.8.1.** Comparando el código de las dos versiones, no se ha borrado ningún fichero de la
aplicación, siguen todas las secciones de la interfaz (11 en la 1.8.1, 12 ahora con Dispositivos) y siguen los cinco
agentes de serie, más el Profesor. Las pruebas automáticas han pasado de 33 a 46 ficheros.

## Comparación con la 1.8.1

| Apartado | 1.8.1 | 2.2 |
|---|---|---|
| **Instaladores** | CPU, con GIMP dentro | CPU, GPU y NPU en el mismo paquete offline: llama-server con CUDA, Vulkan, Metal u OpenVINO y llama.cpp integrado, con GIMP y ahora también el firmware de los dispositivos y los ejemplos para importar |
| **Motor y acelerador** | Automático, sin elección | Ventana al arrancar para elegir motor y CPU/GPU/NPU, **PROBAR** con tokens por segundo y dónde ha quedado el modelo ([ACELERADOR.md](ACELERADOR.md)) |
| **Perfiles de hardware** | — | Seis de serie y los tuyos: motor, modelo, contexto, capas en la GPU y caché KV en VRAM o RAM |
| **Varias GPU y Linux** | Podía usar la integrada o llvmpipe (más lento que la CPU) | `--device` con la GPU elegida, sin dispositivos de software, vuelta a la CPU si la GPU falla |
| **Barra superior** | Indicadores fijos | Menús IA, PERFIL, MEMORIA y RED con lo que tu equipo tiene de verdad |
| **Modelos** | Qwen3 | Qwen3.5 con razonamiento y visión, e IA de apoyo semántica para las herramientas |
| **Chat** | Texto, voz, imágenes | Además **modo en vivo** manos libres e interrumpir hablando |
| **Profesor** | — | Agente con IA online opcional (Gemini, ChatGPT, Perplexity, Claude, Mistral…); los demás le preguntan y aprenden |
| **Bolsa** | Mesa simulada con precios ficticios | Simulación **con cotizaciones reales** (Yahoo, CNBC, Google, Stooq, Coinbase, Kraken), **agente de bolsa autónomo** con su cartera simulada, diario y gráficos en vivo de sus órdenes. Nunca dinero real ([BOLSA.md](BOLSA.md)) |
| **Grafos** | Grafo propio | Importar grafos de más de 50 formatos (Gephi, Neo4j, RDF, Excel…) como conocimiento, en todas las plataformas, y grafo de ejemplo de la Wikipedia en español ([GRAFOS.md](GRAFOS.md)) |
| **Biblioteca RAG** | Documentos y rastreo | Además dominios enteros con grafo multicapa y **lectura como un navegador** de webs hechas con JavaScript ([RAG.md](RAG.md)) |
| **Avatar** | Geisha, grafo, modelo propio | Encuadre automático de modelos de cualquier tamaño (también en el chat) y **biblioteca de avatares 3D gratuitos** (Open Source Avatars, VRM Consortium, Khronos, three.js, Sketchfab) ([AVATAR.md](AVATAR.md)) |
| **Salón** | Mesa con los agentes | Personajes más realistas, reuniones sin solapes, **despachos animados** con lo que hace cada agente y **sexo elegible** de cada personaje, tú incluido ([SALON.md](SALON.md)) |
| **Dispositivos** | — | Luces NexusLED para cualquier ESP32, reloj NexusWatch (ESP32-2424S012C), avatar NexusPico (Pico W, pantalla del pwnagotchi), modos y grabado sin internet ([DISPOSITIVOS.md](DISPOSITIVOS.md)) |
| **Android** | Dos apps: autónoma (CPU/GPU, NPU experimental) y VPN | **Una sola app**: la IA en el móvil (NPU, GPU o CPU) y *Conectar a mi ordenador* por red local, internet o VPN WireGuard. La app VPN ligera sigue para móviles modestos |
| **Edición C++** | — | Nexus-Office 2.1 en C++, un solo ejecutable con las mismas respuestas de la API ([CPP.md](../../nexus-office-cpp/docs/CPP.md)) |

## Lo nuevo de la 2.2
- **Perfiles de hardware** y **elección de GPU**, con el arreglo de la aceleración en Linux y de los equipos con dos GPU.
- **Barra superior con menús** que dependen del hardware.
- **RAG que lee páginas como un navegador**, con un navegador sin ventana del propio equipo, perfil propio y salida
  solo a direcciones públicas.
- **Despachos** animados por agente y **sexo** de cada personaje.
- **APK única** con la IA del móvil y la conexión al ordenador, con VPN WireGuard.
- **Avatares**: el chat encuadra los modelos grandes y hay biblioteca de avatares 3D gratuitos.
- **Bolsa**: gráficos reales de cripto aunque Yahoo limite (Coinbase y Kraken) y de acciones (CNBC).
- **Firmware y ejemplos dentro de los instaladores**.

## Qué no se ha podido probar sin hardware real
- Dos GPU físicas en el mismo equipo y la NPU de Intel (OpenVINO) y de Snapdragon (Hexagon): la lógica está probada con
  salidas reales de `--list-devices`, pero no con las tarjetas.
- El firmware en las placas reales (ver [DISPOSITIVOS.md](DISPOSITIVOS.md)).
- La APK en un móvil físico: compila en CI y la IA del móvil se probó en un Android emulado (2.0), pero no la NPU, la
  GPU del móvil ni la nueva conexión al ordenador (red local, internet y VPN) contra un PC real.
