# Cambios

## Nexus-Office 2.0.0

### Todo un dominio, sin excepciones, convertido en conocimiento para un agente
- **Antes**: *todo el dominio* recorría solo el host de la URL, hasta 100 000 páginas y 50 saltos, sin PDF ni documentos, y descartaba las páginas cortas.
- **Ahora** lee el dominio entero, tenga las páginas que tenga: sin límite de páginas ni de profundidad, con todos sus subdominios (cada uno con su `robots.txt`), los documentos enlazados (PDF, Word, LibreOffice, PowerPoint, Excel, EPUB, RTF, texto, CSV, Markdown) y las URL que aparecen dentro de esos documentos. Siempre en segundo plano y por lotes; si se cierra Nexus-Office, se reanuda solo al volver a arrancar.
- Al terminar construye un **grafo de conocimiento multicapa** propio del dominio: fuentes (secciones, páginas, documentos y enlaces), conceptos relacionados, afirmaciones **contrastadas entre fuentes** (contrastada, fuente única o discrepante, también cotejadas con el resto de la biblioteca), temas y, con el modelo de IA, una síntesis razonada de cada tema con sus conclusiones.
- El grafo se descarga como **paquete ZIP** importable y con **DAR A AGENTE** pasa al subgrafo y a la biblioteca de un agente, que adquiere ese conocimiento. Otro Nexus-Office lo importa con *IMPORTAR CONOCIMIENTO DE DOMINIO (.zip)*. Ver [docs/RAG.md](docs/RAG.md).

### Selector de motor y CPU/GPU/NPU al arrancar
- **Antes**: Nexus-Office decidía solo el motor y el acelerador; no se podía forzar la CPU, la GPU o la NPU sin tocar la configuración, ni saber si de verdad se estaba usando.
- **Ahora**, al abrir, la ventana **MOTOR Y ACELERADOR** deja elegir motor (*automático*, *llama-server* o *llama.cpp integrado*) y acelerador (*automático*, *CPU*, *GPU* o *NPU*), probarlo (tokens por segundo) y ver dónde ha quedado el modelo, leído del registro de llama-server. Lo que no está disponible en el equipo dice por qué y se usa lo siguiente que funcione.
- NPU: Intel Core Ultra con OpenVINO (Windows x64 y Linux x86_64) y Snapdragon con Hexagon en la app Android autónoma.
- **Cada opción lleva su runtime en los instaladores offline**: llama-server en sus variantes, el motor integrado (llama-cpp-python) y llama-server con OpenVINO. Ollama sale del selector porque es una aplicación aparte que no viene en el paquete; se sigue usando si está instalado.
- También desde la terminal: `python -m nexus run --motor … --acelerador …`. Ver [docs/ACELERADOR.md](docs/ACELERADOR.md).

### Modo en vivo por voz en el chat
- Conversación manos libres: escucha, responde en voz alta y vuelve a escuchar; si hablas mientras responde, se calla y te atiende. Responde sin razonar y en frases cortas para contestar enseguida (`live.max_tokens`, `live.reasoning`). Funciona con Whisper local, con la voz del sistema en Android o con el reconocimiento del navegador.

### Qwen3.5 con razonamiento y visión, y una IA de apoyo para todas las herramientas
- Los modelos recomendados pasan a **Qwen3.5** (0.8B, 2B, 4B y 9B), que razonan y ven imágenes en un solo modelo. Los Qwen3-VL Thinking siguen en el catálogo como anteriores.
- El modelo de embeddings multilingüe (Qwen3-Embedding) ayuda a todas las herramientas en cualquier idioma: elige qué herramientas pasan al modelo, enruta al agente adecuado, corrige nombres de herramienta y valores mal escritos, reduce resultados largos a lo relevante y reordena búsquedas.

### Avatar, salón y reuniones
- **Encuadre automático del avatar**: busto o cuerpo entero siempre dentro del hueco libre, con modelos propios en cualquier unidad y orientación. Ver [docs/AVATAR.md](docs/AVATAR.md).
- **Agentes del salón más realistas** (proporciones, ropa, pelo, brazos y mirada) sin coste apreciable: alrededor de 1,3 ms por fotograma.
- **Convocar reunión** ya no se puede pulsar encima de otra reunión (*REUNIÓN EN CURSO…*), la ronda de presentaciones se limita a los agentes más activos y las intervenciones van sin razonar para no hacer esperar. **Convocar reunión** y **Transcripción** funcionan en ordenador, móvil (también Android 7 a 9) y tableta en vertical y horizontal.

## Nexus-Office 1.9.0: importar grafos de cualquier formato como conocimiento

### Cualquier grafo se convierte en conocimiento de los agentes
- **Antes**: el gestor de conocimiento leía diez formatos (JSON, CSV, GraphML, GEXF, DOT, Turtle básico…), un fichero cada vez, y había que dar la extensión correcta. Las propiedades de los nodos se perdían.
- **Ahora** lee más de 50 formatos y los reconoce por el contenido: JSON node-link (D3, vis.js, NetworkX), Cytoscape (.cyjs, CX, SIF, XGMML), Neo4j (Browser, API HTTP, APOC, CSV de neo4j-admin y scripts Cypher), JSON-LD, GraphSON, Arrows.app, Obsidian Canvas, XMind, RDF completo (Turtle con nodos anónimos, N-Triples, N-Quads, TriG, N3, RDF/XML y OWL), GraphML de yEd, GEXF con atributos, GML, Pajek, GDF, UCINET DL, TGF, LGL, NCOL, listas de aristas y de adyacencia, matrices de adyacencia, hojas Excel y ODS, bases SQLite (también la de otro Nexus-Office, con sus ámbitos), Mermaid, PlantUML, texto con flechas, mapas mentales (FreeMind, Freeplane, OPML), esquemas con sangría, YAML, TOML y cualquiera de ellos comprimido (gz, bz2, xz) o varios dentro de un ZIP o TAR.
- Se pueden importar **varios ficheros a la vez** y se unen en un solo grafo: `nodes.csv` + `edges.csv` de Gephi o Neo4j, o una hoja de nodos y otra de aristas.
- Lo importado se **fusiona** con lo que ya se sabe y el aviso final dice cuántos nodos son nuevos y cuántos se han unido a los que ya había. Las relaciones repetidas se refuerzan.
- Los **atributos** de cada nodo (propiedades, literales, columnas) se guardan en su nota y los agentes los usan al responder sobre esa entidad.
- Tres formas de importar, todas sin internet: la app (*Gestor de conocimiento > Importar / exportar*), la orden `python -m nexus grafo importar …` y la herramienta `graph_import`, con la que un agente importa un grafo de su carpeta de trabajo o del texto que le das. Al leer un ZIP, los grafos que trae se importan en cualquiera de estos formatos.
- Ver [docs/GRAFOS.md](docs/GRAFOS.md).

## Nexus-Office 1.8.1: GIMP para los agentes

### Los agentes editan imágenes con GIMP
- **Antes**: los agentes podían generar imágenes, pero no retocar las del cliente: ni recortar una foto para Instagram, ni corregir el color de una foto de producto, ni poner el logo o un texto encima, ni preparar una foto de carnet o un original para la imprenta.
- **Ahora** tienen GIMP como herramienta. Los instaladores de escritorio (Windows x64 y ARM64, macOS, Linux x86_64 y ARM64, Raspberry Pi) traen el GIMP 3 oficial dentro, sin instalar nada y sin internet; si el equipo ya tiene GIMP 3 o 2.10 también vale. GIMP trabaja sin ventana, con un perfil propio que no toca el del usuario.
- Herramientas nuevas: `imagen_editar` (25 operaciones encadenables y 9 recetas: foto de carnet, formatos de redes, banner, producto con fondo blanco, mejora rápida, web ligera, A4 para imprimir), `imagen_info`, `gimp_estado` y `gimp_script` (scripts propios en Python-Fu o Script-Fu, con aprobación de un administrador).
- Trabajan sobre la carpeta de trabajo de los agentes. Las imágenes que adjuntas en el chat se guardan en `adjuntos/` para poder editarlas, los resultados van a `imagenes/`, se ven en el chat y nunca sobrescriben el original.
- Sin GIMP (en Android, por ejemplo) las mismas operaciones se hacen con Pillow; solo el formato XCF y los scripts necesitan GIMP.

### Diseñador gráfico y manual de GIMP
- Agente nuevo **Diseñador gráfico** en las oficinas de agencia de marketing, imprenta y estudio de fotografía. El fotógrafo, el estratega de marketing, el impresor, el comercial y el artista también usan GIMP y le delegan lo complejo.
- **Manual de GIMP** en nueve capítulos (interfaz y formatos, capas y máscaras, color, retoque, recorte y tamaño, texto y diseño con las medidas de redes e imprenta, automatización, atajos y recetas para pymes) en la biblioteca RAG de esos agentes, y un **grafo de conocimiento** de 119 relaciones sobre su manejo en el subgrafo de cada uno. Así pueden explicar a una persona cómo hacerlo ella misma en GIMP, con menús y atajos.
- Los paquetes de conocimiento (`nexus/office/conocimiento/`) se instalan solos al arrancar en los agentes que los usan y se actualizan al cambiar de versión. Ver [docs/GIMP.md](docs/GIMP.md).

## Nexus-Office 1.8.0: oficina de bolsa e inversiones

### Nueva oficina «Bolsa e inversiones» con mesa simulada
- **Antes**: ninguna oficina trabajaba con bolsa ni inversiones, y no había forma de practicar sin arriesgar dinero.
- **Ahora** hay un perfil más, `bolsa-inversiones` (59 en total), con cinco agentes propios: Analista de mercados, Gestor de carteras, Operador de mesa, Riesgos y cumplimiento, y Fiscalidad de inversiones, más los de oficina comunes. Cada uno con su grafo sembrado.
- Sección nueva **BOLSA** en la app y orden `python -m nexus bolsa`: una mesa simulada que imita un mercado real, sin dinero y sin internet. Cartera ficticia de 10.000 €, valores de BME, Xetra, Euronext, Londres, NYSE, Nasdaq y cripto; precios que solo se mueven con la bolsa abierta, en su hora local, con horario de verano y festivos; órdenes a mercado, limitadas, stop y stop limitadas; horquilla y deslizamiento; tres tarifas de comisión, canon, impuesto sobre transacciones financieras y coste del cambio de divisa; lotes FIFO, ganancia realizada, informe fiscal anual y aviso de la regla de los dos meses.
- Se pueden importar cierres reales de un valor (CSV) para anclar su precio y su volatilidad, y añadir valores propios.
- **Cartera real** solo para seguimiento: las órdenes quedan por confirmar, solo una persona las confirma escribiendo `CONFIRMO` (en la app con ADMIN o en la terminal) y Nexus-Office no se conecta a ningún bróker. Ningún agente puede confirmar.
- Siete herramientas nuevas para los agentes (`bolsa_mercados`, `bolsa_cotizacion`, `bolsa_cartera`, `bolsa_costes`, `bolsa_orden`, `bolsa_cancelar`, `bolsa_informe_fiscal`), todas sin internet. Ver [docs/BOLSA.md](docs/BOLSA.md).
- Aviso visible en la app y en cada respuesta de la mesa: los precios son ficticios y no es asesoramiento financiero.

### NPU de Snapdragon en la app Android autónoma
- **Antes**: la app detectaba la NPU del móvil pero no la usaba; el modelo iba en la GPU o en la CPU.
- **Ahora**, en los Snapdragon 8 Gen 2 o posteriores, el modelo va en la NPU Hexagon con el backend Hexagon de llama.cpp (compilado con el Hexagon SDK de Qualcomm). El orden es NPU, GPU y CPU: si la NPU no arranca, la app pasa sola a la GPU, y si la GPU tampoco, a la CPU, sin cortar la conversación. El autoajuste mide si la NPU va más rápida que la CPU.
- El backend Hexagon de llama.cpp es experimental y no se ha probado en un móvil real. En los demás móviles nada cambia.

### La imagen sin modelo ya sigue el prompt
- **Antes**: sin modelo de imagen descargado (y siempre en la app Android, que no lleva stable-diffusion.cpp) se dibujaba un campo de flujo abstracto. Solo cambiaban los colores si el prompt nombraba alguno, así que casi todo salía como el mismo «fractal». Con sd.cpp, el prompt en español llegaba sin traducir a un modelo que solo entiende inglés.
- **Ahora** un compositor de escenas (`nexus/media/scene.py`) lee el prompt en español o inglés y dibuja lo que pide: la hora (día, noche, atardecer, amanecer), el clima (lluvia, nieve, niebla, tormenta), el lugar (mar, playa, montaña, bosque, ciudad, desierto, lago, jardín, espacio) y los sujetos con su color y cuántos hay (gato, perro, casa, castillo, árbol, flor, coche, barco, persona, robot, astronauta, faro, caballo…). El arte abstracto queda solo para cuando se pide («abstracto», «fractal»).
- Antes de llamar a stable-diffusion.cpp, el prompt en español se traduce al inglés (con el LLM si lo hay; si no, con el diccionario del mejorador de prompts).
- La app explica debajo de la imagen por qué no es neuronal (no hay modelo, no hay motor o sd.cpp falló, con el error) y cómo conseguirlo.

## Nexus-Office 1.7.0: la oficina para pymes cambia de nombre

### Antes NEXUS AI, ahora Nexus-Office
- **Antes**: la oficina para pymes se llamaba igual que la IA particular, NEXUS AI, y compartía con ella los nombres de los instaladores, del paquete `.deb`, de las apps Android, del servicio y de las carpetas de datos. Instalar las dos en el mismo equipo o en el mismo móvil hacía que una pisara a la otra.
- **Ahora** se llama **Nexus-Office** en la app, los instaladores, la web de descarga y la documentación, y tiene identificadores propios, así que convive con NexusAI:

| | NexusAI (IA particular) | Nexus-Office 1.7.0 |
|---|---|---|
| Instaladores | `NexusAI-…` | `Nexus-Office-…` |
| Paquete Debian y orden | `nexus-ai` | `nexus-office` (en `/opt/nexus-office`) |
| Datos del usuario (Linux) | `~/.local/share/nexus-ai` | `~/.local/share/nexus-office` |
| Windows | `%LOCALAPPDATA%\NexusAI` | `%LOCALAPPDATA%\Nexus-Office` |
| Servicio | `nexus-ai` / `ai.nexus.server` / tarea «NEXUS AI» | `nexus-office` / `ai.nexus.office.server` / tarea «Nexus-Office» |
| Flatpak | `ai.nexus.NexusAI` | `ai.nexus.NexusOffice` |
| Apps Android | `ai.nexus.client`, `ai.nexus.autonomo` | `ai.nexus.office.client`, `ai.nexus.office.autonomo` |
| Puerto de la webui | 8765 | **8775** |

- El puerto cambia para que las dos puedan funcionar a la vez como servicio en el mismo equipo. La red cooperativa, el acceso remoto y la VPN mantienen sus puertos, que están apagados por defecto; si usas los dos programas con ellos activos, cambia el puerto en *Ajustes* de uno de ellos.
- Las apps Android de Nexus-Office no se instalan encima de las de NexusAI 1.6.0 (el identificador es otro). Si instalaste la oficina 1.6.0 en el móvil, desinstala esa app solo si era la de la oficina.
- Los datos de la oficina 1.6.0 no se mueven solos, para no mezclarlos con los de la IA particular. Para llevarlos a Nexus-Office, copia la carpeta de datos (`data/` de la instalación, o `~/.local/share/nexus-ai`) a la de Nexus-Office antes del primer arranque.
- Por dentro el paquete de Python sigue siendo `nexus` (`python -m nexus …`) y las variables de entorno siguen empezando por `NEXUS_`.

## Android: CPU, GPU y NPU (1.6.0, APK revisión 1)

### El modelo puede ir en la GPU del móvil
- **Antes**: la app Nexus-Office autónomo solo usaba la CPU. En el móvil no veía la GPU ni la NPU (Android no deja leer `/sys/class/drm`), así que *Sistema → Hardware* mostraba solo la CPU.
- **Ahora** la APK lleva también `libllama-server-gpu.so`: llama.cpp enlazado con la libc de Android, con **OpenCL** para Adreno (kernels de Qualcomm), **Vulkan** para Mali, Immortalis, Xclipse, PowerVR y demás, y la CPU en siete variantes de ARM (hasta ARMv9.2 con SVE2/SME). Cada backend se carga solo si el móvil tiene su driver.
- Nexus-Office detecta al arrancar (`nexus/hardware/android_accel.py`):
  - **CPU**: el SoC con su nombre comercial («Qualcomm SM8650 (Snapdragon 8 Gen 3)», «Google Tensor G3»…).
  - **GPU**: su nombre por EGL y, con `--list-devices`, si llama.cpp puede usarla de verdad. Adreno va por OpenCL; su Vulkan no se usa porque en llama.cpp es más lento que la CPU.
  - **NPU**: Hexagon (Qualcomm), APU (MediaTek), TPU (Google Tensor) o NPU de Exynos, por el SoC y las bibliotecas del fabricante. Se muestra, pero llama.cpp no puede usarla en Android sin los SDK propietarios, y el perfil lo explica.
- Con GPU, el perfil pone todas las capas en ella (`--device GPUOpenCL` o `Vulkan0`), y el **autoajuste mide** 3 respuestas con GPU y 3 con CPU y se queda con lo más rápido. Después mide los hilos como antes.
- Si el driver falla al arrancar llama-server, no responde o se cae a mitad de respuesta, Nexus-Office sigue en la CPU sin cortar la conversación y lo apunta en `android_accel.json` hasta la próxima versión de la app o de Android.
- La app escribe `device_info.json` con el SoC (`Build.SOC_*`), la GPU (EGL) y la versión de Vulkan.
- La APK pasa a versionCode 10601 (misma versión 1.6.0, misma firma): se instala encima conservando datos y modelos. Pesa unos 116 MB (antes 96 MB).
- `build_android_runtime.sh` compila el llama-server con GPU si encuentra el NDK; sin él la APK sale como antes, solo CPU.

### Otros arreglos
- El zip de código fuente no llevaba `tests/data/` (los RAR de prueba), porque la regla que deja fuera los datos del usuario (`data/`) también quitaba esa carpeta, y `test_archives` fallaba. Ahora solo se excluye el `data/` de la raíz, y los RAR de prueba (de rarfile, licencia ISC) vuelven a `tests/data/rar/`.
- RAR con el 7‑Zip de Debian/Ubuntu: ese paquete lista el formato RAR pero no trae sus códecs y fallaba con «Unsupported Method». Nexus-Office lo comprueba (`7z i`) y, si no puede, usa bsdtar o unar.

## Oficina portable (sobre 1.6.0)

### 58 perfiles de oficina y modelos sugeridos por sector
- **Antes**: 14 perfiles, todos de oficios (construcción, metal, instalaciones, panadería, taller y jardinería).
- **Ahora**: 58 perfiles. Se suman comercio y hostelería (restaurante, bar, hotel, catering, peluquería, estética, tienda, carnicería, frutería, floristería, óptica, farmacia, lavandería, fotografía), servicios profesionales, salud y cuidados (asesoría, abogados, inmobiliaria, arquitectura, clínica dental, fisioterapia, veterinaria, academia, escuela infantil, ayuda a domicilio, marketing, informática, autoescuela) y agro, transporte, industria y servicios técnicos (transporte, mudanzas, agricultura, ganadería, bodega y almazara, solar, telecomunicaciones, cristalería, tapicería, marmolería, piscinas, plagas, limpieza, imprenta, confección, aluminio y PVC, residuos). Hay 46 agentes de sector nuevos, cada uno con su grafo, y 3 agentes de oficina compartidos más: agenda y reservas, protección de datos y medio ambiente.
- Si hay un modelo específico para el sector, Nexus-Office lo **sugiere** una vez instalado: MedGemma 4B para salud, Qwen2.5‑Coder 7B para informática, el de razonamiento grande para despachos y el de imagen para diseño. Aparece en *Modelos → Recomendados* («◆ para tu oficina»), al aplicar el perfil y en `python -m nexus modelos`. Ver `docs/OFICINA.md`.

### Solo instaladores 100 % portables
- **Antes**: había paquetes *online* de unos 3 MB que descargaban Python y las dependencias al instalar, paquetes *offline* en `.zip`/`.tar.gz`, un zip maestro y un paquete para Termux.
- **Ahora** solo se entregan instaladores **completos**: llevan Python, las wheels y llama-server/sd-cli (CPU y Vulkan; CUDA en Windows x64 y Linux x86_64; Metal en macOS con Apple Silicon) y se instalan **sin internet**. Lo único que se descarga es el modelo de IA, si hay conexión.
- Ficheros de la 1.6.0, en `dist/{windows,linux,macos,android}`:
  - **Windows**: `Nexus-Office-Setup-1.6.0-windows-x64.exe` y `-windows-arm64.exe`. Instalador gráfico NSIS, generado en Linux con `makensis`, sin permisos de administrador (`%LOCALAPPDATA%\Nexus-Office`). Página de opciones: perfil de oficina, descargar el modelo, servicio al encender (solo ese paso pide UAC), acceso desde la red local y acceso directo en el escritorio. Silencioso: `/S /OFICINA=<perfil> /SERVICIO /RED /MODELOS=auto|todos|no /D=<carpeta>`. No está firmado: SmartScreen pide *Más información → Ejecutar de todas formas*.
  - **Linux**: `Nexus-Office-1.6.0-linux-x86_64.run` y `-linux-aarch64.run` (`sh … [carpeta] [--oficina=] [--servicio [--red]] [--modelos=]`), y `nexus-office_1.6.0_amd64.deb` y `_arm64.deb`: programa en `/opt/nexus-office`, lanzador `nexus-office`, datos de cada usuario en `~/.local/share/nexus-office`, servicio con `sudo nexus-office servicio instalar`.
  - **macOS**: `Nexus-Office-1.6.0-macos-arm64.tar.gz` y `-macos-x86_64.tar.gz`, con `Nexus-Office.command`.
  - **Android**: `Nexus-Office-1.6.0-android-autonomo.apk` (todo en el móvil) y `Nexus-Office-1.6.0-android-vpn.apk` (cliente ligero del PC de la oficina).
  - **Código fuente** limpio: `Nexus-Office-1.6.0-codigo-fuente.zip`.
- **ARM**: Windows en ARM (Snapdragon X), Linux ARM64 y Raspberry Pi 4/5 con sistema de 64 bits (Raspberry Pi OS 64‑bit). Los sistemas de 32 bits no están soportados.
- Ya no se entrega el paquete de Termux; `install-termux.sh` sigue en el código fuente y la app Nexus-Office VPN puede bajarlo del PC.
- `packaging/build.py [--targets …] [--apk …] [--docs …] [--keep]` construye todo, con los runtimes preparados antes por `build_python_runtimes.py`, `build_desktop_runtimes.sh` y `build_gpu_runtimes.py`. Ver `docs/PLATAFORMAS.md`.

### El instalador elige el modelo de IA según el equipo
- Nuevo `python -m nexus modelos [--descargar] [--todos]`: muestra el equipo (RAM, VRAM, acelerador, gama) y el modelo elegido de cada tipo: chat y visión (Qwen3‑VL Thinking 2B, 4B u 8B), documentos (Qwen3‑Embedding) y voz (Piper); el de imagen, con `--todos`. Con `--descargar` los baja si hay internet.
- Lo llaman todos los instaladores (`--modelos=auto|todos|no`, `-Modelos`, `/MODELOS=`). Sin internet solo recomienda, y se descargan después desde *Modelos → Recomendados*.

### Nexus-Office como servicio en todas las plataformas
- Nuevo `python -m nexus servicio instalar|quitar|estado [--red] [--puerto P]` (`nexus/service.py`): arranca Nexus-Office solo al encender el equipo, sin ventana, y lo reinicia si falla.
  - **Linux**: systemd del sistema (con `sudo`) o de usuario con `loginctl enable-linger`; sin systemd, cron `@reboot`.
  - **macOS**: LaunchDaemon (con `sudo`) o LaunchAgent.
  - **Windows**: tarea programada al arrancar el equipo como SYSTEM si eres administrador, o al iniciar sesión si no; reinicio ante fallos, sin límite de tiempo y regla de firewall con `--red`.
  - **Termux**: termux-services y Termux:Boot.
- En los instaladores: `--servicio --red` / `-Servicio -Red` / `/SERVICIO /RED`. En la web: *Sistema → Arranque automático* (cambiarlo exige modo administrador).
- Si el servicio ya está en marcha, el acceso directo o `start.sh` solo abren el navegador.
- App **Nexus-Office autónomo**: *Sistema → Arranque automático* → **ARRANCAR AL ENCENDER** inicia su servicio en primer plano al encender el móvil.
- Guía en `docs/SERVICIO.md`. API: `GET/POST /api/service`.

### Descargas para la oficina
- *Sistema → Descargas* lista los instaladores copiados en `<datos>/descargas/` (por ejemplo, `data/descargas/windows/…`), marca el del sistema **anfitrión** y el del **dispositivo** que mira la página, y deja descargarlos desde los móviles y PC de la red, también desde la app Nexus-Office VPN.
- API: `GET /api/downloads` y `GET /descargas/<ruta>`.

### Documentación
- `docs/USUARIO.md` explica la instalación en cada sistema, el modelo elegido según el equipo, el arranque automático y las descargas desde el móvil. Nuevo `docs/SERVICIO.md`.

## Oficina para pymes (sobre 1.6.0)

### Perfiles de oficina por tipo de empresa
- **Antes**: Nexus-Office traía cinco agentes generalistas (Nexus-Office, Artista, Investigador, Forjador, Programador).
- **Ahora** la instalación puede ser **la oficina de una pyme**: eliges un perfil (14 incluidos: herrería, calderería, estructuras metálicas, reformas, construcción, carpintería, electricidad, fontanería y climatización, taller mecánico, cerrajería, pintura, panadería, jardinería y mantenimiento integral) y Nexus-Office monta su equipo con **agentes de oficina** (recepción, administración, presupuestos, compras, prevención, personal, calidad, comercial) y **agentes del gremio** (herrero, calderero, soldador, albañil, fontanero, electricista, carpintero, pintor, cerrajero, climatización, mecánico, panadero, jardinero).
- Cada agente, también los del gremio, tiene **su propio grafo**, que nace sembrado con el saber hacer básico de su oficio y crece con su trabajo.
- Se elige en *Agentes → Perfil de oficina*, con `python -m nexus oficina <perfil>`, al instalar (`--oficina=` / `-Oficina` / `NEXUS_OFICINA`, también en Termux), en la app Android autónoma desde *Agentes* o por API (`/api/office/…`). No borra nada y funciona sin internet. Guía en `docs/OFICINA.md`.

## 1.6.0 — 2026-10-13

### Memoria común y memoria propia de cada agente
- **Antes**: lo que sabía del usuario iba al grafo general y todo lo demás que aprendía un agente (temas, definiciones) iba a su subgrafo, fuera o no de su especialidad. Si hablabas de Django con Nexus-Office, lo aprendía Nexus-Office y no el programador.
- **Ahora** el **grafo general** guarda lo común: tu perfil, tus gustos, las **pautas de comportamiento** que pides, tus **relaciones** con otras personas, las **interacciones** con los agentes y el conocimiento general para razonar. El **subgrafo de cada agente** guarda solo el saber hacer de su especialidad: conceptos, procedimientos, documentos y experiencia.
- Nuevo **organizador** (`nexus/memory/organizer.py`): decide dónde va cada cosa comparándola con la especialidad de cada agente. Usa el modelo de embeddings si lo hay, y si no, palabras clave. Funciona en la conversación, con `memory_save`, al importar grafos y al leer un ZIP.
- Cada agente tiene un campo **Especialidad** (*Agentes → Editar*). Los agentes de serie ya lo traen.
- Las **pautas** («no uses emojis», «respóndeme siempre con ejemplos», «llámame Joseba») se guardan una vez y van en el prompt de todos los agentes.
- Cada nodo tiene **categoría**, **nota** y **origen**. Las bases de datos de la 1.5 se migran solas.

### Gestor de conocimiento en Ajustes (modo administrador)
- **Resumen** de cada grafo por categoría, **explorar y editar** nodos (también buscando por significado), moverlos entre grafos, fusionarlos y editar sus relaciones.
- **Importar grafos** de conocimiento: JSON (Nexus-Office, D3/vis.js, Cytoscape, JSON Graph Format, tripletas), JSONL, CSV/TSV, GraphML, GEXF, DOT, N-Triples/Turtle básico y notas Markdown con `[[enlaces]]` (Obsidian, Logseq). Al general, a un agente o en automático. **Exportar** en JSON, GraphML o CSV.
- **Leer un ZIP** (también RAR, 7z y TAR): descomprime de forma segura, inserta los grafos que traiga, agrupa los documentos por temas, **razona** con el LLM los más representativos (resumen, hechos y procedimientos), lo añade a la biblioteca RAG y escribe una síntesis. Lo común va al general y el saber hacer, al especialista.
- **Mantenimiento**: reorganizar lo aprendido en versiones anteriores (primero muestra la propuesta), buscar duplicados y mover recuerdos entre ámbitos.
- Los trabajos van en segundo plano, con progreso en vivo. API nueva en `/api/kg/…` (ver `docs/API.md`). Guía completa en `docs/MEMORIA.md`.

### Embeddings especializados en documentación: Qwen3-Embedding 0.6B
- Nuevo modelo recomendado `qwen3-embedding-0.6b` (GGUF Q8_0, 639 MB): multilingüe, entrenado para buscar, clasificar y organizar documentación, con **instrucciones por tarea**. Se descarga desde *Modelos* o *Ajustes → Gestor de conocimiento → Embeddings* y queda disponible sin conexión. Si es el único, se activa solo.
- Corre **en CPU** con el mismo `llama-server` de los paquetes offline (o `llama-cpp-python`), en un proceso aparte que se para tras 10 minutos sin uso.
- **Indexado progresivo**: lo nuevo se indexa al momento con vectores rápidos y se recalcula con el modelo en segundo plano, cediendo el turno al chat. En un portátil sin GPU, importar una biblioteca no bloquea las respuestas. Cambiar de modelo reindexa igual.
- `memory.embed_dims` recorta los vectores (Matryoshka) para ahorrar RAM.

### Correcciones
- Los nombres propios ya no se juntan a través de saltos de línea (un título de Markdown y la primera palabra del párrafo formaban un nodo «Despliegue Primero»).

### Paquetes offline con GPU y NPU de cualquier fabricante
- **Antes**: los paquetes offline solo traían llama-server y sd-cli para CPU. La GPU necesitaba descargar la variante desde *Sistema → Runtimes*, y los extras de NPU de Qualcomm o PyTorch, internet.
- **Ahora** cada paquete trae su aceleración, y solo faltan los modelos:
  - **Vulkan** (Windows y Linux, x64 y ARM64) para llama-server y sd-cli: GPU NVIDIA, AMD, Intel, Qualcomm Adreno, Mali... con su driver normal. Compilado con zig desde PyPI.
  - **CUDA 12.9** con cuBLAS para NVIDIA en Windows x64, Linux x64 y Jetson Orin (conda-forge).
  - **Metal** en Mac con Apple Silicon (conda-forge).
  - Wheels de **OpenVINO** (Intel CPU/GPU/NPU), **onnxruntime-qnn** (NPU Snapdragon), **PyTorch con DirectML** (cualquier GPU en Windows) y **PyTorch con Metal** (Mac).
- Nexus-Office prueba la variante de GPU que toca a tu hardware. Si al equipo le falta el driver, pasa a la siguiente y, al final, a la CPU. *Sistema → Runtimes* muestra todas las variantes incluidas y cuál está en uso.
- PyTorch se instala sin preguntar si sus wheels vienen en el paquete y tu GPU lo aprovecha.
- No se incluyen ROCm, SYCL ni PyTorch con CUDA/ROCm (varios GB cada uno): esas GPU van por Vulkan, y con internet siguen pudiendo instalarse.
- Nuevos `packaging/build_gpu_runtimes.py` (CUDA y Metal) y variante `vulkan` en `packaging/build_desktop_runtimes.sh`.

### Limpieza al final de la instalación
- `install.sh` e `install.bat` preguntan al terminar si quieres borrar lo que tu equipo no usa (wheels no instaladas y runtimes de otras GPU) y si guardas lo usado para reinstalar sin internet.
- `--clean` / `-Clean` limpia sin preguntar y `--no-clean` / `-NoClean` no limpia. Sin terminal no se limpia nada.
- `python -m nexus cleanup [--dry-run] [--no-keep]` hace lo mismo en cualquier momento. El runtime en uso y su respaldo CPU nunca se borran.

### Paquetes
- Las 6 plataformas offline, el zip python y las dos APK (Nexus-Office VPN y Nexus-Office autónomo) pasan a la 1.6.0. La APK autónoma lleva el código nuevo, incluido el gestor de conocimiento.

## 1.5.0 — 2026-10-13

### RAG web: dominios enteros por lotes correlativos
- **Antes**: `crawl_docs` y el botón *Rastrear* paraban a las 500 páginas y lo metían todo en un solo documento, así que un dominio grande quedaba a medias o agotaba la memoria.
- **Ahora** un dominio de hasta 100 000 páginas se procesa por **lotes ordenados**. Cada lote es un documento propio de la biblioteca: «Documentación: X · lote 003 (págs. 401-600)». El tamaño del lote se ajusta a la RAM: 50 páginas con menos de 4 GB, 100 en móviles o con menos de 8 GB, 200 con menos de 16 GB y 300 a partir de ahí. Se puede fijar con `rag.crawl_batch_pages`.
- **Reanudable**: el trabajo se guarda en `data/rag/crawls/` después de cada lote, con la cola de URLs en el orden de descubrimiento, las páginas visitadas y las huellas del contenido. Si se pausa, se reinicia Nexus-Office o se corta, al reanudar sigue por la página siguiente, sin repetir ninguna y en el mismo orden que si se hubiera hecho de una vez.
- **Panel de rastreos** en *Biblioteca RAG*: muestra el progreso de cada trabajo (páginas, lotes y cola pendiente) y tiene botones **PAUSAR**, **REANUDAR** y **✕** (borrar el trabajo y, si quieres, sus documentos). El botón *Rastrear* trabaja en segundo plano.
- **Chat y agentes**: «aprende todo el dominio X» o «toda la documentación de X» lanza el rastreo por lotes en segundo plano. «sigue rastreando X» reanuda el último trabajo sin terminar. La herramienta `crawl_docs` admite `job`, `continue` y `background`.
- Volver a rastrear la misma URL sustituye todos los lotes anteriores en cuanto termina el primer lote nuevo.
- API: `GET /api/rag/crawls`, `POST /api/rag/crawls/{id}/resume`, `POST /api/rag/crawls/{id}/pause` y `DELETE /api/rag/crawls/{id}?docs=true`. `POST /api/rag/crawl` admite `background` y `batch_pages`.

### Otras herramientas que ya no se quedan cortas
- **Proyectos grandes en el RAG** (zip, tar, 7z, rar): hasta 20 000 ficheros y 1 GB. Se indexan por grupos de 200 ficheros, en partes correlativas («· parte 2 (ficheros 201-400)»). Borrar la primera parte borra todas.
- **Embeddings por bloques** de 2048 fragmentos, para que un documento enorme no dispare la memoria.
- `web_fetch`: descarga en streaming con un tope de 5 MB, vuelve a comprobar la URL tras cada redirección y pagina el texto con `offset` y `next_offset`.
- `file_read` lee por tramos (`offset`, `max_chars`, `next_offset`) y `file_list` pagina de 500 en 500.
- `rag.allow_private_urls` (desactivado por defecto) permite rastrear la intranet o `localhost`.

### Modelos LLM con visión y razonamiento
- El catálogo recomendado pasa a **Qwen3-VL Thinking** (2B, 4B y 8B, en GGUF Q4_K_M con su proyector de visión). Los tres ven imágenes y razonan antes de contestar. Se retira SmolVLM, que no razonaba. El modelo «mejor» para cada equipo es siempre uno de ellos, también con poca RAM (2B desde 3 GB).
- Si Hugging Face cambia el nombre de los ficheros, Nexus-Office lista el repositorio y elige el Q4_K_M y el proyector equivalentes en vez de fallar.
- **Razonamiento separado de la respuesta**: llega aparte (`reasoning_content` de llama-server y OpenAI, `thinking` de Ollama, o `<think>…</think>` en el texto) y la web lo muestra en un bloque plegable «Razonamiento» encima de la respuesta.
- **Presupuesto de razonamiento**: `llm.reasoning_budget` (2048 tokens en el chat) y `llm.reasoning_budget_tasks` (768 en tareas internas como resúmenes). Se suma a `max_tokens`, así que el razonamiento no deja la respuesta sin espacio. Con llama-server se usa el límite nativo (`reasoning_budget_tokens`). `llm.reasoning: off` lo desactiva.
- App Android autónoma: propone Qwen3-VL 2B o 4B Thinking según la RAM, y sugiere el cambio si tenías un modelo anterior.

### Paquetes offline completos: solo faltan los modelos
- Los paquetes `-offline` de Windows (x64 y ARM64), Linux (x86_64 y ARM64) y macOS (Apple Silicon e Intel) llevan el **Python portable**, **todas las wheels** (base y extras de voz y NPU) y **llama-server y sd-cli precompilados** (solo CPU) para chatear y generar imágenes. Se instalan y funcionan sin internet: solo hay que descargar los modelos.
- El Python portable se arma ahora desde conda-forge (`packaging/build_python_runtimes.py`) si GitHub no está disponible. llama-server y sd-cli se compilan para las 8 combinaciones con `packaging/build_desktop_runtimes.sh` (zig, desde las fuentes que publican llama-cpp-python y stable-diffusion-cpp-python en PyPI, sin GitHub). En x86_64 hay dos variantes: `cpu` (AVX2) y `cpu-compat` (cualquier CPU de 64 bits); Nexus-Office elige sola.
- *Sistema → Runtimes* marca lo incluido con «✓ incluido», y `nexus setup` ya no intenta descargarlo. En modo offline, lo que no venga incluido se omite con un aviso en vez de dar error. Para usar la GPU (CUDA, Vulkan, Metal) se sigue pudiendo instalar la variante oficial desde ahí.
- **Corregido: faltaban wheels en Windows y macOS.** `pip download` decide las dependencias según el equipo donde se ejecuta, no según la plataforma destino. Por eso el wheelhouse de Windows no traía `colorama` (lo pide click), `comtypes` ni `pywin32` (pyttsx3), y el de macOS no traía `pyobjc`: la instalación offline habría fallado. Ahora `nexus wheels` recorre las dependencias con los marcadores de la plataforma destino, baja las que falten y `--check` lo comprueba. Probado instalando el paquete de Windows sin red con wine.
- **Windows ARM64**: el paquete offline usa el Python x64, que Windows 11 ARM emula, y sus wheels. llama-server y sd-cli son nativos ARM64.

### Instalación sin compilar y sin internet (Linux, WSL, Windows y macOS)
- **Windows**: `install.ps1` / `install.bat` también instalan solo wheels binarias (no hace falta Visual Studio). Tienen `-FetchWheels` (prepara `wheelhouse\` y `runtime\`) y `-Offline`, con el mismo comportamiento que en Linux.
- **Windows ARM64**: PyYAML no tiene wheel para esta plataforma, y pip habría tenido que compilarlo. `vendor/wheels/` trae la misma versión en puro Python, y solo se usa donde falta la binaria.
- **macOS**: usa `install.sh` con las mismas opciones. En Mac Intel no existen wheels de piper-tts, faster-whisper ni onnxruntime: `nexus setup` lo dice y sigue, sin compilar.
- **Termux**: pydantic-core y pyyaml también salen de `pkg`. Antes, pydantic-core se compilaba con Rust. Si a tu repositorio le falta alguno, se instala `rust` para compilarlo. Ahora instala `requirements.txt` completo; antes faltaban fpdf2 y pypdf, necesarios para los PDF.
- **Docker**: `pip install --only-binary=:all:`.
- Comprobado con `nexus wheels --check` (instalación simulada sin red) en Linux x86_64 y ARM64, Windows x64 y ARM64, y macOS ARM e Intel. El flujo de `install.ps1` se probó con PowerShell 7. El script de Termux se ha revisado, pero no se ha probado en un móvil.
- **Nunca compila** (Linux/WSL): `install.sh` y `nexus setup` instalan solo wheels binarias (`--only-binary=:all:`). Si falta una, lo dicen claramente en vez de intentar compilar con gcc. `NEXUS_ALLOW_BUILD=1` lo permite.
- **Wheels offline**: `bash install.sh --fetch-wheels` baja a `wheelhouse/` las dependencias y los extras de voz y NPU (piper-tts, faster-whisper, pyttsx3, onnxruntime, openvino), y el Python portable a `runtime/`. Luego `bash install.sh --offline` instala sin red. Se cubren glibc 2.39 y 2.28 (Ubuntu 20.04 en adelante). Nuevo comando `python -m nexus wheels` (`--target`, `--runtime`, `--check`) y `wheelhouse/SHA256SUMS`.
- Los extras de `nexus setup` (también desde *Sistema*) usan primero `wheelhouse/` sin red. Antes iban siempre a internet, incluso en los paquetes offline.
- Los paquetes offline de `packaging/build.py` incluyen ahora los extras de voz y NPU, y se comprueba que instalan sin internet antes de empaquetar.
- **WSL**: `install.sh` ya no borra el runtime de Windows si la carpeta es la misma (`venv\python.exe`), y avisa si Nexus-Office está en `/mnt/c` (más lento). Si falta `curl`, dice cómo instalarlo.
- `install.sh` y `start.sh` vuelven a ser ejecutables dentro del zip.

## 1.4.3 — 2026-10-12

### App Android autónoma: corregido el «Error 159» al arrancar
- **Qué pasaba**: Android mata con la señal SIGSYS a cualquier programa de una app que use una llamada al sistema fuera de su lista permitida. El proceso termina con el código 159 (128 + 31). Al importar numpy, OpenBLAS usa `mbind` para colocar su memoria, y Android no la permite. Por eso Nexus-Office moría al arrancar, por ejemplo en un OnePlus 13 con Android 16. Se reproduce en un PC con un filtro seccomp igual, y da el mismo código 159.
- **Solución**: un manejador de SIGSYS (`android-autonomo/native/nexus_sigsys.c`) hace que esas llamadas devuelvan `ENOSYS`, como si el kernel no las tuviera, y los programas siguen sin ellas. Va en la APK como `libnexussys.so`: Python lo carga con el cargador de musl (`--preload`), también los Python hijos, y `llama-server` lo lleva enlazado. Cada llamada bloqueada se anota una vez en `nexus.log`.
- Si Nexus-Office se cierra por una señal, el mensaje dice cuál (por ejemplo «código 159, señal 31»).
- Todos los binarios están alineados a 64 KB, así que también valen para móviles con páginas de memoria de 16 KB.

## 1.4.2 — 2026-10-12

### App Android autónoma: primer arranque sin cuelgues
- **Espacio**: antes de preparar nada comprueba que hay sitio (unos 350 MB). Si falta, lo dice con los MB que necesita y los que hay.
- **Descompresión en paralelo y verificada**: copia el entorno de ejecución de la APK y lo descomprime con 2 a 4 hilos en una carpeta temporal. Comprueba ficheros y bytes contra `assets/runtime.json` y solo entonces lo da por bueno. Si la app se cierra a medias, la siguiente vez empieza de cero en lugar de usar un entorno incompleto. En un PC tarda unos 2 s; en el móvil, segundos en vez de minutos.
- **Comprobación en cada arranque**: si falta algún fichero imprescindible (Python, la biblioteca estándar o Nexus-Office), lo vuelve a preparar solo.
- **Arranque vigilado**: si Nexus-Office no responde en 4 minutos, lo detiene y explica qué pasó con las últimas líneas del registro. Si se cierra al arrancar, lo reintenta una vez, y si el registro indica ficheros dañados, repara antes el entorno.
- **Pantalla de arranque**: muestra la fase («Copiando…», «Descomprimiendo… (4 hilos)», «Comprobando…») y la última línea del registro de Nexus-Office. Si algo falla, ofrece **Reintentar**, **Reparar** (vuelve a descomprimir el entorno sin tocar datos ni modelos) y **Copiar registro**. *Reintentar* ahora para de verdad un arranque colgado, también a mitad de la descompresión.
- El registro anterior se guarda como `nexus.prev.log`.

### Optimización después de cada inicio
- Cuando Nexus-Office ya responde, en segundo plano:
  - borra temporales de más de un día y recorta los registros grandes;
  - optimiza la base de datos (`PRAGMA optimize` y compacta el WAL);
  - aplica los hilos medidos y los límites por temperatura o batería;
  - precarga el modelo con una petición de un token, para que la primera respuesta no espere a leerlo del disco. Solo lo hace si el móvil no está caliente, tiene batería (o está cargando) y hay RAM libre.
- Lo hecho aparece en *Sistema → Autoajuste adaptativo*.

## 1.4.1 — 2026-10-12

### App Android autónoma que se optimiza sola
- **Núcleos rápidos**: detecta cuántos núcleos grandes tiene el móvil, por el modelo de CPU (`/proc/cpuinfo`) o por la frecuencia máxima de cada núcleo, y empieza con esos hilos. Los núcleos pequeños frenan a llama.cpp.
- **Hilos medidos con tus respuestas**: prueba 2 o 3 números de hilos (por ejemplo 3, 4 y 6) con la velocidad real de generación de cada respuesta y se queda con el más rápido. Lo recuerda entre arranques y vuelve a medir al cambiar de modelo, de variante de llama.cpp o de versión de la app, y de vez en cuando por si el móvil ha cambiado. Los cambios se aplican entre respuestas, nunca a mitad.
- **Temperatura y batería**: la app comunica cada 30 s el estado térmico de Android, la batería y el ahorro de energía. Con el móvil caliente, la batería por debajo del 15 % sin cargar o el ahorro activado, usa la mitad de hilos (redondeando hacia arriba); con el móvil templado, uno menos. Al normalizarse los recupera. Mientras está limitado no cuenta como medida.
- **Memoria**: si Android se queda con menos de 0,6 GB libres, reduce el contexto para el siguiente arranque de llama-server. En el móvil el contexto empieza en 4096 como máximo y no pasa de 8192.
- **Modelo**: si el modelo va a menos de 4 tokens/s, la app propone uno más pequeño; si va a más de 14 y hay RAM, uno mayor. Al aceptar, lo descarga y lo activa al terminar. Si se rechaza, no vuelve a preguntar en 7 días.
- La variante de llama.cpp (ARMv8, dotprod o i8mm) se sigue eligiendo por la CPU en cada arranque.
- API: `GET /api/android/autoajuste` (núcleos, hilos, velocidades medidas, límites y consejo), `POST /api/android/autoajuste/medir` y `POST /api/android/autoajuste/modelo/{id}`. Lo que decide aparece también en *Sistema → Autoajuste*.
- **Actualizar**: instalar la APK nueva encima conserva los datos y los modelos. La app prepara su runtime nuevo aunque la versión coincida, porque lo distingue por fecha de instalación.

## 1.4.0 — 2026-10-12

### Un grafo aislado para cada agente, dentro del grafo general
- El grafo de conocimiento tiene ahora **ámbitos**: el **general**, común a todos los agentes, y un **subgrafo aislado por agente** donde cada uno acumula su propio conocimiento.
- Qué va a cada sitio:
  - Lo que se sabe **del usuario** (nombre, gustos, dónde vive...) va al general: todos los agentes lo conocen.
  - Los **temas, definiciones y respuestas** de una conversación van al subgrafo del agente que conversa.
- Cada agente consulta el general y el suyo, **nunca el de otro agente**. Lo mismo vale para sus recuerdos privados.
- Al borrar un agente se borra su subgrafo y sus documentos pasan al general, así que no se pierde ninguno.
- **Núcleo**: un selector sobre el grafo 3D muestra *Todo* (cada agente es un racimo con su color, enlazado al núcleo), *General* o el grafo individual de un agente.
- Las bases de datos anteriores se migran solas al arrancar: lo existente queda en el general y se conservan los ids.

### RAG al grafo general o al de un agente
- La Biblioteca tiene un selector **Importar en**: *grafo general* (lo consultan todos) o *solo un agente* (solo lo consulta él y entra en su grafo individual). Vale para documentos sueltos, comprimidos, URL → PDF y rastreo web.
- Cada documento muestra en qué grafo está y se puede **mover** a otro; su nodo de conocimiento se mueve con él.
- Todo documento importado se enlaza en su grafo (`ha_leido`, `trata_de`), no solo los de URL y rastreo.
- API: campo `agent` en `/api/rag/upload`, `/api/rag/import`, `/api/rag/url` y `/api/rag/crawl`. Nuevos `PATCH /api/rag/{id}`, `GET /api/graph?scope=` y `GET /api/graph/scopes`. Herramientas `url_to_pdf` y `crawl_docs` con `target: general|agente`.

### Optimización del grafo y de la búsqueda
- Aprender de un mensaje es unas 2 veces más rápido: cada entidad se guarda con una sola sentencia (`INSERT ... ON CONFLICT ... RETURNING`) y todo el mensaje se confirma en una sola transacción, en lugar de una por escritura. En discos lentos la ganancia es mayor.
- Índices nuevos en relaciones (origen, destino, ámbito) y entidades (ámbito, peso). `facts_about` y la vista del grafo filtran en SQL.
- La búsqueda RAG ya no carga el texto de todos los fragmentos en memoria y selecciona los mejores con `argpartition`.
- Poda periódica de co-ocurrencias débiles y antiguas para que el grafo no crezca sin límite.

### Salón de reuniones con estética de Los Sims
- La mesa de reuniones del Salón es ahora una sala isométrica: suelo de tablas, paredes con zócalo, puerta, ventana, plantas, alfombra y una mesa ovalada de madera con jarrón de flores.
- Cada agente es un personaje con su rombo verde encima, ropa del color del agente, una tarjeta con su nombre, taza de café y portátil o libreta. El aspecto (piel, pelo, peinado) sale de su id, así que siempre es el mismo.
- Movimiento sin rigidez: todo se anima por tiempo real con transiciones amortiguadas. Los agentes entran andando por la puerta y se sientan, respiran, parpadean, miran a quien habla, asienten, gesticulan, teclean y beben café.
- Bocadillos blancos que se escriben poco a poco y se apartan para no taparse (como mucho 3 a la vez; 2 en el móvil). Las delegaciones vuelan como avión de papel, los aprendizajes encienden una bombilla y las propuestas ponen el rombo en ámbar.
- Tú te sientas de espaldas en primer plano. En pantallas estrechas se recortan las paredes, no la mesa.

### Importación de documentos y rastreo web: procesador al máximo
- **Importación en lote**: `POST /api/rag/import` con muchos ficheros a la vez. Cada documento suelto se lee en un núcleo distinto (`nexus/parallel.py`: pool de procesos, o de hilos en Android y con `NEXUS_NO_PROCESSES=1`). Si un fichero falla, los demás se importan igualmente. La Biblioteca envía los ficheros soltados en tandas de 20 y muestra el resumen.
- **Archivos comprimidos**: además de ZIP, ahora **RAR, 7z y TAR** (`.tar.gz`, `.tgz`, `.tar.bz2`, `.tar.xz`).
  - RAR y 7z usan la herramienta del sistema: `unrar`, `7z`/`7zz`, `bsdtar` o `unar`.
  - Se descomprimen en cuarentena con los mismos límites anti‑bomba y anti‑*zip slip*. Los ficheros cifrados o dañados se descartan y se informa de ellos.
  - Si falta la herramienta, Nexus-Office dice cuál instalar en cada sistema.
- **Nuevos formatos de documento**: OpenDocument (ODT, ODS, ODP), PowerPoint (PPTX), Excel (XLSX), EPUB y RTF, además de texto y código (`nexus/rag/readers.py`). `GET /api/rag/formats` los lista.
- **Rastreo web en paralelo**:
  - Descarga varias páginas a la vez por conexiones reutilizadas (`performance.crawl_connections`, de 4 a 8 por defecto).
  - Analiza las páginas en varios núcleos.
  - Mantiene el mismo orden y resultado que el rastreo secuencial.
  - Con `Crawl-delay` en robots.txt va de una en una, como antes.
- **Embeddings más rápidos**: el motor *hashing* reparte los lotes grandes entre núcleos. Ollama recibe lotes de 128. Los fragmentos se guardan con una sola transacción.
- Nueva sección de configuración `performance`: `workers` y `crawl_connections`, con 0 = automático.

### Descargar además de copiar
- Cada bloque de código del chat tiene los botones **COPIAR** y **DESCARGAR**. El fichero se guarda con la extensión del lenguaje.
- Cada respuesta se puede descargar en `.md`.
- Las propuestas de herramientas se pueden descargar en `.py`.
- En las apps Android, el sistema pregunta dónde guardar.

### Dos apps Android (versión 1.4.0)
- **Nexus-Office VPN** (`Nexus-Office-1.4.0-android-vpn.apk`, `ai.nexus.office.client`, Android 7+): sustituye a la app cliente 1.3.
  - Conecta con el Nexus-Office del PC (Windows, Linux o macOS) por un **túnel WireGuard cifrado**: Curve25519, ChaCha20‑Poly1305 y clave precompartida por móvil.
  - Funciona en casa y fuera. Solo pasa por el túnel el tráfico de la app.
  - Mantiene la conexión por Wi‑Fi y por HTTPS anclado.
  - Se compila sin Gradle con `packaging/build_apk.py` e incluye `libwg-go.so` oficial, con la huella comprobada.
  - **Firmada con una clave nueva**: desinstala la app 1.3 antes de instalarla.
- **Nexus-Office autónomo** (`Nexus-Office-1.4.0-android-autonomo.apk`, `ai.nexus.office.autonomo`, Android 9+): Nexus-Office completo dentro del móvil, sin PC.
  - **La APK lo lleva todo menos los modelos**: Python 3.12 para ARM64, Nexus-Office, sus dependencias y `llama-server`.
    - `llama-server` va en tres variantes (ARMv8, dotprod, i8mm) y se elige según la CPU.
  - **Modelo según el móvil**: solo descarga el modelo adecuado a su RAM (SmolVLM 500M, Qwen3‑VL 2B o 4B). Con Wi‑Fi empieza solo; con datos móviles pregunta.
  - Servidor en `127.0.0.1` dentro de un servicio en primer plano.
  - **Sin SDK, NDK ni Gradle de Google**:
    - `packaging/build_android_runtime.sh` compila Python y llama.cpp para `aarch64-linux-musl` con zig y lo prueba emulando ARM64 con qemu.
    - `packaging/build_apk_autonomo.py` monta la APK.
  - Resolvedor DNS propio (`nexus/android_dns.py`), porque Android no tiene `/etc/resolv.conf`.
  - Las herramientas que ejecutan Python lanzan el intérprete a través del cargador (`nexus/proc.py: python_cmd`).
- `nexus/android_main.py`: arranque de Nexus-Office dentro de la app, con datos privados, ajustes para móvil, certificados raíz y la variante de `llama-server` adecuada a la CPU.
- Perfil de hardware para Android: modelos más pequeños según la RAM, como mucho 4 hilos, y solo modelos que la app puede usar.

### VPN cifrada en el PC
- Nuevo `nexus/remote/vpn.py` y panel *Red COOP → VPN cifrada para la app Android*.
  - Genera las claves y la configuración `wg-quick` del PC.
  - Genera un código `NEXUSV1:` (o un `.conf` para la app oficial de WireGuard) por móvil.
- Levanta o recarga el túnel solo si Nexus-Office tiene permisos. Si no, da los comandos para Windows, Linux, macOS y WSL.
- Abre el puerto UDP por UPnP.
- Red del túnel `10.78.0.0/24`, distinta de la red cooperativa.
- API `/api/vpn…`, documentación en [VPN](docs/VPN.md).
- Prueba de extremo a extremo con WireGuard real: `packaging/test_vpn_e2e.sh`.

### También en esta versión
- **ZIP con proyectos o documentación** como entrada, desde el chat (📎) o la Biblioteca RAG. Se descomprime de forma segura en `data/sandbox/proyectos/<nombre>/` y sus documentos y código se indexan en el RAG con la ruta de cada fichero. El mensaje del chat indica al agente dónde está. Detalles en [RAG](docs/RAG.md#archivos-zip-rar-7z-y-tar-con-proyectos-o-documentación).
- **Rastreo (crawling) de sitios de documentación** para el RAG: desde la Biblioteca (*RASTREAR*), el chat (*«rastrea la documentación de https://…»*) o la herramienta `crawl_docs` del Investigador. Recorre la sección (o todo el dominio) respetando robots.txt y el sitemap, indexa las páginas como un documento con la URL en cada fragmento y aprende un resumen. Volver a rastrear actualiza. El agente Nexus-Office la tiene siempre; el Investigador, en instalaciones nuevas. Detalles en [RAG](docs/RAG.md#rastreo-crawling-de-un-sitio-de-documentación).
- `file_list` acepta una subcarpeta. Programador gana `rag_search`, e Investigador gana `file_list`/`file_read` (solo en instalaciones nuevas: los agentes ya creados en `data/agents` no cambian).

### Pruebas
- 127 pruebas: grafos por agente, archivos (incluidos RAR reales: RAR3, RAR5 sólido, cifrado y con enlace malicioso), formatos, importación en lote, rastreo paralelo idéntico al secuencial, VPN, app Android autónoma (DNS propio, variante de CPU, modelos por RAM) y arranque autónomo.
- La APK autónoma se probó emulando ARM64 con qemu, arrancada igual que la app:
  - descompresión del runtime;
  - Nexus-Office responde en unos 16 s;
  - importación de un ZIP;
  - Python hijo lanzado por el cargador;
  - `llama-server` de la variante adecuada;
  - parada limpia con SIGTERM.

## 1.3.0 — 2026-10-12
- **Acceso remoto por internet** (`nexus/remote/`): un segundo puerto HTTPS (8443 por defecto) sobre la misma aplicación; el puerto local no cambia.
  - **Cifrado:** solo TLS 1.3. El intercambio de claves es post‑cuántico (X25519MLKEM768) o X25519, con secreto hacia adelante. Los datos van con AES‑GCM, o con ChaCha20‑Poly1305 si el cliente lo prefiere.
  - **Identidad del servidor:** certificado propio ECDSA P‑256 anclado por su huella SHA‑256.
  - **Dispositivos emparejados:** código de un solo uso de 60 bits que caduca en 10 minutos. Cada dispositivo recibe una llave de 256 bits (el servidor solo guarda su huella) y se puede revocar al momento.
  - **Protección del puerto:** `RemoteGate` exige la llave en HTTP y WebSocket aunque la conexión venga del propio equipo. Añade cabeceras de seguridad y una cookie `HttpOnly`/`Secure`/`SameSite=Strict`.
  - **Fuerza bruta:** 8 fallos en 15 minutos bloquean la IP, tanto en códigos como en el PIN de administrador. No se aplica en el propio equipo.
  - **Router:** apertura del puerto por UPnP (solo a petición), IP pública y aviso de CGNAT con alternativas (Tailscale, ZeroTier, túnel).
  - Panel en *Red COOP → Acceso remoto*.
  - Apagado limpio de ambos puertos en 3 segundos como máximo, aunque un móvil se haya quedado sin cobertura.
- **Asistente del router** (*Ajustes*, y botón en *Red COOP → Acceso remoto*):
  - Detecta la puerta de enlace, la IP y la MAC de este equipo.
  - Identifica la marca del router (15 fabricantes y operadores) y abre su configuración en el navegador.
  - Si el router admite UPnP, abre el puerto con un clic. Si no, muestra la ruta del menú y la regla exacta para copiar, y aconseja la reserva DHCP.
  - Comprueba la IP pública y el CGNAT, y genera el código para la app.
  - Solo lee: nunca inicia sesión en el router.
  - Tolera routers con cabeceras HTTP no estándar o TLS antiguo.
- **App Android 1.3:** *Conectar desde internet* con el código `NEXUSR1:…`.
  - Ancla la huella del certificado: rechaza cualquier otro servidor.
  - Elige sola la dirección que responde (internet, casa o Tailscale).
  - Se empareja automáticamente y guarda la llave.
- Guía [Acceso remoto](docs/ACCESO_REMOTO.md) y API documentada.

## 1.2.0 — 2026-10-11
- **Interfaz adaptativa para móvil y tablet**: `js/device.js` detecta antes del primer pintado el cliente (app Android, PWA o navegador), el tipo de pantalla, la orientación, si es táctil y si el equipo es modesto, y elige el diseño.
  - **Móvil**: barra inferior con Chat, Salón, Núcleo y Estudio más la hoja **«Más»** con el resto de secciones; avatar en franja plegable sobre el chat (se recoge sola con el teclado abierto); controles de 44 px; en horizontal, barra lateral compacta.
  - **Tablet**: avatar en franja en vertical y en columna lateral en horizontal; rejillas a una o dos columnas.
  - En pantallas táctiles, Intro hace salto de línea y se envía con el botón; acciones de los mensajes siempre visibles.
  - Equipos modestos: sin desenfoques ni animaciones de fondo, estrellas a 30 fps y avatar 3D sin antialias; los avatares dejan de dibujar cuando no se ven.
  - Ajuste **«Diseño de la interfaz»** (automático, móvil, tablet, escritorio), guardado solo en ese dispositivo; también `?layout=phone|tablet|desktop|auto`.
- **App Android 1.2**: informa de la clase de pantalla oficial (móvil o tablet, ≥ 600 dp), botón **Atrás** integrado (cierra hojas y diálogos y vuelve al chat antes de salir), barras del sistema con el color del tema, respuesta háptica y aviso de giros, plegables y multiventana.

- `packaging/split_parts.py`: prepara `dist/envio/` para enviar los paquetes al móvil. Divide los grandes en partes de menos de 25 MB, con scripts para unirlas y comprobación SHA-256.
- **`requirements.txt` de referencia completa**, generado por `packaging/requirements_ref.py`:
  - Paquetes pip con su versión mínima y la versión probada.
  - Python portable (python-build-standalone 3.11.17, build 20261009, para las 6 plataformas).
  - llama.cpp y stable-diffusion.cpp, con sus versiones probadas y variantes de binario.
  - Extras de voz y NPU, y herramientas de compilación.
  - Catálogo de pesos de IA y los pesos sugeridos para 11 perfiles de hardware.
- Los instaladores usan como Python de reserva esa misma versión probada (antes 3.11.10 de 2024).
- **Carpeta de código fuente limpio** en cada compilación (`dist/Nexus-Office-<versión>-codigo-fuente/` y su `.zip`): sin entorno, datos, modelos, cachés ni compilados. La compilación se detiene si encuentra claves de firma, configuración con secretos o logs.
- Corregido: si fallaba la red, la caché de la app instalada (service worker) devolvía la página principal en lugar de un script nuevo, y la interfaz se rompía.

## 1.1.0 — 2026-10-10
- Avatar **Geisha** reconstruido a partir de la hoja de personaje: segmentación de cada plano, rig de siluetas (casco visual) y proyección multivista por shader; busto y cuerpo entero (`packaging/geisha_assets.py` lo regenera desde otra hoja).
- **Mejorador de prompts** (LLM o reglas) para chat, imagen y música; herramienta `enhance_prompt` y mejora automática de prompts de imagen.
- **Gestor de modelos abierto** a cualquier modelo: repositorios completos, GGUF multiparte, emparejado automático de `mmproj`, `ollama pull`, anotaciones de compatibilidad/ajuste/velocidad y ★ recomendado.
- **Gestor de drivers** de la GPU con actualizadores oficiales (Intel DSA, páginas de NVIDIA/AMD, Windows Update, ubuntu-drivers/Mesa).
- **Editor de temas** con 7 plantillas, vista previa en vivo y exportación/importación.
- **App Android (APK)** cliente con descubrimiento en red, voz nativa y guía Termux; **clave de red local** para el acceso desde otros dispositivos; paquete Termux servido por el propio Nexus-Office.
- Instaladores **`.run`** autoextraíbles para Linux (normal y offline), paquete **Python** multiplataforma y **zip maestro** por plataformas.

## 1.0.0 — 2026-10-10
- Primera versión: agente multimodal portable (texto, visión, imagen, música, voz).
- Memoria semántica, grafo de conocimiento, aprendizaje por retroalimentación y avatar 3D del grafo.
- Sistema de agentes configurable con delegación y **salón virtual** (habilidades aprendidas y propuesta de nuevos agentes).
- Herramientas modulares supervisadas, sandbox, auditoría y propuestas de herramientas por los agentes.
- Detección de hardware (NVIDIA, AMD, Intel, Apple, NPU), perfiles óptimos, monitorización y autoajuste en cada arranque.
- Runtimes nativos llama.cpp / stable-diffusion.cpp por variante de hardware; llamadas nativas a herramientas.
- Gestor de modelos con Hugging Face, Ollama, ModelScope, Civitai y Kaggle; importación local; lector GGUF.
- Red cooperativa: LAN virtual cifrada entre instancias a través de internet.
- Modos Usuario y Administrador; webui futurista instalable como PWA.
- Instaladores para Windows (x64/ARM64, .exe gráfico), Linux (x86_64/ARM64, .deb), macOS, Android (Termux) y Docker.
- Biblioteca **RAG** (PDF/TXT/MD/HTML/DOCX, búsqueda híbrida, citas) y herramienta **URL → PDF estructurado** que publica, archiva y asimila el contenido.
- **Avatares seleccionables**: geisha del eclipse 3D animada, grafo neuronal y modelos GLB/GLTF/VRM propios con sincronía labial.
