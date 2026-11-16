# Oficina Herrero: estructuras metálicas

La pestaña **HERRERO** (y el agente *Herrero*) es una oficina técnica de cerrajería y estructuras metálicas en
pequeño. Le describes la estructura con sus medidas, por ejemplo:

> Porche para dos coches de 6 x 4 m y 2,6 m de altura, galvanizado, cubierta de panel sándwich, a 650 m de altitud

y en menos de un segundo (también en el móvil y sin conexión) te entrega:

- los **perfiles** recomendados con sus comprobaciones (aprovechamiento de cada uno),
- el **despiece** con marca, cantidad, perfil, longitud y peso de cada pieza, y las chapas y anclajes,
- la **lista de corte** optimizada en barras comerciales de 6 o 12 m,
- las **uniones soldadas** con su garganta, metros de cordón, kilos de hilo y su símbolo ISO 2553,
- el **plano de montaje** A3 (alzado frontal, alzado lateral y planta a escala normalizada, con cotas y marcas),
- la **vista isométrica**,
- un **PDF de 3 hojas** (montaje; isométrica y despiece; lista de corte, chapas y comprobaciones),
- una **imagen realista** de la estructura,
- el despiece en **CSV** para la hoja de cálculo o el programa de presupuestos.

> **Aviso.** Es un **predimensionado orientativo** para presupuestar y preparar el taller. Una estructura real
> necesita el proyecto y la firma de un técnico competente (CTE) y su ejecución con marcado CE según EN 1090.

## Qué estructuras sabe hacer

| Tipo | Se reconoce por | Qué calcula |
|---|---|---|
| Porche, cochera, marquesina | porche, cochera, marquesina, carport, tejadillo, cubierta | Pórticos con jácena y pilares empotrados en la base, correas, placas base y de cabeza, levantamiento por viento |
| Pérgola | pérgola | Como el porche, con lamas |
| Nave | nave, hangar, cobertizo | Pórticos a dos aguas (pilar y dintel), correas, vigas de atado, cruces de San Andrés en cubierta y fachadas |
| Entreplanta | entreplanta, altillo, plataforma | Malla de pilares, vigas, viguetas, suelo (tramex, chapa, tablero o losa colaborante), barandillas, cruces |
| Escalera | escalera | Huella y contrahuella del CTE DB-SUA, zancas, peldaños de chapa lagrimada, barandilla |
| Barandilla | barandilla, baranda, antepecho | Montantes, pasamanos, travesaño y barrotes con hueco < 10 cm; altura 0,90 o 1,10 m según desnivel |
| Pasarela | pasarela, puente peatonal | Vigas, travesaños, suelo de tramex, barandillas a los dos lados, pilares y arriostramiento |

Del texto también se leen: medidas (`6x4x3`, `6 m de largo`, `luz 12 m`, `altura de 3,5 m`, en m, cm o mm),
**acero** (S235, S275, S355), **acabado** (galvanizado, pintado/antracita, oxirón/forja, blanco, verde, corten),
**cubierta** (sándwich, chapa, policarbonato, teja, lamas, sin cubierta), **suelo** (tramex, lagrimada, tablero,
colaborante), **uso** (vivienda, oficinas, almacén, público, comercial, industrial), **altitud** (nieve),
**zona de viento** (A, B, C), **pendiente** (% o grados), **separación** de pórticos (`pórticos cada 6 m`),
**desnivel** de una barandilla y `barandilla a los dos lados`. Lo que no se dice se toma por defecto y la
*Recepción* lo anota como supuesto. En *Ajustar datos* se puede fijar cualquier dato a mano.

## Cómo calcula (todo en código, nada de memoria del modelo)

El cálculo no depende del modelo de lenguaje: es Python determinista en `nexus/office/herrero_*.py`, así que da el
mismo resultado en el ordenador y en el móvil, con un modelo pequeño o sin modelo.

- **Perfiles**: IPE, HEA y HEB calculados con sus dimensiones nominales y los radios de acuerdo; tubos cuadrados,
  rectangulares y redondos EN 10219 con sus esquinas; UPN de tabla; redondos, cuadradillos, pletinas y angulares.
  Coinciden con las tablas de los fabricantes (IPE 200: 28,5 cm², Iy 1943 cm⁴, Wpl 221 cm³).
- **Acciones** (CTE DB-SE-AE): peso propio de cubierta o suelo, sobrecarga de uso (tabla 3.1), nieve aproximada por
  altitud (envolvente de la tabla E.2; si conoces la zona de clima, dásela con `nieve 0,6 kN`), viento con la
  presión dinámica de la zona y la altura, y fuerza horizontal en barandillas (3.2).
- **Combinaciones**: ELU 1,35 G + 1,5 Q principal + 1,5 ψ0 Q acompañantes; la sobrecarga de mantenimiento no se suma
  a la nieve. ELS con las cargas características.
- **Comprobaciones** (CTE DB-SE-A / EN 1993-1-1, γM0 = γM1 = 1,05): flexión plástica, cortante, flecha (L/300 en
  general, L/250 en cubiertas, L/350 en pasarelas), pandeo por flexión con las curvas europeas a, b y c,
  interacción axil-flector y esbeltez. Elige el **perfil más ligero** de las series adecuadas que cumple todas con
  un margen del 5 %.
- **Pilares**: porches y pasarelas en ménsula (Lcr = 2h) con la imperfección de 1/200 y el viento; naves con
  pórtico traslacional (Lcr,y = 2h); entreplantas biarticuladas con arriostramiento.
- **Placas de anclaje**: tamaño por el perfil, espesor por la presión en el hormigón HA-25 y por la tracción de los
  pernos (8.8, M16 a M24).
- **Soldaduras** (EN 1993-1-8): garganta mínima 3 mm (más con espesores gruesos) y máxima 0,7 t; la recomendada
  iguala la resistencia de la chapa más fina con cordón doble. Proceso 135 (MAG), hilo G3Si1 y gas M21.
- **EN 1090**: clase de ejecución recomendada (EXC1 o EXC2) y lo que exige (soldadores ISO 9606-1, WPS/WPQR, END).

## La oficina por puestos

| Puesto | Qué hace |
|---|---|
| Recepción | Entiende el encargo y anota los supuestos |
| Calculista | Cargas, perfiles y comprobaciones |
| Despiece | Piezas con marca, chapas, lista de corte y kilos |
| Delineante | Plano de montaje, isométrica y PDF |
| Soldador | Uniones, gargantas, símbolos ISO 2553, metros de cordón y requisitos EN 1090 |
| Montaje | Secuencia de montaje en obra |
| Fotografía | Imagen de la estructura |

El agente **Herrero** (lista de agentes y chat) usa esta oficina con la herramienta `herrero_proyecto` y explica el
resultado; en la pestaña, *Preguntar a la oficina* abre una conversación con él sobre el proyecto abierto. Las
oficinas de empresa *Estructuras metálicas* y *Herrería y forja* añaden además al **Calculista de estructuras** y al
**Delineante de estructuras metálicas**, que usan las mismas herramientas. El soldador de la oficina usa
`herrero_soldadura`.

## La imagen

- **Sin modelo de imagen** (por defecto, también en el móvil): un render en perspectiva de la geometría calculada,
  con cielo, césped y arboleda al fondo, losa de hormigón, luz del sol con sombras arrojadas, brillo según el acabado
  (el galvanizado brilla más que el oxirón) y una persona de 1,75 m como escala.
- **Con modelo de imagen** (Modelos → imagen, p. ej. SDXS o SD-Turbo con stable-diffusion.cpp) y la casilla *Imagen
  fotográfica con IA* marcada: además, una versión fotográfica que parte del render (imagen a imagen, fuerza 0,42),
  así la foto respeta las medidas y la forma de la estructura calculada.

## Ficheros

Cada proyecto se guarda en `<datos>/outputs/herrero/<id>/`: `proyecto.json` (todo el resultado), `proyecto.pdf`,
`plano-montaje.svg`, `isometrica.svg`, `despiece.csv` (separado por `;` para Excel o LibreOffice), `render.jpg` y,
con IA, `foto-ia.png`. La lista de *Proyectos guardados* los vuelve a abrir; borrarlos pide modo administrador.

## Herramientas y API

| Herramienta | Uso |
|---|---|
| `herrero_proyecto` | Encargo (y datos opcionales) → resumen, despiece, soldaduras y enlaces a los ficheros |
| `herrero_perfil` | Propiedades de un perfil y, con luz y carga o axil, su comprobación como viga o pilar |
| `herrero_soldadura` | Garganta mínima, máxima y recomendada, resistencia por cm, longitud para una fuerza y símbolo |

Rutas: ver [API](API.md#oficina-herrero-43).

## Límites

- Estructuras sencillas y regulares de las siete tipologías; no hace celosías, pórticos con puente grúa, cargas
  puntuales de maquinaria ni cálculo sísmico.
- Uniones atornilladas y nudos rígidos de nave con su descripción, sin cálculo de la tornillería del nudo.
- La nieve por altitud es una envolvente; el viento usa un coeficiente de exposición de entorno rural llano.
- No da precios: el peso y las barras sirven para pedir presupuesto al almacén.
