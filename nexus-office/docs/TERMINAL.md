# Terminal IA

El **Terminal IA** (desde la 4.2) une en Agente IA lo que hacen los terminales de IA para programar: una consola del
equipo con sesión, un intérprete con memoria, ficheros que se editan con cambios exactos y se pueden deshacer,
procesos de fondo y un agente, **terminal**, que trabaja como un programador. Está en `nexus/terminal/` y no depende
de ningún programa externo.

## En la interfaz

La vista **TERMINAL IA** es solo para el **administrador** (ejecuta órdenes en el equipo). Tiene:

- **Consola**: escribe una orden y verás la salida según llega. La sesión recuerda la carpeta (`cd`) y las variables
  que exportes. ↑ y ↓ recorren el historial; **CORTAR** interrumpe la orden; **FONDO** la lanza como proceso de
  fondo. El selector de lenguaje permite mandar el texto al intérprete de Python (con memoria entre llamadas), a Node
  (JavaScript, si está instalado) o abrir HTML en una pestaña.
- **Agente**: pídele un trabajo («añade una prueba para la función X y pásala»). Verás su lista de tareas y cada
  herramienta que usa. Las órdenes y el código piden tu aprobación (riesgo alto).
- **Modo**: en **plan** el agente solo lee y propone lo que haría; en **construir** ejecuta y edita.
- **Ficheros**: explorador de la carpeta de la sesión, editor, y **deshacer** del último cambio de cada fichero.
- **Procesos**: los de fondo, con su salida, entrada y botón para pararlos.
- **Ficha**: crea la ficha con la que otro programa o equipo usa la API del terminal (`Authorization: Bearer ...`).

## El agente terminal

Agente `terminal` (`nexus/agents/defaults/terminal.yaml`), con hasta 24 pasos por petición (`max_steps`). Trabaja así:

1. Mira antes de cambiar: lista y lee los ficheros que vienen al caso y busca en el proyecto.
2. Para un trabajo largo escribe una lista de tareas (`terminal_tareas`) y la va marcando.
3. Edita con cambios exactos (`terminal_editar`: un texto que aparece una sola vez se cambia por otro) o escribe
   ficheros nuevos; cada cambio guarda copia para deshacerlo.
4. Comprueba lo que hace (pruebas, compilar, ejecutar) y lo cuenta.

Recibe en su contexto la carpeta, la consola, el modo y, si existen, las instrucciones del proyecto (`AGENTS.md` en
la carpeta o en una de las cuatro de encima), las **habilidades** (`.agents/skills/<nombre>/SKILL.md` del proyecto y
`data/terminal/habilidades/`) y las **órdenes propias** (`.agents/commands/<orden>.md` y `data/terminal/ordenes/`, con
`$ARGUMENTOS` o `$ARGUMENTS`; se usan escribiendo `/orden argumentos`).

El programador puede usar también `terminal_leer`, `terminal_buscar`, `terminal_editar`, `terminal_ejecutar` y
`terminal_codigo`, y delegar en el agente terminal. Sin modelo cargado, `$ orden` o «ejecuta: orden» en el chat
ejecutan la orden directamente (con aprobación).

## Herramientas

| Herramienta | Riesgo | Qué hace |
|---|---|---|
| `terminal_ejecutar` | alto | Orden en la consola de la sesión (bash en Linux, macOS y Android; PowerShell en Windows) |
| `terminal_codigo` | alto | Código en el intérprete: Python con estado, JavaScript con Node, consola o HTML |
| `terminal_proceso` | alto | Procesos de fondo: lista, lanzar, leer salida, escribir entrada, parar, interrumpir |
| `terminal_escribir` | medio | Crea o sustituye un fichero (con copia) |
| `terminal_editar` | medio | Cambios exactos, varios a la vez (`cambios`), o `deshacer` |
| `terminal_leer` | bajo | Lee un fichero con números de línea, por tramos, o lista una carpeta |
| `terminal_buscar` | bajo | Busca como grep (expresión regular) o lista ficheros por patrón |
| `terminal_tareas` | bajo | Lista de tareas del trabajo |

Las de riesgo alto las aprueba el administrador y las de riesgo medio el usuario (política por defecto del
supervisor). No hay sandbox (como el resto de herramientas desde la 4.0.3): las órdenes corren con el usuario del
servicio. Las peticiones que llegan de otros nodos de la red cooperativa se rechazan.

## En la terminal del sistema

```
python -m nexus terminal                         # interactivo en la carpeta actual
python -m nexus terminal --cwd ~/proyecto --modo plan
python -m nexus terminal -p "¿qué hace este proyecto?" -f json
python -m nexus terminal -p "pasa las pruebas y arregla lo que falle" --si   # aprueba sin preguntar
```

Dentro: `!orden` ejecuta tal cual, `/py código` usa el intérprete, `/modo plan|construir`, `/carpeta RUTA`,
`/tareas`, `/deshacer`, `/ordenes` y `/salir`.

## Cómo funciona

- Cada orden corre en un proceso nuevo de la consola, que entra en la carpeta de la sesión, ejecuta la orden y al
  final escribe una marca única con el código de salida, la carpeta y las variables de entorno. Así la sesión
  conserva `cd` y `export` sin dejar una consola abierta que se pueda quedar colgada.
- Las órdenes que pasan de `terminal.espera_s` (120 s) se cortan con todos sus hijos; para algo largo están los
  procesos de fondo. Las salidas de más de `terminal.salida_max` caracteres (30 000) se recortan y se guardan enteras
  en `data/terminal/salidas/`.
- Las copias para deshacer van a `data/terminal/copias/`.
- El intérprete de Python es un proceso aparte que conserva variables e importaciones; si se pasa de tiempo se
  reinicia.

## Configuración (`terminal.*`)

| Clave | Por defecto | Para qué |
|---|---|---|
| `activo` | `true` | Activa el terminal |
| `carpeta` | (vacía) | Carpeta inicial; vacía = `data/sandbox` |
| `shell` | (vacía) | Consola: vacía = la del sistema (`bash`, `sh`, `powershell`, `cmd` o una ruta) |
| `espera_s` | `120` | Tiempo máximo de una orden |
| `salida_max` | `30000` | Caracteres de salida que se devuelven |
| `pasos_max` | `24` | Pasos del agente por petición |
| `modo` | `construir` | Modo inicial (`plan` o `construir`) |
| `sesiones_max` | `8` | Sesiones abiertas a la vez |
| `ficha` | (vacía) | Ficha para la API (secreto) |

Rutas de la API en [API.md](API.md#mundo-terminal-de-bolsa-y-terminal-ia-42).
