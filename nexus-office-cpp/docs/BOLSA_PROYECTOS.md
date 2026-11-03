# Proyectos de inversión con IA importados al agente de bolsa

Se revisaron en GitHub los proyectos libres más usados de inversión asistida por IA y se apuntaron las etapas que
recorren desde el dato hasta una inversión real. Cada etapa se ha **reescrito en C++** dentro de Nexus-Office (sin
Python, sin dependencias nuevas y sin copiar su código), así que funciona sin conexión en Windows, Linux, macOS y
Android igual que el resto de la oficina. La tabla dice de dónde sale cada idea y su licencia.

| Proyecto | Licencia | Qué hace | Qué se ha tomado |
|---|---|---|---|
| [TauricResearch/TradingAgents](https://github.com/TauricResearch/TradingAgents) | Apache-2.0 | Equipo de agentes LLM que imita una firma de trading | Mesa de analistas: técnico, noticias, fundamental, debate alcista/bajista, operador, equipo de riesgo (agresivo, neutral, conservador) y gestor con lecciones de operaciones pasadas |
| [virattt/ai-hedge-fund](https://github.com/virattt/ai-hedge-fund) | MIT | Fondo simulado con agentes por estilo inversor, gestor de riesgo y backtester | Señales técnicas combinadas (tendencia, reversión, momento, volatilidad, Hurst) con confianza; tamaño por volatilidad |
| [AI4Finance-Foundation/FinRL](https://github.com/AI4Finance-Foundation/FinRL) | MIT | Aprendizaje por refuerzo: datos → entorno → agente → backtest → paper trading (Alpaca) | El recorrido backtest → simulación → real y el enlace con Alpaca en papel |
| [AI4Finance-Foundation/FinRobot](https://github.com/AI4Finance-Foundation/FinRobot) · [FinGPT](https://github.com/AI4Finance-Foundation/FinGPT) | Apache-2.0 · MIT | Agentes y LLM financieros, sentimiento de noticias | Analista de noticias y sentimiento (titulares RSS con léxico financiero y, si hay, la IA financiera) |
| [microsoft/qlib](https://github.com/microsoft/qlib) | MIT | Plataforma cuantitativa: datos, factores, modelos, backtest y análisis | Banco de datos local de cierres diarios y backtest con tramo fuera de muestra |
| [freqtrade/freqtrade](https://github.com/freqtrade/freqtrade) | GPL-3.0 | Bot con backtesting, dry-run, protecciones y live | Solo la idea (no el código): protecciones que frenan las compras (caída máxima, racha de pérdidas, mal día) y el paso dry-run → live |
| [mementum/backtrader](https://github.com/mementum/backtrader) · [kernc/backtesting.py](https://github.com/kernc/backtesting.py) | GPL-3.0 · AGPL-3.0 | Motores de backtest | Solo la idea: simulación día a día con comisiones y deslizamiento |
| [ranaroussi/quantstats](https://github.com/ranaroussi/quantstats) | Apache-2.0 | Métricas de carteras | Rentabilidad, CAGR, volatilidad, Sharpe, Sortino, caída máxima y su duración, Calmar, VaR y CVaR, aciertos, factor de beneficio |
| [PyPortfolio/PyPortfolioOpt](https://github.com/PyPortfolio/PyPortfolioOpt) | MIT | Optimización de carteras | Varianza mínima, Sharpe máximo, paridad de riesgo, HRP de López de Prado y asignación en títulos enteros |
| [QuantConnect/Lean](https://github.com/QuantConnect/Lean) · [nautilus_trader](https://github.com/nautechsystems/nautilus_trader) | Apache-2.0 · LGPL-3.0 | Motores con brókers, paper y live | La separación entre papel y real y el envío de órdenes a un bróker |
| [OpenBB](https://github.com/openbq-org/OpenBB) · [a-stock-data](https://github.com/simonlin1212/a-stock-data) | AGPL-3.0 · — | Plataformas de datos financieros | El banco de datos: históricos, noticias e informes guardados para trabajar sin conexión |

El código GPL/AGPL no se ha copiado ni adaptado: Nexus-Office es MIT y de esos proyectos solo se han tomado ideas
generales y bien conocidas (backtest con costes, protecciones). Los algoritmos públicos (Sharpe, HRP, paridad de
riesgo, exponente de Hurst, MACD, Bollinger) están implementados desde su definición.

## Las cinco etapas

`GET /api/bolsa/etapas` (y la herramienta `bolsa_etapas`) dice en qué punto está la estrategia y qué le falta.
En la app es la fila de tarjetas del **Laboratorio de inversión**.

| Etapa | Qué pide | Dónde |
|---|---|---|
| 1. Banco de datos | Al menos un valor con 120 sesiones diarias guardadas | `bolsa_real.cpp` (`banco`, `banco_descargar`), tabla `historico` |
| 2. Investigación | Un informe de la mesa de analistas en los últimos 30 días | `bolsa_mesa.cpp`, tablas `bolsa_informes` y `bolsa_noticias` |
| 3. Backtest | Último backtest con las reglas del agente: Sharpe ≥ 0,5, caída máxima ≤ 25 %, ≥ 10 operaciones y resultado positivo fuera de muestra | `bolsa_cuant.cpp` (`backtest`), tabla `bolsa_backtests` |
| 4. Simulación | El agente autónomo lleva ≥ 30 días, ≥ 10 operaciones cerradas, gana y no cae más de un 20 % | `bolsa_agente.cpp` (cartera simulada con precios reales) |
| 5. Inversión real asistida | Etapas 3 y 4 superadas, una cartera real elegida y las propuestas habilitadas por la persona | `bolsa_real.cpp` (`proponer_real`, broker) |

Con la etapa 5 habilitada, cada compra o venta que el agente hace en su cartera simulada se **propone** en la
cartera real, a escala de su patrimonio y como orden **por confirmar**. Nada sale hasta que una persona escribe
`CONFIRMO`. Con un bróker conectado, la orden confirmada se envía; si no, la ejecutas tú y registras el precio.

## Qué hace cada parte

- **Señales** (`bolsa_senales`, `GET /api/bolsa/senales/<símbolo>`): cinco estrategias técnicas con su señal (-1 a 1)
  y su confianza, combinadas en una lectura alcista, neutral o bajista, más RSI, MACD y bandas de Bollinger.
- **Backtest** (`bolsa_backtest`, `POST /api/bolsa/backtest`): reglas del agente autónomo (las mismas que usa cada
  ciclo, incluido el aprendizaje de pesos), señales combinadas, cruce de medias 20/50 o comprar y mantener. Stops y
  objetivos con el máximo y el mínimo del día, comisión (% con mínimo), deslizamiento y reserva de liquidez. Compara
  con comprar y mantener a partes iguales y separa el último 30 % como fuera de muestra.
- **Monte Carlo** (`bolsa_montecarlo`, `POST /api/bolsa/montecarlo`): remuestrea las rentabilidades diarias de un
  valor o de un backtest guardado: abanico de valores finales, probabilidad de perder y caída máxima esperable.
- **Cartera óptima** (`bolsa_optimizar`, `POST /api/bolsa/optimizar`): pesos iguales, inversa de la volatilidad,
  varianza mínima, paridad de riesgo, Sharpe máximo o HRP, con tope por valor, contribución al riesgo y títulos
  enteros para un capital.
- **Protecciones** del agente: deja de abrir posiciones si la cartera cae más de `max_drawdown_pct` (15 %) desde su
  máximo de 30 días, tras `racha_perdidas` (4) operaciones perdedoras seguidas durante `pausa_racha_h` (24 h), o si
  pierde más de `perdida_diaria_pct` (3 %) en 24 horas. Sigue gestionando stops y salidas. 0 desactiva cada freno.
- **Mesa de analistas** (`bolsa_informe`, `POST /api/bolsa/informe`): ocho papeles con la IA financiera instalada; sin
  IA cada papel lo hace una regla y el informe sale igual. Con `bolsa.agente.mesa = true` el agente autónomo solo
  compra si el gestor dice «comprar» (la opinión vale 4 horas).
- **Noticias** (`bolsa_noticias`, `GET /api/bolsa/noticias/<símbolo>`): titulares de Yahoo Finance y Google Noticias
  por RSS, sin clave, con sentimiento de -1 a 1 y más peso a lo reciente. Se guardan 120 días.
- **Bróker** (`GET/POST /api/bolsa/broker`, admin): `manual` (por defecto) o `alpaca`. Alpaca empieza en su entorno de
  **papel** (`paper-api.alpaca.markets`); el real (`api.alpaca.markets`) solo si lo eliges. Opera acciones de EE. UU. y
  cripto en USD. Las claves se guardan como secretos de la configuración. `POST /api/bolsa/broker/sincronizar` (y
  cada ciclo del agente) recoge las ejecuciones y las registra en la cartera.

## Lo que no hace

- No hay datos contables en vivo (exigen clave o registro): el analista fundamental usa el conocimiento del modelo y
  avisa de que puede estar desfasado.
- No entrena redes ni aprendizaje por refuerzo como FinRL o Qlib: el agente aprende ajustando los pesos de sus
  factores con cada operación cerrada, y la IA financiera revisa cada compra.
- No es asesoramiento financiero. Un backtest bueno no garantiza nada: por eso existe la etapa de simulación con
  precios reales antes de proponer dinero.
