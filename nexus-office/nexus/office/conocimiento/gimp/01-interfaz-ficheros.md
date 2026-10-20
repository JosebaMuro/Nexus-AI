# Manual de GIMP 1. Interfaz, ficheros y formatos

GIMP (GNU Image Manipulation Program) es un editor de imágenes libre (licencia GPL v3) para Windows, macOS y Linux.
Nexus-Office trae la versión 3 incluida en sus instaladores de escritorio y la usa sin ventana con la herramienta
`imagen_editar`. Este manual sirve para explicar a una persona cómo hacer cada cosa en GIMP con ventana, y para que
el agente sepa qué operación o script usar.

## Ventanas y paneles
- Modo de ventana única: menú Ventanas > Modo de ventana única. Es el modo por defecto desde GIMP 2.10.
- Caja de herramientas a la izquierda; Opciones de herramienta debajo: cada herramienta tiene sus opciones y casi
  todo el ajuste fino de una herramienta está ahí, no en un menú.
- Paneles empotrables a la derecha: Capas, Canales, Rutas, Historial de deshacer, Pinceles, Fuentes. Se abren desde
  Ventanas > Diálogos empotrables. La tecla Tab oculta y muestra todos los paneles.
- Si se pierde un panel: Ventanas > Diálogos empotrables, o Editar > Preferencias > Interfaz y restablecer la
  disposición guardada de las ventanas.
- Ver > Ampliación: 1 muestra al 100 %, Mayús+Ctrl+J ajusta la imagen a la ventana. Ctrl+rueda del ratón amplía.
- Barra de estado: unidad de medida (px, mm, cm, pulgadas) y nivel de ampliación.

## XCF frente a exportar
- XCF es el formato propio de GIMP: guarda capas, máscaras, rutas, texto editable, guías y (en GIMP 3) los filtros
  no destructivos. Se guarda con Archivo > Guardar (Ctrl+S) o Guardar como (Mayús+Ctrl+S).
- Todo lo demás (JPEG, PNG, WebP, TIFF, PDF, PSD...) se exporta: Archivo > Exportar como (Mayús+Ctrl+E) elige nombre
  y formato por la extensión; Archivo > Exportar a (Ctrl+E) repite la última exportación; Archivo > Sobrescribir
  aparece cuando la imagen se abrió de un JPEG o PNG.
- Buena práctica profesional: guardar siempre el XCF de trabajo y exportar las copias finales. El original del
  cliente no se sobrescribe nunca.
- Abrir como capas (Ctrl+Alt+O) añade una imagen a la actual como capa nueva, por ejemplo un logotipo.

## Qué formato elegir
| Uso | Formato | Notas |
|---|---|---|
| Fotografía para web, correo, redes | JPEG calidad 80-90 | Sin transparencia. Cada guardado pierde calidad: editar siempre desde el original o el XCF |
| Logotipos, capturas, gráficos con texto, transparencia | PNG | Sin pérdida; pesa más en fotos |
| Web moderna | WebP calidad 75-85 | Pesa un 25-35 % menos que JPEG; admite transparencia |
| Imprenta, archivo de calidad | TIFF (LZW) o PDF | 300 ppp a tamaño final; preguntar a la imprenta si quiere CMYK |
| Intercambio con Photoshop | PSD | GIMP abre y exporta PSD con capas; efectos de capa de Photoshop no se conservan |
| Proyecto editable | XCF | Solo lo abre GIMP |

- JPEG: en el diálogo de exportar, Calidad 85-92 para entrega, 95 para archivo; Opciones avanzadas: submuestreo
  4:4:4 (mejor color en textos y logos), progresivo para web, «Guardar datos EXIF» si se quieren conservar.
- PNG: nivel de compresión 9 no pierde calidad, solo tarda más.
- Metadatos: al exportar se puede quitar EXIF y GPS (protección de datos: una foto de un móvil puede llevar la
  ubicación de la casa del cliente).

## Deshacer e historial
- Ctrl+Z deshace, Ctrl+Y rehace. Editar > Historial de deshacer muestra todos los pasos y permite volver a uno.
- El historial se pierde al cerrar: por eso conviene guardar el XCF.

## Precisión y modo de imagen
- Imagen > Modo: RGB (color), Escala de grises, Indexado (paleta, para GIF y algunos PNG pequeños).
- Imagen > Precisión: 8 bits enteros es lo normal; 16 o 32 bits para retoque fuerte de fotos RAW (menos bandas en
  degradados). Exportar a JPEG siempre baja a 8 bits.
- RAW de cámara: GIMP abre los RAW a través de darktable o RawTherapee si están instalados.

## Novedades de GIMP 3 que conviene conocer
- Filtros no destructivos: los filtros GEGL (Filtros > ...) quedan como efectos editables en la capa (icono fx en el
  panel Capas) hasta que se combinan. Se pueden reordenar, ocultar y volver a editar.
- Selección de varias capas a la vez (Mayús o Ctrl + clic en el panel Capas) para moverlas, agruparlas o alinearlas.
- Capas que crecen solas al pintar fuera de su borde (opción de la herramienta de pintura «Ampliar capas»).
- Gestión de color mejorada y prueba en pantalla de CMYK (Ver > Prueba de color) con un perfil de imprenta.
- Plugins en Python 3 y Script-Fu nuevo; la automatización está en el capítulo 8.
- Atajo nuevo para buscar cualquier orden: la barra / (Ayuda > Buscar y ejecutar una orden).
