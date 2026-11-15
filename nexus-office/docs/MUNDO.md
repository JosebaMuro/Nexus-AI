# Mundo: información del mundo para los agentes

El servicio **Mundo** (`nexus/mundo/`, desde la 4.2) recoge cada pocos minutos datos de fuentes públicas, los ordena
por **categoría** y **país** y los guarda en la base de datos local para que los agentes los consulten. Se inspira en
los paneles de «vista de pájaro» del mundo en tiempo real, pero sin mapa 3D: lo que importa aquí es que los agentes
tengan la información ordenada y al día.

Solo **lee** de servicios públicos: no envía nada de tu equipo. Las peticiones salen con un agente de usuario
genérico.

## En la interfaz

La vista **MUNDO** muestra:

- Cuántos registros hay por categoría y país (matriz), con filtros por categoría, país, texto y orden (recientes o
  más importantes primero).
- La tabla de fuentes: activa o no, intervalo, último dato, número de registros y último error. Cada fuente tiene
  **AHORA** para pedirla al momento.
- Los ajustes (modo administrador): activar el servicio, dar contexto a los agentes, el **factor** de los intervalos
  (2 = la mitad de peticiones, 0,5 = el doble), horas que se guardan los datos, **países de interés** (por nombre o
  código: «España, Francia, MX») y las claves gratuitas de NASA FIRMS y AISStream.

## Fuentes

| Fuente (`id`) | Categoría | Origen | Cada | Mínimo | Clave |
|---|---|---|---|---|---|
| `sismos` | sucesos | USGS (magnitud ≥ `mundo.sismos_magnitud_min`, 2,5) | 5 min | 1 min | |
| `eventos_naturales` | sucesos | NASA EONET | 30 min | 10 min | |
| `desastres` | sucesos | GDACS (ONU y Comisión Europea) | 15 min | 5 min | |
| `perimetros_incendio` | sucesos | WFIGS / NIFC (EE. UU., apagada por defecto) | 1 h | 15 min | |
| `focos_fuego` | sucesos | NASA FIRMS (VIIRS, 24 h, por país) | 1 h | 15 min | `firms` |
| `vuelos` | movimiento | OpenSky Network (por país y códigos 7500/7600/7700) | 30 min | 15 min | |
| `vuelos_militares` | movimiento | adsb.lol | 10 min | 2 min | |
| `buques` | movimiento | AISStream (muestra de `mundo.buques_muestra_s` segundos) | 30 min | 10 min | `aisstream` |
| `bicis` | movimiento | MobilityData GBFS (y los sistemas de `mundo.gbfs_sistemas`) | 1 día | 1 h | |
| `ciclones` | clima | NOAA NHC / CPHC | 30 min | 10 min | |
| `tiempo` | clima | Open-Meteo (capitales de tus países y `mundo.lugares`) | 30 min | 10 min | |
| `satelites` | espacio | CelesTrak (grupos de `mundo.satelites_grupos`) | 12 h | 2 h | |
| `lanzamientos` | espacio | Launch Library 2 | 1 h | 15 min | |
| `conflictos` | noticias | GDELT | 15 min | 5 min | |
| `noticias_pais` | noticias | GDELT (medios de cada país de interés) | 30 min | 10 min | |
| `camaras` | cámaras | TfL y Fintraffic (apagada por defecto) | 6 h | 1 h | |
| `radio` | radio | Radio Browser | 1 día | 1 h | |
| `fauna` | naturaleza | iNaturalist | 1 h | 10 min | |
| `indices` | mercados | Yahoo Finance, Stooq y otros (`mundo.mercados_simbolos`) | 5 min | 1 min | |
| `cripto` | mercados | CoinGecko y Binance | 5 min | 1 min | |
| `macro` | mercados | FRED (curva del Tesoro y VIX) | 6 h | 1 h | |
| `calendario_economico` | mercados | Forex Factory | 1 h | 15 min | |
| `noticias_mercado` | mercados | Yahoo Finance y Google Noticias | 15 min | 5 min | |
| `sectores` | mercados | TradingView | 15 min | 5 min | |
| `resultados` | mercados | Nasdaq | 6 h | 1 h | |
| `alertas_bolsa` | mercados | tus alertas de bolsa (`bolsa_alerta`) | 5 min | 1 min | |

Una fuente sin clave no se pide hasta que la pones. Si falla, espera cada vez más antes de volver a intentarlo (su
intervalo, o 2, 4, 8... minutos si es más, como mucho una hora), y el error se ve en la tabla. Las fuentes «acumulativas» (noticias, alertas) guardan
cada titular nuevo; las demás sustituyen su foto anterior.

## Para los agentes

- `mundo_resumen` (`pais`, `categoria`): cuántos registros hay y lo más destacado de cada categoría.
- `mundo_consultar` (`categoria`, `pais`, `texto`, `fuente`, `horas`, `orden`, `limite`): los registros, cada uno con
  fecha, país, valor (importancia), título, detalle y enlace.
- `mundo_fuentes` (`actualizar`): el estado de las fuentes; con `actualizar` pide una ya.
- **Contexto automático** (`mundo.contexto`, activado): si la pregunta nombra un país o un tema del mundo (terremoto,
  huracán, vuelos, bolsa...), el agente recibe unas pocas líneas con lo más reciente sobre ello (como mucho
  `mundo.max_por_fuente` caracteres). No se añade nada a las peticiones de otros nodos de la red.
- Sin modelo cargado, «¿qué está pasando en el mundo?» responde con `mundo_resumen`.

Las tienen el coordinador (todas), el investigador, el profesor y los agentes de la oficina de bolsa.

## Configuración (`mundo.*`)

| Clave | Por defecto | Para qué |
|---|---|---|
| `activo` | `true` | Arranca el servicio con Agente IA |
| `factor` | `1.0` | Multiplica todos los intervalos (entre 0,1 y 20; nunca baja del mínimo de cada fuente) |
| `fuentes` | `{}` | Por fuente: `{"activa": bool, "intervalo_s": n}` |
| `paises` | `[]` | Países de interés (códigos ISO; hasta 25) |
| `claves` | `{}` | `firms` y `aisstream` (se guardan como secretos y no se muestran) |
| `retencion_h` | `48` | Horas que se guardan los registros |
| `contexto` | `true` | Contexto automático para los agentes |
| `max_por_fuente` | `1500` | Tamaño del contexto |
| `sismos_magnitud_min`, `gbfs_sistemas`, `lugares`, `satelites_grupos`, `buques_muestra_s`, `mercados_simbolos`, `mercados_noticias` | | Ajustes de cada fuente |

Con la variable `NEXUS_SIN_MUNDO=1` el servicio no arranca (lo usan las pruebas).

## Terminal y API

```
python -m nexus mundo estado              # fuentes, intervalos, último dato y errores
python -m nexus mundo resumen --pais ES   # lo destacado (también --categoria)
python -m nexus mundo probar --todas      # pide cada fuente en vivo y dice si responde
python -m nexus mundo probar --solo sismos --json
```

Rutas en [API.md](API.md#mundo-terminal-de-bolsa-y-terminal-ia-42). Cada actualización emite el evento `mundo` por el
WebSocket para que la vista se refresque.

## Código

- `fuentes.py`: cada fuente es una `Fuente(id, nombre, categoria, obtener, intervalo_s, minimo_s, origen,
  descripcion, clave, peso, acumula, activa)`; `obtener(ctx)` devuelve registros `{uid, pais, ts, valor, titulo,
  detalle, url, lat, lon, datos}`. Para añadir una fuente basta con escribir su lector y añadirla a `FUENTES`.
- `servicio.py`: `Mundo` (planificador en un hilo, reintentos, tablas `registros` y `estado` en su propia base
  SQLite dentro de `data/mundo/`, `contexto_para(texto)`).
- `paises.py` + `paises.json`: 250 países y territorios con nombres en español e inglés, capital y un rectángulo para
  situar un punto (`packaging/build_paises.py` lo regenera).
