# Turnos de modelos

Agente IA usa varios modelos a la vez: el de texto (y visión), el de imagen, la voz (whisper y Piper) y el de
embeddings para la memoria y la biblioteca. En un equipo o un móvil con poca RAM no caben todos cargados a la vez.
Desde la 4.3.1, cuando no caben, **se cargan por turnos según la etapa del trabajo** en la que esté cada paso, para
que en cada momento el paso en curso tenga toda la memoria y use el mejor modelo que ese hardware puede mover.

## Cómo funciona

- Al arrancar se suma lo que ocuparían todos los modelos activos y se compara con la RAM útil (la total menos una
  reserva para el sistema: el 20 % en un PC, mínimo 1,5 GB, y el 45 % en el móvil, porque Android cierra apps antes
  de llenar la memoria). Con GPU dedicada se cuenta también la VRAM. Si todo cabe, no cambia nada.
- Si no cabe, cada paso del flujo entra en su **etapa**: *texto*, *visión*, *imagen*, *voz a texto*, *texto a voz*
  y *búsqueda semántica*. Antes de empezar, si no hay sitio para su modelo, se descargan los que esa etapa no usa,
  del menos usado recientemente al más, hasta que quepa. Los modelos se vuelven a cargar solos cuando otra etapa
  los necesita.
- Un modelo que está trabajando nunca se descarga para otro.
- La **indexación semántica de fondo** no echa a ningún modelo: espera a que no haya etapas en curso y haya sitio.
- Cada etapa recibe su presupuesto de memoria y lo aprovecha:
  - **Texto**: si el modelo con el contexto del perfil no cabe, se reduce el contexto para esa respuesta (nunca por
    debajo de 2048 tokens) en vez de quedarse sin memoria o tirar de swap.
  - **Imagen** (stable-diffusion.cpp): primero el VAE troceado (`--vae-tiling`) y, si no basta, menos resolución.
    El resultado lo indica en `ajuste_memoria`.
  - **Voz a texto**: el whisper más grande que cabe, sin pasar del que pide el perfil.

## Autoajuste

Convive con el Autoajuste adaptativo: en *Sistema > Autoajuste* aparece «se cargan por turnos según la etapa del
flujo» cuando está activo. Las recargas de cada sesión se guardan en el historial; si el modelo de texto se ha
recargado muchas veces (8 o más y al menos el 40 % de las respuestas), el siguiente arranque reduce el contexto y
recomienda un modelo de texto más pequeño. Con la alternancia activa, el modelo de embeddings se para tras 90 s sin
uso en lugar de 10 minutos.

## Ajustes

En *Sistema > Turnos de modelos* se ve qué modelos hay, cuánto ocupa cada uno, si está cargado, sus recargas y las
últimas etapas. El modo (administrador) puede ser:

| Modo | Qué hace |
|---|---|
| `auto` (por defecto) | Alterna solo si los modelos no caben todos a la vez. |
| `siempre` | Cada etapa descarga todos los modelos que no usa (máximo de memoria para cada paso, más recargas). |
| `nunca` | Todo se queda cargado, como antes de la 4.3.1. |

Claves de configuración: `hardware.alternar_modelos` y `hardware.reserva_ram_mb` (0 = automática).

API: `GET /api/turnos` (estado) y `POST /api/turnos` con `{"modo": "auto|siempre|nunca", "reserva_ram_mb": n}`
(administrador). Código en `nexus/hardware/turnos.py`.
