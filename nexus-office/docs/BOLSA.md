# Oficina de bolsa e inversiones y mesa simulada

El perfil `bolsa-inversiones` convierte Nexus-Office en la oficina de una pyme o un despacho que trabaja con bolsa,
fondos, ETF, renta fija y criptoactivos. Trae una **mesa de bolsa simulada** para practicar sin dinero en un entorno
que imita uno real, en Windows, macOS, Linux, Raspberry Pi y Android. Desde la 2.1 la mesa usa **cotizaciones
reales** cuando hay conexión, y sin ella sigue simulada desde el último precio real.

> **Aviso.** La mesa es **siempre simulada**: nunca usa dinero ni envía órdenes a ningún bróker. Con conexión los
> precios son reales (con el retraso de cada bolsa); sin ella, simulados. Nada de lo que dicen los agentes es
> asesoramiento financiero personalizado, que solo puede dar una entidad o un asesor registrado en la CNMV. Invertir
> conlleva el riesgo de perder dinero.

## Datos reales (2.1)
Con `bolsa.datos: "reales"` (por defecto) la mesa toma las cotizaciones y el cambio de divisa de internet:

| Fuente | Para qué | Retraso |
|---|---|---|
| **Yahoo Finance** | Acciones, ETF, cripto y divisas con los mismos símbolos del catálogo (SAN.MC, AAPL, VWCE.DE, BTC-EUR, EURUSD=X) y gráficos | ≈15 min en bolsa, tiempo real en cripto |
| **CNBC** | Respaldo de cotizaciones y **velas** de acciones cuando Yahoo limita (pasa mucho desde servidores) | ≈15 min |
| **Google Finance** y **Stooq** | Respaldo de cotizaciones | ≈15 min |
| **Coinbase** y **Kraken** | Respaldo de cotizaciones y **velas** de cripto | Tiempo real |
| **BCE** | Cambio de divisa de respaldo | Diario |

Cada fuente que falla se aparta un rato (30 s, 60 s, 120 s… hasta 15 min) y se prueba la siguiente. Sin ninguna, la
mesa sigue con la simulación desde el último precio real. Cada precio dice su fuente y su retraso.
`bolsa.datos: "simulados"` vuelve a los precios ficticios de antes.

## Agente de bolsa autónomo (2.1)
Gestiona su propia cartera **simulada** («Agente autónomo (simulada)») con esos datos reales, como lo haría una
persona en la mesa, con sus comisiones, horquilla y horarios. En cada ciclo (60 s por defecto):

1. Recopila cotizaciones y cierres diarios de su universo de valores y guarda el histórico para trabajar sin red.
2. Calcula medias de 20 y 50 sesiones, RSI de 14, momento ajustado por volatilidad y ATR.
3. Puntúa cada valor con pesos que **aprende** de sus operaciones cerradas.
4. Gestiona lo que tiene: stop de protección en el libro, stop que sube con el precio, toma de beneficios y salida si
   se rompe la tendencia.
5. Abre posiciones con la bolsa abierta arriesgando como mucho el 1 % del patrimonio por operación (stop a 2 ATR),
   con límites de peso por valor, de posiciones y una reserva de liquidez.

Su **diario** explica por qué compra, vende o espera. En la vista Bolsa se ven en vivo su curva de patrimonio y el
**gráfico de cada orden activa** (precio, límite, stop, ejecución y objetivo). Se ajusta o se para en la vista Bolsa o
con `POST /api/bolsa/agente` (`activo`, `intervalo_s`, `riesgo_pct`, `max_posiciones`, `max_peso_pct`,
`reserva_pct`, `universo`, `capital`, `var_max_pct`, `correlacion_max`); `GET /api/bolsa/agente` da su estado.
Nunca opera con dinero real.

Desde la 4.2 controla además el riesgo de la cartera entera: no abre posiciones nuevas si el **VaR histórico de un
día** supera `var_max_pct` (3 % del patrimonio por defecto) y no compra un valor cuyas rentabilidades diarias se
mueven casi igual (correlación mayor que `correlacion_max`, 0,85) que las de uno que ya tiene. Lo deja escrito en
su diario.

## Terminal financiero

Desde la 4.2 los agentes de la oficina de bolsa (analista, gestor de carteras, riesgos, operador y fiscalidad) tienen
las herramientas de un terminal financiero, y en la vista Bolsa el panel **TERMINAL DE BOLSA** las reúne con
formularios. Funcionan con los datos reales de `mercados.py` (TradingView, Yahoo, Nasdaq, SEC, FRED, CoinGecko...)
cuando hay conexión; las cifras siempre llevan su fuente y nada de esto es asesoramiento financiero.

| Herramienta | Qué hace |
|---|---|
| `mercado_panel` | Responde a una pregunta en lenguaje corriente con las recetas que hagan falta (`bolsa_terminal.ruta`): cotización, velas, indicadores, métricas, perfil, noticias, opciones, resultados, BPA, estimaciones, comparativa, criptomonedas, cribas, sectores, calendario |
| `mercado_consulta` | Una receta suelta con sus parámetros |
| `bolsa_verificar` | Comprueba que cada cifra de un texto sale de los datos consultados en la conversación (admite coma decimal, %, millones, billones) |
| `bolsa_opciones` | Black-Scholes con dividendos, griegas (delta, gamma, theta por día, vega, rho), volatilidad implícita y estrategias: `call_comprada`, `put_comprada`, `call_cubierta`, `put_protectora`, `collar`, `diferencial_alcista`, `diferencial_bajista`, `cono`, `cuna`, `mariposa`, `condor_hierro`; curva de resultado, máximos, equilibrios y probabilidad de beneficio |
| `bolsa_riesgo` | `cartera`: VaR histórico y paramétrico, CVaR, volatilidad (también EWMA), caída máxima, beta y correlaciones; `estres`: crisis de 2008, covid 2020, subida de tipos de 2022, puntocom 2000, estanflación de los setenta y corrección del 10 %; `posicion`: tamaño por riesgo máximo, stop, objetivo y Kelly fraccionado |
| `bolsa_valoracion` | `dcf` (valor por acción con tus supuestos) y `dcf_inverso` (qué crecimiento descuenta el precio) |
| `bolsa_criba` | Cribas `graham`, `lynch`, `greenblatt`, `buffett`, `dividendo`, `impulso`, `pequenas_valor`, o filtros en texto y una fórmula de puntuación propia (evaluada sin `eval`, con un analizador seguro) |
| `bolsa_alerta` | Alertas con varias condiciones (`precio`, `cambio_pct`, `volumen`, `rsi`, `sma20`, `sma50`, `sma200`, `distancia_maximo_pct`); se revisan cada 5 minutos como la fuente `alertas_bolsa` del Mundo |
| `bolsa_expediente` | Tesis, supuestos, evidencias con su fuente, riesgos y revisiones de cada valor (`data/bolsa/expedientes.json`) |
| `bolsa_informe_oficial` | SEC EDGAR: lista de presentaciones, lectura de 10-K/10-Q/8-K con impulsores y frenos en citas literales verificadas, novedades frente al informe anterior y cifras XBRL. La SEC pide un agente de usuario: se usa uno genérico o `bolsa.sec_agente` |
| `bolsa_validar` | Prueba una estrategia del laboratorio por tramos sucesivos del histórico (walk-forward) |

Código: `nexus/office/bolsa_analisis.py` (cálculos, alertas y expedientes), `bolsa_informes.py` (SEC),
`bolsa_terminal.py` (recetas y escritorio), herramientas en `nexus/tools/builtin/mercado_tools.py`. API:
`GET /api/mercados/recetas`, `POST /api/mercados/herramienta/<nombre>` (solo estas herramientas, pasando por el
supervisor) y `GET|POST /api/mercados/escritorio`.

## Agentes

| Agente | Qué hace | Herramientas de la mesa |
|---|---|---|
| Analista de mercados (`analista-mercados`) | Análisis fundamental (PER, BPA, EV/EBITDA, deuda, flujo de caja, dividendos) y técnico (medias, RSI, MACD), renta fija y criptoactivos | `bolsa_cotizacion`, `bolsa_mercados` |
| Gestor de carteras (`gestor-carteras`) | Asignación de activos, diversificación, rebalanceo, aportaciones periódicas, gastos, traspasos de fondos | `bolsa_cartera`, `bolsa_costes`, `bolsa_cotizacion` |
| Operador de mesa (`operador-bolsa`) | Da y vigila las órdenes, explica tipos de orden, horarios, comisiones y liquidación | todas menos el informe fiscal |
| Riesgos y cumplimiento (`riesgos-inversion`) | Tamaño de posición, stops, volatilidad, caídas máximas, MiFID II, KID, CNMV, chiringuitos, MiCA | `bolsa_cartera`, `bolsa_cotizacion`, `bolsa_costes` |
| Fiscalidad de inversiones (`fiscalidad-inversiones`) | IRPF de ganancias y pérdidas, FIFO, regla de los dos meses, dividendos, modelos 720 y 721 | `bolsa_informe_fiscal`, `bolsa_cartera` |

Además lleva los agentes de oficina comunes: recepción, agenda, administración, comercial y protección de datos.
Cada agente aprende en su propio grafo, como en el resto de perfiles (ver [OFICINA.md](OFICINA.md)).

```
python -m nexus oficina bolsa-inversiones
```

## La mesa simulada

Se abre en la sección **BOLSA** de la app (en el móvil, en *Más*) o con `python -m nexus bolsa`. Los datos se guardan
en `data/bolsa/bolsa.db`.

- **Cartera ficticia** en euros, con 10.000 € de partida (puedes crear otras con el capital y la tarifa que quieras,
  y reiniciarlas).
- **Valores de varias bolsas**: acciones de BME, Xetra, Euronext, Londres, NYSE y Nasdaq, ETF de renta variable y de
  bonos, y bitcoin y ether. Puedes añadir cualquier otro con su precio de referencia.
- **Precios que se mueven como en el mercado**: mientras la bolsa está abierta el precio sigue un paseo aleatorio
  con la volatilidad de cada activo, en cada apertura hay un pequeño salto y con la bolsa cerrada el precio no se
  mueve. Si pegas cierres reales (`fecha;cierre`, o un CSV con columnas *Date* y *Close*), el valor se ancla al último
  cierre y usa la volatilidad de esos datos.
- **Horarios reales**: cada bolsa en su hora local, con el cambio de hora de verano de Europa y de EE. UU. y sus
  festivos (Viernes Santo, Lunes de Pascua, 1 de mayo, Nochebuena, Navidad, San Esteban y Nochevieja en Europa; los
  de la NYSE en EE. UU.). Las criptomonedas cotizan 24 horas todos los días.
- **Órdenes** a mercado, limitadas, stop y stop limitadas, válidas el día o hasta cancelar (90 días). Con la bolsa
  cerrada la orden espera a la apertura. El efectivo de las compras abiertas queda reservado. No hay ventas en corto.
- **Ejecución realista**: se compra al precio de venta y se vende al de compra (horquilla), con **deslizamiento**
  que crece con el tamaño de la orden frente al volumen diario del valor.
- **Costes**: tres tarifas típicas (bróker online, banco tradicional, neobróker) con comisión nacional,
  internacional y de cripto; canon aproximado de BME; impuesto sobre transacciones financieras (0,2 % en compras de
  grandes empresas españolas, 0,3 % en francesas); y coste del cambio de divisa en valores en dólares o libras.
- **Resultado como en Hacienda**: posiciones por lotes **FIFO**, ganancia o pérdida realizada en euros con gastos
  incluidos, informe fiscal anual por valor y aviso de la **regla de los dos meses** (art. 33.5 LIRPF) cuando vendes
  con pérdidas y conservas o recompras títulos del mismo valor.

Las órdenes pendientes se revisan cada vez que se consulta la mesa (la app, la terminal o un agente). Las cifras de
tarifas, canon e impuestos son aproximadas y se pueden ajustar en `nexus/office/bolsa.py` (`TARIFAS`, `CANON_BME` y la
columna `impuesto_pct` de cada valor).

## Cartera real: nunca sin tu confirmación

Una cartera en modo **real** sirve para llevar el seguimiento de tus inversiones de verdad, y está pensada para que
nada se ejecute por su cuenta:

1. Cualquier orden (tuya o de un agente) queda **por confirmar**. No se envía a ningún sitio.
2. Solo una persona puede confirmarla, escribiendo `CONFIRMO`: en la app con el modo **ADMIN** activo, o en la
   terminal con `python -m nexus bolsa confirmar <id>`. Ningún agente tiene una herramienta para confirmar.
3. **Nexus-Office no se conecta a ningún bróker.** La orden confirmada la ejecutas tú en tu bróker.
4. Después registras la ejecución real (precio, comisiones) con *REGISTRAR EJECUCIÓN* o
   `python -m nexus bolsa ejecucion <id> <precio> --comision <euros>`, y la cartera se actualiza.

Crear una cartera real también requiere el modo administrador.

## Terminal

```
python -m nexus bolsa                         # resumen de la cartera
python -m nexus bolsa mercados                # bolsas abiertas o cerradas, en hora local
python -m nexus bolsa valores santander       # buscar valores
python -m nexus bolsa cotizar SAN.MC
python -m nexus bolsa comprar SAN.MC 100
python -m nexus bolsa comprar AAPL 5 --tipo limitada --limite 210 --validez hasta_cancelar
python -m nexus bolsa vender SAN.MC 50 --tipo stop --stop 5.6
python -m nexus bolsa ordenes | operaciones | cancelar <id>
python -m nexus bolsa nueva "Reto 2027" --capital 3000 --tarifa neobroker
python -m nexus bolsa reiniciar --cartera 2
python -m nexus bolsa importar SAN.MC cierres.csv
python -m nexus bolsa fiscal 2026
python -m nexus bolsa nueva "Mi cartera" --modo real --capital 12000     # seguimiento, sin ejecutar nada
python -m nexus bolsa confirmar <id>
python -m nexus bolsa ejecucion <id> 6.12 --comision 8
```

En la app Nexus-Office autónomo de Android la mesa está en la sección **BOLSA** igual que en el escritorio.

## API

| Método y ruta | Uso |
|---|---|
| `GET /api/bolsa?cartera=<id>` | Resumen, mercados, carteras, órdenes y operaciones |
| `GET /api/bolsa/instrumentos?q=` | Buscar valores |
| `GET /api/bolsa/cotizacion/<símbolo>` | Cotización simulada y últimos precios |
| `POST /api/bolsa/costes` | Gastos estimados de una orden |
| `POST /api/bolsa/orden` | Nueva orden (`simbolo`, `lado`, `cantidad`, `tipo`, `limite`, `stop`, `validez`, `cartera`) |
| `POST /api/bolsa/orden/<id>/cancelar` | Cancelar |
| `POST /api/bolsa/orden/<id>/confirmar` | Confirmar una orden real (`{"confirmacion": "CONFIRMO"}`, requiere admin) |
| `POST /api/bolsa/orden/<id>/ejecucion` | Registrar la ejecución real (requiere admin) |
| `POST /api/bolsa/cartera` | Crear cartera (la real requiere admin) |
| `POST /api/bolsa/cartera/<id>/reiniciar` | Reiniciar una cartera simulada |
| `POST /api/bolsa/instrumento` | Añadir un valor propio |
| `POST /api/bolsa/importar` | Importar cierres reales (`simbolo`, `csv`) |
| `GET /api/bolsa/fiscal?anio=` | Ganancias y pérdidas realizadas del año |
