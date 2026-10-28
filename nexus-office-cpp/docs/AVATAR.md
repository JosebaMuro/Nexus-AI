# Avatares

Se eligen en **Ajustes → Avatar** (modo usuario). Todos reaccionan al estado (en línea, pensando, herramienta, hablando, escuchando), al volumen de la voz y muestran los nodos del grafo de memoria orbitando como partículas.

| Avatar | Descripción | Coste |
|---|---|---|
| **Geisha del eclipse** (por defecto) | Construida a partir de la **hoja de personaje** (planos A–H): cada plano se segmenta del fondo, las siluetas de frente y perfil definen un **casco visual 3D** (anillos de anchura y profundidad por altura) y un shader proyecta las fotos de frente, perfiles y espalda mezclándolas según la orientación de la superficie. Cabeza articulada sobre cuerpo entero (kimono, obi, manos, sandalias), varillas *kanzashi* 3D, párpados y boca animados, fondo de pagoda y eclipse con corona del color del estado. **Doble clic** alterna busto / cuerpo entero. Si faltan los recursos, se usa la versión procedural anterior. | WebGL; fluido en gráficas integradas |
| **Grafo neuronal** | El avatar *es* la memoria: núcleo luminoso y entidades aprendidas en una esfera 3D en canvas 2D. | Mínimo |
| **Modelo propio** | Sube un **GLB, GLTF (embebido) o VRM**. Se escala y encuadra solo, reproduce su animación (prefiere una llamada *idle*), mueve la cabeza hacia el puntero si tiene hueso `head` y abre la boca con morphs `aa`, `mouthOpen`, `jawOpen`, `viseme_aa`… | Depende del modelo |

## Encuadre automático (2.0)
Todos los avatares 3D se encuadran solos en el hueco libre del panel, entre los rótulos, al cambiar el tamaño de la ventana, al girar el móvil o al cargar un modelo:
- **Busto** (cabeza y hombros) o **cuerpo entero**: doble clic alterna entre los dos.
- Los modelos propios en cualquier unidad (cm, mm, m) se normalizan a 1,75 m de alto; los tumbados o no humanos, por su lado mayor. Los VRM 0.x se giran para mirar a la cámara.
- Si el modelo no tiene hueso `head`, el busto toma el 18 % superior.
- La respiración ya no saca del encuadre a los modelos cuyo origen no está en los pies, y el aro de estado y las partículas se ajustan a la medida del modelo.

## Encuadre en el chat (2.2)
Los modelos muy grandes, muy pequeños o con el origen lejos del cuerpo ya no se salen del hueco del chat: la cámara
se coloca con la caja real del modelo ya escalado (no con la del fichero), en el hueco que deja libre el chat en
ordenador, tableta y móvil, y vuelve a encuadrar cuando cambia el tamaño del panel.

## Biblioteca de avatares 3D gratuitos (2.2)
En **Ajustes → Avatar → AVATARES 3D GRATIS** se buscan y descargan modelos gratuitos:

| Fuente | Qué hay | Licencia |
|---|---|---|
| **Open Source Avatars** (opensourceavatars.com) | Cientos de avatares VRM: 100Avatars de Polygonal Mind, ToxSam, chibis… El índice está en GitHub y los modelos en Arweave o IPFS (si una pasarela falla prueba otra). | Casi todos CC0 |
| **Modelos de referencia** | Muestras del VRM Consortium, Khronos glTF y three.js. | Abiertas |
| **Sketchfab** | Búsqueda de modelos descargables con licencia Creative Commons. Para descargar hace falta la clave de API gratuita de tu cuenta (*Ajustes → Avatar*, campo *Clave de API de Sketchfab*, `avatares.sketchfab_token`). | CC (la de cada modelo) |

Lo descargado queda en `outputs/avatars` con su miniatura, autor, licencia y origen (`biblioteca.json`), y se elige
como avatar en cualquier momento, también sin conexión. Solo se descargan direcciones que vienen del propio catálogo,
de 300 MB como mucho y en GLB, GLTF o VRM.

API: `GET /api/avatares/buscar?fuente=osa|muestras|sketchfab&q=...`, `POST /api/avatares/descargar` `{id}` (devuelve
un trabajo que se sigue en `GET /api/avatares/trabajo/{tid}`), `GET /api/avatares` (biblioteca),
`POST /api/avatares/{id}/usar` y `DELETE /api/avatares/{id}`.

## Crear un avatar de calidad a partir de imágenes
La geisha actual se genera a partir de la hoja de personaje (proyección de fotos sobre un volumen de siluetas): de frente y en perfil es fiel a la referencia, y en ángulos intermedios mezcla vistas. Para un modelo con malla y rig completos:

1. **Modelo propio GLB/VRM** creado en VRoid Studio o Blender, o generado con herramientas *image‑to‑3D* (Hunyuan3D, TripoSR, Meshy…) a partir de las imágenes de referencia, y después riggeado (Mixamo/AccuRIG) y con *blendshapes* de boca y parpadeo. Nexus-Office lo carga directamente.
2. **Mejorar la hoja**: planos de mayor resolución (≥1024 px por vista), fondo liso y vistas exactas de frente/perfil/espalda mejoran directamente el resultado de `nexus-construir avatar`.

### Imágenes de referencia necesarias
Del **mismo personaje**, coherentes entre sí (una *character sheet / turnaround*), PNG o JPG ≥ 1024 px, iluminación plana sin sombras duras, fondo liso:

| # | Imagen | Para qué |
|---|---|---|
| 1 | **Rostro frontal** neutro: mirando a cámara, boca cerrada, ojos abiertos, sin inclinación, sin pelo cubriendo la cara | Textura de la cara y proporciones |
| 2 | **Perfil izquierdo y derecho** (90°) de la cabeza | Forma del cráneo, nariz, mentón y volumen del peinado |
| 3 | **Vista 3/4** de la cabeza | Validar volúmenes |
| 4 | **Peinado desde atrás y desde arriba** + primer plano de los *kanzashi* (horquillas, peineta, flores, cadenas de cuentas) | Modelado del moño y adornos |
| 5 | **Primeros planos del maquillaje**: ojos y boca | Detalle de la textura facial |
| 6 | **Estampado del kimono en plano** (tela extendida, sin pliegues) + **obi** y **collar** | Texturas de tela |
| 7 | **Tatuajes** de cuello, pecho, brazos y manos vistos de frente | Textura de piel |
| 8 | **Cuerpo entero** en pose T o A, de frente y de lado (si se quiere más que el busto) | Proporciones y rig |
| 9 | Opcional: **expresiones** (boca en «A», «O», «I», ojos cerrados, sonrisa) | *Blendshapes* de habla y parpadeo |
| 10 | Opcional: **fondo** por separado (pagoda, eclipse) | Escenario |

Si las imágenes se generan con IA, pide una *"character turnaround sheet, front view, side view, back view, neutral expression, flat lighting, white background"* con el mismo personaje/semilla.

### Regenerar el avatar con otra hoja de personaje
```bash
build/nexus-construir avatar ruta/a/la_hoja.jpg       # sin ruta: la hoja actual (character_sheet.jpg)
```
Recorta los planos (coordenadas en `SHEET_BOXES`), segmenta, mide las siluetas y escribe `recursos/web/assets/geisha/` (PNG con transparencia + `rig.json`; `--salida` para otra carpeta). Las referencias de alineación (coronilla, barbilla, cuello, suelo) están al principio de `tools/construir/avatar.cpp`.
