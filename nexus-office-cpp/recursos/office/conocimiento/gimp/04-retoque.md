# Manual de GIMP 4. Retoque fotográfico

## Herramientas de retoque
| Herramienta | Tecla | Uso |
|---|---|---|
| Clonar | C | Copia píxeles de un origen. Ctrl+clic marca el origen y luego se pinta. Para quitar objetos con bordes nítidos |
| Saneado (sanear) | H | Como Clonar pero funde textura y luz con el entorno. Granos, manchas, arrugas, polvo del sensor |
| Clonar en perspectiva | — | Clona respetando una perspectiva (suelos, fachadas) |
| Difuminar | S | Arrastra el color como un dedo |
| Desenfocar/Enfocar | Mayús+U | Local, a pincel |
| Blanquear/Ennegrecer (esquivar/quemar) | Mayús+D | Aclarar u oscurecer zonas |
| Borrador | Mayús+E | Borra a transparente si la capa tiene canal alfa |

- Trabajar siempre en una capa duplicada (Mayús+Ctrl+D) o en una capa vacía encima con la opción «Muestreo combinado»
  activada en Clonar o Saneado: el original queda intacto.
- Pincel con dureza baja y tamaño algo mayor que el defecto. Tamaño con [ y ].
- Mayús+clic con cualquier herramienta de pintura traza una línea recta desde el último punto (cables, bordes).

## Quitar objetos
1. Objetos pequeños sobre fondo uniforme: Saneado (H) a pinceladas cortas.
2. Objetos sobre fondo con líneas (baldosas, ladrillos): Clonar (C) alineando el origen con las líneas, o Clonar en
   perspectiva.
3. Objetos grandes: seleccionar un trozo limpio del fondo, copiar, pegar como capa, moverlo encima, máscara de capa
   para fundir los bordes.
4. El plugin Resynthesizer («Heal selection», relleno según contenido) no viene con GIMP; G'MIC trae filtros de
   relleno («Inpaint»). Nexus-Office no los instala.

## Piel (retrato) de forma natural
- Separación de frecuencias: duplicar la capa dos veces; a la de abajo Desenfoque gaussiano (radio 4-8 px en una foto
  de 24 Mpx); a la de arriba modo «Extraer granulado» con la de abajo visible, Capa nueva desde lo visible y poner esa
  capa en «Combinar granulado». La de alta frecuencia lleva la textura (poros) y la de baja el color: se sanea la
  textura y se suaviza el color por separado. Alternativa integrada: Filtros > Realzar > Descomposición en ondículas.
- Esquivar y quemar no destructivo: capa nueva rellena de gris 50 % en modo Luz suave; pintar blanco o negro con
  opacidad 5-10 %.
- Ojos rojos: seleccionar los ojos y Filtros > Realzar > Eliminar ojos rojos.
- Ética y ley: no cambiar rasgos de forma engañosa en publicidad (Ley General de Publicidad) y respetar lo pactado
  con la persona retratada.

## Nitidez y ruido
- Filtros > Realzar > Enfocar (máscara de desenfoque): Radio 0,5-2 px para web, Cantidad 0,3-0,8, Umbral 0-5 para no
  enfocar el ruido. Siempre al final y a tamaño de salida. Con Ver al 100 % para juzgarlo.
- Para imprimir se enfoca algo más de lo que parece bien en pantalla.
- Filtros > Realzar > Reducción de ruido (GEGL) para fotos de interior con ISO alto; Filtros > Realzar > Quitar
  manchas (despeckle) para escaneos con polvo.
- Desenfoque gaussiano (Filtros > Desenfoques) para fondos y efectos; Desenfoque de lente para simular poca
  profundidad de campo sobre una selección del fondo.

## Viñeta, sombras y efectos de luz
- Filtros > Luz y sombra > Viñeta oscurece los bordes y dirige la mirada al centro. Moderación: 20-40 %.
- Filtros > Luz y sombra > Sombra arrojada para productos recortados o textos sobre fotos.
- Filtros > Luz y sombra > Destello de lente, Supernova: con prudencia.
