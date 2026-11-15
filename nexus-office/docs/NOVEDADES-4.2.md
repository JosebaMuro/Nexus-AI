# Novedades de Agente IA 4.2

La 4.2 trae a la edición Python lo que aún faltaba de la edición C++ 3.x.

## Noticias de la mesa de analistas

- Botón **NOTICIAS** en el laboratorio de bolsa: titulares recientes del valor de Yahoo Finance (por símbolo) y de
  Google Noticias (por nombre de la empresa), cada uno con su sentimiento según el léxico financiero del laboratorio.
- La **mesa de analistas** usa esos titulares de los últimos 14 días (se puede desactivar con `"noticias": false`).
- Se guardan en `data/bolsa/noticias.json`: sin conexión la mesa sigue con lo guardado. Se consultan como mucho cada
  30 minutos y se olvidan a los 120 días.
- API: `GET /api/bolsa/noticias/<símbolo>?refrescar=true`.

## Bróker: de la mesa simulada a órdenes de verdad

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

## Editor de vídeo en el navegador

- **EDITOR DE VÍDEO** (sección Vídeo) abre `/static/video.html`: línea de tiempo con imágenes, textos, transiciones
  y música, y grabación a WebM en el propio navegador, sin ffmpeg ni conexión. El resultado se guarda en
  `outputs/videos/` con `POST /api/video/guardar`.
- Cada vídeo rápido (`crear_video`) deja junto al AVI un `.proyecto.json`: el enlace **Abrir en el editor** carga esas
  imágenes en el editor para retocarlo.

## GPS del móvil en segundo plano (APK autónoma)

- En **Ajustes > Ubicación**, la app Android muestra **Usar el GPS de este móvil**. Android pide permiso de ubicación
  y la app escribe la posición en `ubicacion.json` (fuente «app» de `ubicacion_actual`), sin internet.
- Con la app abierta funciona con el permiso normal. Con **TAMBIÉN CON LA APP CERRADA** Android pide «Permitir
  siempre» y el servicio de Agente IA sigue leyendo el GPS en segundo plano.
- Además del GPS escucha las posiciones que ya piden otras apps (proveedor pasivo), que no gastan batería extra.
- La posición no sale del móvil.

## Lo que sigue solo en la edición C++

La capa `hal/` (memoria compartida con el acelerador, anillos sin bloqueo) con los pipelines de percepción y pantalla,
y el motor propio de difusión ONNX, son piezas nativas de la C++ y no se portan: en la edición Python las imágenes se
generan con los motores de `nexus/motores/` (ver [MOTORES.md](MOTORES.md)).
