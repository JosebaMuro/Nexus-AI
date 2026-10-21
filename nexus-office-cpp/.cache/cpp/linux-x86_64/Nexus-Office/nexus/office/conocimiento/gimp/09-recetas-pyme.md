# Manual de GIMP 9. Recetas para el día a día de una pyme

Cada receta indica los pasos en GIMP con ventana y cómo lo hace un agente con `imagen_editar`.

## Foto de carnet (DNI y pasaporte españoles)
- Requisitos: 32 mm de alto por 26 mm de ancho, en color, reciente, de frente, cabeza descubierta, sin gafas de sol,
  fondo blanco o claro liso y uniforme, buena iluminación sin sombras. La cara ocupa la mayor parte de la foto.
- En GIMP: Recortar (Mayús+C) con relación de aspecto fija 26:32 dejando el pelo completo y algo de aire encima;
  Imagen > Escalar imagen a 307 × 378 px; Imagen > Tamaño de impresión a 300 ppp; si el fondo no es blanco,
  seleccionar el fondo y rellenar de blanco o aclararlo con Niveles; exportar JPEG calidad 95.
- Hoja para imprimir en 10 × 15 cm (1181 × 1772 px a 300 ppp): Imagen nueva de ese tamaño, abrir la foto como capa y
  duplicarla en cuadrícula de 2 × 3 o 3 × 3 con guías de corte.
- Agente: receta `foto_carnet` (encuadre centrado; si la cara no queda centrada, antes un `recortar` a mano).

## Producto para tienda online o catálogo
1. Balance de blancos (cuentagotas gris de Niveles sobre el fondo) y Niveles.
2. Recortar el producto y ponerlo sobre blanco puro #ffffff (fondo seleccionado y rellenado, o máscara de capa).
3. Lienzo cuadrado 2000 × 2000 con el producto ocupando el 85-90 % y centrado.
4. Enfocar suave y exportar JPEG 90 o WebP 85. Mismo encuadre y luz en todo el catálogo.
- Agente: receta `producto_fondo_blanco` (si el fondo de la foto no es blanco, primero quitarlo en GIMP).
- Amazon y otros marketplaces piden fondo blanco puro y el producto sin textos ni marcas de agua en la foto principal.

## Publicación para redes con texto y logo
1. Encuadrar a 1080 × 1350 (4:5) o 1080 × 1080.
2. Corrección rápida: balance de blancos, niveles, saturación +10.
3. Texto corto (5-7 palabras) en la zona más limpia de la foto; logo en una esquina al 15 %.
- Agente: `encuadrar` + `mejora_rapida`-like + `texto` + `superponer` con `imagenes/logo.png`.

## Carta o menú del restaurante con fotos de los platos
- Fotos con luz natural lateral, desde arriba (90°) para platos planos o a 45° para hamburguesas y postres.
- Corregir el balance de blancos (luz de cocina amarilla), subir algo la saturación y el contraste, recortar 1:1.
- Exportar en dos tamaños: 1080 px para redes y 2480 px de ancho (A4 a 300 ppp) para la carta impresa.

## Inmobiliaria: fotos de vivienda
- Enderezar verticales (Perspectiva, o Medir + Enderezar): las paredes torcidas hacen el piso más pequeño.
- Sombras-Luces para ver por la ventana y el interior a la vez; balance de blancos neutro.
- No quitar defectos reales de la vivienda (humedades, grietas): sería publicidad engañosa.
- Pixelar matrículas, caras y datos personales que salgan en las fotos (Filtros > Desenfoques > Pixelar).

## Antes y después (reformas, peluquería, estética, limpieza)
- Mismo encuadre y misma luz en las dos fotos. Lienzo del doble de ancho, una foto a cada lado con Abrir como capas y
  Alinear, texto «Antes» y «Después». Consentimiento de la persona si sale en la foto.

## Documento escaneado o foto de un papel
- Perspectiva para enderezar el papel, Recortar, Colores > Desaturar, Niveles llevando el blanco hasta que el fondo
  sea blanco puro, Umbral (Colores > Umbral) para texto en blanco y negro puro. Exportar PDF o PNG.
- Firma escaneada: Color a alfa sobre el blanco, guardar PNG con transparencia.

## Logo del cliente en varios formatos
- Desde el original de mayor calidad (mejor SVG). Exportar PNG transparente a 512, 1024 y 2048 px de ancho, versión en
  blanco (Colores > Invertir sobre el logo negro, o Colores > Curvas) para fondos oscuros, y favicon 512 × 512.

## Avisos legales que el agente debe recordar
- Propiedad intelectual: retocar una foto ajena no da derecho a usarla; hace falta licencia del autor.
- Derecho a la propia imagen (LO 1/1982) y RGPD: consentimiento para publicar a personas; en menores, de padres o
  tutores. Quitar el GPS del EXIF al publicar.
- No manipular documentos oficiales, sellos ni firmas de terceros.
- Publicidad: los retoques no pueden engañar sobre el producto o servicio (Ley 34/1988 General de Publicidad).
