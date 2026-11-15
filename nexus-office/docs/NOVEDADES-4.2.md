# Novedades de la 4.2

La 4.2 junta en Agente IA las ideas de varios proyectos abiertos (un panel del mundo en tiempo real, terminales
financieros y terminales de IA para programar), las escribe en Python dentro de la app y las deja al alcance de
todos los agentes. Además trae lo que aún faltaba de la edición C++ 3.x: noticias de la mesa, bróker en papel, editor
de vídeo y GPS del móvil. También acelera el arranque en Windows y Android, quita del repositorio la edición C++ de
referencia y trae un script para hacer una Ubuntu mínima con Agente IA ya instalado como servicio.

Todo lo nuevo funciona sin internet salvo lo que lee fuentes públicas (el mundo, los datos de bolsa reales y las
noticias) y el bróker de Alpaca. Nada de lo que pasa en el equipo se envía fuera: las fuentes solo se consultan, y al
bróker solo llegan las órdenes que confirmas escribiendo CONFIRMO.

## Mundo: información del mundo por categorías y países

Una vista nueva, **MUNDO**, y tres herramientas para los agentes. Agente IA pide cada pocos minutos datos a 26
fuentes públicas y los ordena por **categoría** y **país** (con su código ISO, de 250 países y territorios):

| Categoría | Fuentes |
|---|---|
| Sucesos | terremotos (USGS), incendios, volcanes y tormentas (NASA EONET), alertas de desastres (GDACS), perímetros de incendios de EE. UU. (NIFC), focos de fuego por satélite (NASA FIRMS, con clave gratuita) |
| Movimiento | aviones en vuelo por país y códigos de emergencia (OpenSky), aeronaves militares con transpondedor (adsb.lol), barcos por país (AISStream, con clave gratuita), bicicleta y patinete compartidos (GBFS) |
| Clima | ciclones tropicales (NOAA), tiempo y calidad del aire de tus ciudades (Open-Meteo) |
| Espacio | satélites en órbita (CelesTrak), próximos lanzamientos (Launch Library 2) |
| Noticias | conflictos y protestas, noticias de tus países (GDELT) |
| Cámaras, radio y naturaleza | cámaras públicas de tráfico (Londres y Finlandia), emisoras de radio (Radio Browser), avistamientos de fauna y flora (iNaturalist) |
| Mercados | índices, materias primas y divisas, criptomonedas, tipos de interés y VIX (FRED), calendario económico, noticias de mercado, sectores, resultados del día y tus alertas de bolsa |

- **Intervalos regulables**: cada fuente tiene su intervalo por defecto y un mínimo para no abusar del servicio
  público. En la vista Mundo cambias el de cada una, la activas o la paras, y un **factor** global multiplica todos
  (2 = la mitad de peticiones). Si una fuente falla, espera cada vez más antes de volver a pedirla.
- **Países de interés**: con ellos llegan noticias de esos países, el tiempo de sus capitales y se priorizan en las
  respuestas. Se escriben por nombre o código («Francia, MX»).
- **Para los agentes**: `mundo_resumen` (lo más destacado por categoría y país), `mundo_consultar` (filtra por
  categoría, país, texto o fuente) y `mundo_fuentes` (estado de cada fuente). Además, cuando preguntas algo que
  nombra un país o un tema del mundo, el agente recibe solo un resumen breve de lo que hay sobre ello
  (`mundo.contexto`). «¿Qué está pasando en el mundo?» funciona incluso sin modelo cargado.
- Desde la terminal: `python -m nexus mundo resumen`, `mundo estado` y `mundo probar --todas` (prueba cada fuente en
  vivo y dice cuál responde).

Detalle técnico en [MUNDO.md](MUNDO.md).

## Bolsa: el agente de bolsa con funciones de terminal financiero

El agente de bolsa ya existente (y los agentes de la oficina de bolsa) completan sus capacidades con lo que hace un
terminal financiero. Todo devuelve cifras calculadas, con su fuente, y un aviso de que no es asesoramiento:

- **Panel por pregunta** (`mercado_panel`): «¿a cuánto cotiza Iberdrola y qué noticias hay?» elige y ejecuta las
  recetas que hacen falta (cotización, gráfico, indicadores, fundamentales, noticias, opciones, resultados,
  comparativa, criptomonedas, cribas). `mercado_consulta` da cada dato suelto.
- **Verificación de cifras** (`bolsa_verificar`): antes de responder, el agente comprueba que cada precio o porcentaje
  sale de los datos consultados; lo que no tiene respaldo se quita o se marca.
- **Opciones** (`bolsa_opciones`): Black-Scholes con griegas, volatilidad implícita y 11 estrategias (put protectora,
  collar, call cubierta, diferenciales, cono, cuna, mariposa, cóndor de hierro...) con su curva de resultado,
  puntos de equilibrio y probabilidad de beneficio.
- **Riesgo** (`bolsa_riesgo`): VaR histórico y paramétrico, CVaR, volatilidad, caída máxima, beta y correlaciones;
  escenarios de estrés (2008, covid, subida de tipos de 2022, puntocom, estanflación); tamaño de posición por riesgo máximo con stop y Kelly fraccionado.
- **Valoración** (`bolsa_valoracion`): descuento de flujos (DCF) y DCF inverso, que dice qué crecimiento descuenta
  ya el precio.
- **Cribas** (`bolsa_criba`): Graham, Lynch, Greenblatt, Buffett, dividendo, impulso y pequeñas de valor, o filtros
  y fórmulas propias («per < 15 y rentabilidad_dividendo_pct > 3,5»).
- **Informes oficiales** (`bolsa_informe_oficial`): lista y lee los 10-K, 10-Q y 8-K de la SEC, con los impulsores y
  frenos del negocio en citas literales comprobadas, lo nuevo frente al informe anterior y las cifras XBRL.
- **Alertas** (`bolsa_alerta`) con varias condiciones (precio, RSI, medias, distancia al máximo); las que se cumplen
  llegan como sucesos del Mundo. **Expedientes** (`bolsa_expediente`) con la tesis, supuestos, evidencias con su
  fuente, riesgos y revisiones de cada valor. **Validación por tramos** (`bolsa_validar`) de una estrategia.
- **Agente autónomo más prudente**: no abre posiciones si el VaR de un día de su cartera simulada supera
  `var_max_pct` (3 %) ni compra un valor que se mueve casi igual (correlación > 0,85) que uno que ya tiene.
- En la vista BOLSA, el panel **TERMINAL DE BOLSA** reúne todo con formularios y un escritorio con tus valores.

Detalle en [BOLSA.md](BOLSA.md#terminal-financiero).

## Terminal IA

Una vista nueva, **TERMINAL IA** (solo administrador), y un agente **terminal** que trabaja en la consola del equipo
como un programador: mira antes de cambiar, lleva una lista de tareas, edita con cambios exactos que se pueden
deshacer y comprueba lo que hace.

- **Consola con sesión**: bash (Linux, macOS, Android) o PowerShell (Windows); recuerda la carpeta y las variables
  entre órdenes, corta lo que tarda demasiado y guarda las salidas largas.
- **Intérprete** de Python que conserva variables entre llamadas (y JavaScript con Node si está instalado).
- **Ficheros**: explorador, editor, búsqueda como grep y **deshacer** de cada cambio.
- **Procesos de fondo**: servidores o tareas largas que siguen corriendo; se lee su salida y se escribe en su entrada.
- **Modos plan y construir**: en plan el agente solo mira y propone; en construir, cambia.
- Lee las instrucciones del proyecto en `AGENTS.md`, habilidades en `.agents/skills/` y órdenes propias en
  `.agents/commands/`.
- **Seguridad**: ejecutar órdenes y código es de riesgo alto y lo aprueba el administrador (como el resto de
  herramientas, sin sandbox); desde otros equipos solo con una **ficha** que crea el administrador.
- En la terminal del sistema: `python -m nexus terminal` (interactivo, con `!orden`, `/py`, `/modo`, `/deshacer`) o
  `python -m nexus terminal -p "pregunta"` para una sola orden.

El programador también usa ahora las herramientas `terminal_*` y puede delegar en el agente terminal.
Detalle en [TERMINAL.md](TERMINAL.md).

## Lo que faltaba de la edición C++ 3.x

### Noticias de la mesa de analistas

- Botón **NOTICIAS** en el laboratorio de bolsa: titulares recientes del valor de Yahoo Finance (por símbolo) y de
  Google Noticias (por nombre de la empresa), cada uno con su sentimiento según el léxico financiero del laboratorio.
- La **mesa de analistas** usa esos titulares de los últimos 14 días (se puede desactivar con `"noticias": false`).
- Se guardan en `data/bolsa/noticias.json`: sin conexión la mesa sigue con lo guardado. Se consultan como mucho cada
  30 minutos y se olvidan a los 120 días.
- API: `GET /api/bolsa/noticias/<símbolo>?refrescar=true`.

### Bróker: de la mesa simulada a órdenes de verdad

Panel **Bróker** en la sección Bolsa (solo administrador). Reglas que no cambian:

- El agente autónomo opera siempre en su cartera simulada; nunca envía órdenes al bróker por su cuenta.
- Ninguna orden sale sin que una persona escriba **CONFIRMO**.
- Tipo **manual** (por defecto): no conecta con nada; la orden queda apuntada para ejecutarla en tu banco o bróker.
- Tipo **alpaca**: empieza en el entorno de **papel** de Alpaca (dinero ficticio con precios reales). El entorno real
  solo se usa con `broker.entorno = real` **y** `broker.permitir_real = true`.
- La clave y el secreto de la API se guardan enmascarados como el resto de secretos.

| Método y ruta | Uso |
|---|---|
| `GET /api/bolsa/broker?consultar=true` | Estado y, con Alpaca, la cuenta |
| `GET /api/bolsa/broker/ordenes?sincronizar=true` | Órdenes apuntadas y, con Alpaca, las del bróker y las posiciones |
| `POST /api/bolsa/broker/orden` | `{"orden": {simbolo, lado, cantidad, tipo, limite, validez}, "confirmacion": "CONFIRMO"}` |
| `DELETE /api/bolsa/broker/orden/<id>` | Cancelar una orden en Alpaca |

### Editor de vídeo en el navegador

- **EDITOR DE VÍDEO** (sección Vídeo) abre `/static/video.html`: línea de tiempo con imágenes, textos, transiciones
  y música, y grabación a WebM en el propio navegador, sin ffmpeg ni conexión. El resultado se guarda en
  `outputs/videos/` con `POST /api/video/guardar`.
- Cada vídeo rápido (`crear_video`) deja junto al AVI un `.proyecto.json`: el enlace **Abrir en el editor** carga esas
  imágenes en el editor para retocarlo.

### GPS del móvil en segundo plano (APK autónoma)

- En **Ajustes > Ubicación**, la app Android muestra **Usar el GPS de este móvil**. Android pide permiso de ubicación
  y la app escribe la posición en `ubicacion.json` (fuente «app» de `ubicacion_actual`), sin internet.
- Con la app abierta funciona con el permiso normal. Con **TAMBIÉN CON LA APP CERRADA** Android pide «Permitir
  siempre» y el servicio de Agente IA sigue leyendo el GPS en segundo plano.
- Además del GPS escucha las posiciones que ya piden otras apps (proveedor pasivo), que no gastan batería extra.
- La posición no sale del móvil.

### Lo que sigue solo en la edición C++

La capa `hal/` (memoria compartida con el acelerador, anillos sin bloqueo) con los pipelines de percepción y pantalla,
y el motor propio de difusión ONNX, son piezas nativas de la C++ y no se portan: en la edición Python las imágenes se
generan con los motores de `nexus/motores/` (ver [MOTORES.md](MOTORES.md)).

## Más rápido en Windows y Android

- **Arranque**: Agente IA arranca con el hardware que detectó la vez anterior si nada ha cambiado (mismo sistema,
  procesador, memoria, versión y runtimes) y vuelve a detectarlo en segundo plano. En Windows ya no espera a la
  consulta de PowerShell y WMI; si algo cambió, avisa en Sistema y se aplica al reiniciar o con «Detectar de nuevo».
- **Windows**: el servicio y llama-server se quitan la limitación de energía (EcoQoS) y suben a prioridad «por encima
  de lo normal». Sin esto, Windows 11 manda a los núcleos de eficiencia a los procesos sin ventana y la generación
  iba bastante más lenta (`hardware.prioridad_alta`).
- **Procesadores híbridos** (Intel de 12.ª generación o posterior): llama.cpp usa un hilo por núcleo de rendimiento.
- **Interfaz comprimida**: la app cliente del móvil y el acceso remoto cargan la interfaz unas cuatro veces más
  rápido (gzip).
- El instalador de Windows precompila el código, y en Android las propiedades del sistema se leen una sola vez.
- Los motores siguen eligiéndose solos según el chip (ONNX Runtime GenAI, OpenVINO GenAI, LiteRT-LM, ExecuTorch,
  Genie...) con llama.cpp de respaldo, y van dentro de los instaladores.

## Ubuntu mínima con Agente IA preinstalado

`packaging/ubuntu/rehacer-ubuntu.sh` (también suelto en la Release) rehace la ISO oficial de Ubuntu Server para que
la instalación **mínima** lleve un proyecto Python como **servicio activo y preconfigurado**. Por defecto instala
Agente IA desde su `.deb`; sirve igual para cualquier proyecto Python con `--proyecto` y `--orden`. Puede preparar una
instalación desatendida. Detalle en [UBUNTU.md](UBUNTU.md).

## Limpieza

- La **edición C++ de referencia** sale del repositorio: su código queda en el tag `v3.2.3-cpp`, completo con los
  últimos cambios en el commit `59fa50d` de `main`, y como zip en esta Release
  (`Agente-IA-cpp-referencia-codigo-fuente.zip`).
- Fuera también sus flujos de trabajo de GitHub, un flujo de publicación antiguo y las pruebas dentro de los
  instaladores (siguen en el zip del código fuente).

## Lo que falta probar

- Las fuentes del mundo en vivo desde casa (en el entorno de desarrollo solo se probaron con datos de ejemplo; el
  flujo de trabajo de pruebas las comprueba en vivo), y FIRMS y AISStream con tus claves.
- El terminal IA en un Windows real (PowerShell) y en el móvil.
- El bróker con una cuenta de Alpaca en papel y el GPS en segundo plano en un móvil real.
- La prioridad y EcoQoS en un Windows 11 real con procesador híbrido, y GPU y NPU en hardware real.
- La ISO de Ubuntu en un equipo físico (se prueba instalándola en una máquina virtual).
