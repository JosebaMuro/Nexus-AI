# Biblioteca RAG y conversión URL → PDF

## RAG (generación aumentada por recuperación)
- **Formatos de documento**:
  - PDF, por páginas.
  - Word (DOCX).
  - OpenDocument: ODT, ODS y ODP.
  - PowerPoint (PPTX) y Excel (XLSX), por diapositiva u hoja.
  - EPUB, por capítulo.
  - RTF, HTML, Markdown, TXT, CSV, JSON y YAML.
  - Código fuente, con más de 80 extensiones.

  Cada documento se copia en `data/rag/files/`, así que la biblioteca viaja con la carpeta. `GET /api/rag/formats` devuelve la lista exacta.
- **Archivos comprimidos**: ZIP, RAR, 7z y TAR (`.tar`, `.tar.gz`/`.tgz`, `.tar.bz2`, `.tar.xz`). Ver la sección siguiente.
- **Importación en lote**: suelta muchos ficheros a la vez en la Biblioteca, sueltos y comprimidos mezclados. Se envían en tandas de 20 a `POST /api/rag/import` (multipart, campo `files`). La respuesta trae `results` (uno por fichero, en el mismo orden), `ok`, `failed` y `seconds`. Si un fichero falla, se informa de él y los demás se importan igualmente.
- **Troceado** por párrafos (~900 caracteres, solapamiento de 150) con número de página.
- **Búsqueda híbrida**: `0,7 × similitud semántica + 0,3 × coincidencia de términos`. Usa el mismo motor de embeddings que la memoria (sentence-transformers › Ollama › hashing).
- **En cada mensaje**, los 3 fragmentos más relevantes (si superan el umbral) se añaden al contexto del agente con su fuente; el modelo cita como `[n]`. Sin LLM, el núcleo integrado responde citando los fragmentos.
- **Herramientas**: `rag_search`, `rag_list`, `url_to_pdf` y `crawl_docs`.
- **Interfaz**: *Núcleo → Biblioteca RAG*: procesar URL, rastrear documentación, arrastrar documentos, buscar, abrir, mover de grafo y borrar.

## Leer páginas como un navegador (2.2)
Muchas webs (tiendas, documentación hecha con React, Vue o Angular, paneles) llegan casi vacías y se pintan después con
JavaScript. Para que la Biblioteca, *URL a PDF*, el rastreador y la herramienta web de los agentes lean lo mismo que tú:

- Nexus-Office busca un **Chrome, Edge, Chromium o Brave** en el equipo (Windows, macOS y Linux, también el
  `chrome-headless-shell` de Playwright) y lo abre **sin ventana** (`--headless --dump-dom`), dejando unos segundos
  para que la página ejecute su JavaScript. Se queda con el DOM final y lo procesa como cualquier HTML.
- **Cuándo**: en *Automático* solo si la página lo necesita (un `<div id="root">` o `__next` vacío, `<noscript>` que
  pide JavaScript, muchos scripts y poco texto). *Siempre como navegador* lo hace con todas; *Nunca* lo apaga.
  Se cambia en el menú **MEMORIA** de la barra superior o con `rag.browser` (`auto`, `siempre`, `nunca`).
- **Seguridad**: el navegador usa un perfil propio de Nexus-Office (no el tuyo), que se vacía de cookies y
  almacenamiento antes de cada lectura, y sale a internet por un proxy propio que **solo deja llegar a direcciones
  públicas** (nada de `localhost`, la red local ni los metadatos de la nube), salvo que se active
  `rag.allow_private_urls`. Como mucho hay dos navegadores a la vez.
- Si no hay navegador, o tarda más de 40 s, se usa la descarga normal y se avisa en la respuesta.
- Un administrador puede fijar uno concreto con `rag.browser_path`. `GET /api/rag/navegador` dice el modo y el
  navegador encontrado. La herramienta `web_fetch` de los agentes indica `"navegador": true` cuando lo ha usado.

## Grafo general o grafo de un agente
El grafo de conocimiento tiene un **grafo general**, común a todos los agentes, y dentro de él un **subgrafo aislado por agente**.

- **Importar en → grafo general**: el documento lo consultan todos los agentes y su nodo entra en el grafo general.
- **Importar en → solo un agente**: solo ese agente lo encuentra (en el contexto de cada mensaje y con `rag_search`/`rag_list`), y el documento entra en su grafo individual. Su resumen y puntos clave, si es una URL o un rastreo, quedan como recuerdos privados de ese agente.
- El selector afecta a documentos sueltos, comprimidos, URL → PDF y rastreo. En la lista, cada documento muestra su grafo y un desplegable para moverlo; el nodo se mueve con él.
- Un agente puede importar para sí mismo con `url_to_pdf` o `crawl_docs` y `target: "agente"`. Por defecto se importa al general.
- Si se borra un agente, sus documentos pasan al grafo general.
- En la conversación, lo que se sabe del usuario (perfil, gustos, pautas, relaciones) va al grafo general. El resto lo reparte el organizador: el saber hacer de una especialidad va al subgrafo de ese agente y el conocimiento común, al general. Ver [MEMORIA.md](MEMORIA.md).
- *Ajustes → Gestor de conocimiento → Leer un ZIP* hace más que importarlo: inserta los grafos que traiga, agrupa los documentos por temas y los razona con el LLM. Ver [MEMORIA.md](MEMORIA.md#leer-un-zip-también-rar-7z-y-tar).

## Archivos ZIP, RAR, 7z y TAR con proyectos o documentación
Un archivo comprimido se puede adjuntar en el **chat** (botón 📎) o soltar en la **Biblioteca RAG**. Nexus-Office:

1. **Descomprime** el archivo de forma segura en `data/sandbox/proyectos/<nombre>/`. Si todo cuelga de una única carpeta raíz, se aplana. Si el nombre ya existe, se usa `<nombre>_2`, `<nombre>_3`…
2. **Indexa** en la biblioteca los documentos (PDF, DOCX, HTML, MD, TXT…) y el código y la configuración (más de 80 extensiones, además de `Dockerfile`, `Makefile`, `README`…) como **un único documento** del tipo `zip`, `rar`, `7z` o `tar`. Cada fragmento lleva delante la ruta del fichero (`[proyectos/x/src/main.py]`), así que las citas dicen de qué fichero sale. La primera página es la **estructura** del proyecto.
3. En el chat, el mensaje lleva una nota con la carpeta, para que el agente pueda explorarla con `file_list` (con `path`) y `file_read` y buscar con `rag_search`. Sin texto, pide un resumen del contenido.

Herramientas de descompresión:

| Formato | Cómo se descomprime |
|---|---|
| ZIP y TAR | Con la biblioteca estándar de Python, sin instalar nada |
| RAR | Con el primer programa disponible: `unrar`, `7z` (con el códec RAR), `bsdtar` o `unar` |
| 7z | Con `7zz`/`7z`/`7za`, `bsdtar` o `unar` |

Si no hay ninguno, Nexus-Office dice qué instalar:

- Windows: 7-Zip o WinRAR. En Windows 10/11 también vale el `tar` del sistema.
- Ubuntu y WSL: `sudo apt install unrar 7zip`. El paquete `7zip` de Debian/Ubuntu no trae el códec RAR, así que para RAR instala `unrar` (o `unar`).
- macOS: `brew install sevenzip` o `brew install unar`.
- Termux: `pkg install unrar p7zip`.

Los programas externos descomprimen primero en una carpeta de cuarentena. Nexus-Office lee antes el listado del archivo para aplicar los mismos límites que al ZIP. Después revisa cada fichero, como con el ZIP, antes de moverlo a su carpeta. Los ficheros cifrados o dañados se descartan y se informa de ellos.

Seguridad y límites:
- Rechaza rutas absolutas, con `..` o con unidad de Windows (*zip slip*), enlaces simbólicos y entradas cifradas.
- Contra bombas ZIP: 20 000 ficheros, 1 GB descomprimido, 50 MB por fichero y ratio de compresión ≤ 200 (se comprueba el tamaño real al descomprimir, no el declarado).
- No descomprime `.git`, `node_modules`, `__pycache__`, `venv`/`.venv`, `dist`, `build`, `target` ni similares.
- No indexa (aunque sí descomprime) `.env`, claves (`.pem`, `.key`, `id_rsa`…), minificados, mapas, lockfiles ni `vendor/`/`third_party/`. Tampoco binarios. Por fichero indexa como máximo 512 KB, y en total hasta 20 000 ficheros.
- **Proyectos grandes por partes**: lee los ficheros en grupos de 200 y, cuando una parte llega a 6 millones de caracteres, sigue en la siguiente. Las partes son documentos correlativos de la biblioteca («Proyecto X · parte 2 (ficheros 201-400)»). Borrar la primera borra todas, junto con el archivo y su carpeta.
- Al borrar el documento de la biblioteca se borran también el archivo y su carpeta descomprimida.

## Rendimiento: procesador al máximo
- **Lectura en paralelo**: en una importación en lote, cada documento suelto se lee en un núcleo distinto, con un pool de procesos (`nexus/parallel.py`). Los archivos comprimidos se procesan uno tras otro, porque cada uno ya usa el disco a fondo.
- **Embeddings**: el motor *hashing* reparte entre núcleos los lotes a partir de 256 fragmentos. Ollama recibe lotes de 128 y sentence-transformers de 64. Los fragmentos se guardan en la base de datos con una sola transacción.
- **Ajustes** (`performance` en la configuración):
  - `workers`: número de procesos. 0 = automático, uno por núcleo disponible.
  - `crawl_connections`: descargas simultáneas al rastrear. 0 = automático, entre 4 y 8.
  - `NEXUS_NO_PROCESSES=1` fuerza hilos en lugar de procesos. En Android se hace solo.

## Rastreo (crawling) de un sitio de documentación
Para que Nexus-Office aprenda una documentación entera (un manual, la API de una librería, la guía de un producto), dale la página inicial:

- **Interfaz**: *Núcleo → Biblioteca RAG*, fila **RASTREAR**: URL, máximo de páginas (40 por defecto, hasta 100 000) y ámbito (*esta sección* o *todo el dominio + grafo*). Con *todo el dominio* el máximo no se aplica. Se ejecuta en segundo plano y aparece en el panel de rastreos.
- **Chat**: *«Rastrea la documentación de https://docs.python.org/3/tutorial/ y apréndela»*. Con *«todo el dominio»* recorre el sitio entero. Sin LLM, el núcleo integrado lo reconoce y lanza la herramienta.
- **Herramienta** `crawl_docs` (`url`, `max_pages`, `max_depth`, `scope`, `background`, `job`, `continue`), del agente Investigador. **API**: `POST /api/rag/crawl` `{url, max_pages, max_depth, scope, background, batch_pages}`.

Cómo recorre el sitio:
1. **Ámbito**: por defecto, la carpeta de la URL inicial (`https://docs.x.org/3/tutorial/` → solo `/3/tutorial/…`); con `scope=domain`, todo el dominio registrable con todos sus subdominios (ver *Todo el dominio, sin excepciones* más abajo). Si la portada redirige (`/docs` → `/docs/es/latest/`), el ámbito se toma del destino.
2. **Recorrido en anchura** hasta `max_depth` saltos (3 por defecto). Sigue también los enlaces del menú lateral, aunque ese menú no se indexa.
3. **robots.txt**: respeta `Disallow` y `Crawl-delay` (como máximo 5 s).
   - Si el sitio declara `Crawl-delay`, Nexus-Office va de una en una página.
   - Si no, descarga varias páginas a la vez (`performance.crawl_connections`, de 4 a 8 por defecto) por conexiones reutilizadas (keep-alive), con al menos 0,1 s entre peticiones.
   - El análisis de cada página (extracción y deduplicación) se reparte entre los núcleos.
   - Los resultados se aplican en el orden de descubrimiento, así que el resultado es idéntico al de un rastreo secuencial, pero varias veces más rápido.
4. **sitemap.xml** (el declarado en robots.txt o `/sitemap.xml`, también índices de sitemaps) como semilla, filtrado por el ámbito.
5. **Descarta**: otros dominios, imágenes, CSS/JS, descargas y PDF, búsquedas, inicios de sesión, `_sources`/`_static`, páginas `noindex`, canónicas ya leídas, contenido duplicado y páginas casi vacías. Quita los parámetros de seguimiento (`utm_*`, `fbclid`…) y los `#fragmentos`.
6. **Extracción** de cada página con el mismo extractor estructurado que URL → PDF.

Qué guarda:
- **Un único documento** del tipo `web` en la biblioteca, una página por página rastreada. Cada fragmento lleva delante su URL (`[https://docs.x.org/3/tutorial/classes.html]`), así que las citas dicen de qué página sale.
- Una **copia legible en Markdown** con índice (botón *ABRIR*), en `data/rag/files/`.
- **Asimilación**: resumen y puntos clave a la memoria; el sitio al grafo de conocimiento (`ha_leido`, `trata_de`, `contiene` con los títulos de las páginas).
- **Volver a rastrear** la misma URL y ámbito **actualiza** el documento: el anterior se borra al terminar el nuevo.

### Todo el dominio, sin excepciones (2.0)
Con el ámbito **todo el dominio** Nexus-Office lee el sitio entero, tenga las páginas que tenga:
- **Sin límite** de páginas ni de profundidad. `max_pages` y `max_depth` se ignoran.
- **Todos los subdominios** del dominio registrable (`www.`, `blog.`, `docs.`…; también `.co.uk`, `.com.es` y similares), cada uno con su propio `robots.txt`.
- **Documentos enlazados**: PDF, Word, LibreOffice (odt, ods, odp), PowerPoint, Excel, EPUB, RTF, texto, CSV y Markdown, hasta 60 MB cada uno, con los mismos lectores de la biblioteca RAG. Las URL que aparecen dentro del texto de un documento también se siguen.
- **Páginas cortas** (desde 5 palabras): glosarios, fichas y avisos también entran.
- **Sitemaps**: hasta 2000 ficheros de sitemap (índices incluidos).
- Siempre en **segundo plano y por lotes**; si se cierra Nexus-Office, el trabajo **se reanuda solo** al volver a arrancar.
- Solo quedan fuera lo que el sitio prohíbe en `robots.txt`, otros dominios, imágenes, CSS/JS, binarios y el contenido duplicado.

#### Grafo de conocimiento multicapa del dominio
Al terminar, el rastreo construye un **grafo propio del dominio** en `data/rag/crawls/<id>.kg.sqlite`, por capas:
1. **Fuentes**: dominio, secciones (subdominios y carpetas), páginas y documentos, con sus enlaces.
2. **Conceptos**: términos clave de cada página y documento, relacionados por coaparición y por parecido semántico (modelo de embeddings).
3. **Afirmaciones**: frases con datos (cifras, fechas, definiciones). Se **contrastan entre fuentes**: las que dicen lo mismo en páginas distintas quedan como *contrastada (N fuentes)*; las que dan cifras distintas sobre lo mismo, como *discrepante*, con cada versión y su fuente; el resto, como *fuente única*. También se cotejan con los otros documentos de la biblioteca. Los textos de plantilla que solo cambian un número (por ejemplo, «Capítulo 1…», «Capítulo 2…») no cuentan como discrepancia.
4. **Temas**: grupos de conceptos muy relacionados.
5. **Síntesis razonada**: con el modelo de IA activo, cada tema recibe un resumen y sus conclusiones, que razona solo a partir de las afirmaciones contrastadas y citando fuentes. Sin modelo, se queda en las capas anteriores.

En el panel de rastreos, cada trabajo de dominio muestra páginas, afirmaciones contrastadas y nodos, y tiene:
- **CREAR GRAFO / REHACER GRAFO**: lo construye o lo vuelve a construir (por ejemplo, tras activar un modelo con razonamiento).
- **⬇ ZIP**: descarga el **paquete de conocimiento** (`graph.json` en formato nexus-kg, `conocimiento.md` legible y `manifest.json`).
- **DAR A AGENTE**: importa el grafo en el subgrafo de un agente (o en el general) y añade el texto de conocimiento a su biblioteca, así que el agente **adquiere ese conocimiento** y lo cita.
- El ZIP se puede importar en otro Nexus-Office con **IMPORTAR CONOCIMIENTO DE DOMINIO (.zip)** en la *Biblioteca RAG* (va al grafo elegido en *Importar en*) (o `POST /api/rag/domain-knowledge/import`, con el fichero y el campo `agent`).

API: `POST /api/rag/crawls/{id}/knowledge` (construir), `GET /api/rag/crawls/{id}/knowledge` (ZIP), `POST /api/rag/crawls/{id}/knowledge/import` `{agent}`.

Rendimiento medido: 5000 páginas se añaden al grafo en unos 13 s y el grafo se construye en unos 3 s, con unos 350 MB de memoria.

### Dominios enteros: rastreo por lotes
Un dominio grande no cabe de una vez, así que se procesa en **lotes correlativos** que se guardan según terminan:
- Si `max_pages` supera el tamaño de lote, el rastreo se convierte en un **trabajo por lotes**. El lote depende de la RAM: 50 páginas con menos de 4 GB, 100 en móviles o con menos de 8 GB, 200 con menos de 16 GB y 300 a partir de ahí. `rag.crawl_batch_pages` lo fija a mano.
- Cada lote es un documento propio, ordenado: «Documentación: X · lote 003 (págs. 401-600)». El resumen y los puntos clave se hacen con el primero.
- Tras cada lote se guarda el trabajo en `data/rag/crawls/<id>.json`: la cola de URLs pendientes en orden de descubrimiento, las visitadas y las huellas del contenido. Al **reanudar** (después de pausar, de reiniciar Nexus-Office o de un corte) sigue por la página siguiente, sin repetir ninguna y en el mismo orden.
- **Panel de rastreos** en la Biblioteca RAG: progreso, lotes y cola pendiente de cada trabajo, con **PAUSAR**, **REANUDAR** y **✕** (borra el trabajo y, si quieres, sus documentos). Un trabajo en curso cuando Nexus-Office se cerró aparece como *interrumpido* y se reanuda igual.
- **Chat**: «aprende todo el dominio https://docs.x.org» lanza el trabajo en segundo plano (hasta 100 000 páginas); «sigue rastreando https://docs.x.org» reanuda el último sin terminar. Desde la herramienta, `crawl_docs` con `job=<id>` o `continue=true`.
- Volver a rastrear la misma URL sustituye todos los lotes antiguos al terminar el primer lote nuevo.
- API: `GET /api/rag/crawls`, `POST /api/rag/crawls/{id}/pause`, `POST /api/rag/crawls/{id}/resume` y `DELETE /api/rag/crawls/{id}?docs=true`.

Seguridad y límites: solo direcciones públicas (se comprueba en cada petición y tras cada redirección; `rag.allow_private_urls: true` permite la red local), 3 MB por página, 6 millones de caracteres por documento o lote y 15 minutos por tanda (un trabajo por lotes sigue en la siguiente tanda). Las páginas que solo muestran contenido con JavaScript se omiten por tener poco texto. La herramienta tiene **riesgo medio** (accede a internet): con la política por defecto el usuario la aprueba en una tarjeta.

## URL → PDF estructurado → conocimiento
Pide en el chat *«Procesa https://… y hazme un PDF»* (o usa *Procesar URL* en la Biblioteca):

1. **Descarga** (solo direcciones públicas; se bloquean IPs locales también tras redirecciones; máx. 8 MB).
2. **Extracción estructurada**: título, descripción, autor, fecha, idioma, sitio; jerarquía de secciones; párrafos, listas, tablas, bloques de código, citas; imágenes y enlaces. Prioriza `<article>`/`<main>` y descarta navegación, pies, formularios, banners de cookies, etc.
3. **Resumen y puntos clave** con el LLM (o resumen extractivo sin LLM).
4. **PDF maquetado**: portada, índice con números de página, marcadores PDF navegables, resumen, puntos clave, contenido, galería de imágenes incrustadas (hasta 8), enlaces y metadatos. Fuentes Unicode del sistema (Segoe UI/DejaVu/Arial/Roboto) con respaldo automático.
5. **Publicación** al usuario: tarjeta de PDF en el chat (`data/outputs/docs/`).
6. **Archivo en el RAG**: el PDF generado se indexa en la biblioteca.
7. **Asimilación**: el resumen y los puntos clave pasan a la memoria a largo plazo; el documento se añade al grafo de conocimiento (`ha_leido`, `trata_de`, `publicado_en`, `escrito_por`, `contiene`) y aparece en el avatar.

Medido en el equipo de desarrollo (CPU i3, Qwen3‑VL 2B): artículo de Wikipedia de 4 540 palabras → PDF de 25 páginas, 16 secciones, 8 imágenes, 74 fragmentos RAG y 14 conocimientos en ~68 s.

La herramienta tiene **riesgo medio** (accede a internet): con la política por defecto el usuario la aprueba en una tarjeta.
