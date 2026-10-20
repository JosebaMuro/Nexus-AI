# Avatares

Se eligen en **Ajustes → Avatar** (modo usuario). Todos reaccionan al estado (en línea, pensando, herramienta, hablando, escuchando), al volumen de la voz y muestran los nodos del grafo de memoria orbitando como partículas.

| Avatar | Descripción | Coste |
|---|---|---|
| **Geisha del eclipse** (por defecto) | Construida a partir de la **hoja de personaje** (planos A–H): cada plano se segmenta del fondo, las siluetas de frente y perfil definen un **casco visual 3D** (anillos de anchura y profundidad por altura) y un shader proyecta las fotos de frente, perfiles y espalda mezclándolas según la orientación de la superficie. Cabeza articulada sobre cuerpo entero (kimono, obi, manos, sandalias), varillas *kanzashi* 3D, párpados y boca animados, fondo de pagoda y eclipse con corona del color del estado. **Doble clic** alterna busto / cuerpo entero. Si faltan los recursos, se usa la versión procedural anterior. | WebGL; fluido en gráficas integradas |
| **Grafo neuronal** | El avatar *es* la memoria: núcleo luminoso y entidades aprendidas en una esfera 3D en canvas 2D. | Mínimo |
| **Modelo propio** | Sube un **GLB, GLTF (embebido) o VRM**. Se escala y encuadra solo, reproduce su animación (prefiere una llamada *idle*), mueve la cabeza hacia el puntero si tiene hueso `head` y abre la boca con morphs `aa`, `mouthOpen`, `jawOpen`, `viseme_aa`… | Depende del modelo |

## Crear un avatar de calidad a partir de imágenes
La geisha actual se genera a partir de la hoja de personaje (proyección de fotos sobre un volumen de siluetas): de frente y en perfil es fiel a la referencia, y en ángulos intermedios mezcla vistas. Para un modelo con malla y rig completos:

1. **Modelo propio GLB/VRM** creado en VRoid Studio o Blender, o generado con herramientas *image‑to‑3D* (Hunyuan3D, TripoSR, Meshy…) a partir de las imágenes de referencia, y después riggeado (Mixamo/AccuRIG) y con *blendshapes* de boca y parpadeo. Nexus-Office lo carga directamente.
2. **Mejorar la hoja**: planos de mayor resolución (≥1024 px por vista), fondo liso y vistas exactas de frente/perfil/espalda mejoran directamente el resultado de `geisha_assets.py`.

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
venv/python.exe packaging/geisha_assets.py ruta/a/la_hoja.jpg
```
Recorta los planos (coordenadas en `SHEET_BOXES`), segmenta, mide las siluetas y escribe `nexus/web/assets/geisha/` (PNG con transparencia + `rig.json`). Las referencias de alineación (coronilla, barbilla, cuello, suelo) están en `HEAD` y `BODY` al principio del script.
