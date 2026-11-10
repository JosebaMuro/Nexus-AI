# Laboratorio BETA: pruebas de rendimiento y aspecto (desde la 2.10.0-beta.1)

Esta beta trae herramientas para **medir el rendimiento** de Agente IA en cada equipo y para **evaluar su aspecto**,
y guarda todo en `data/beta/` para comparar versiones, equipos y temas. No cambia el comportamiento de la oficina.

## Laboratorio BETA (interfaz)

Se abre con el botón del matraz en la barra lateral (en el móvil, en *Más*) o con la etiqueta **BETA** junto al logo.

### Rendimiento
- **Banco de pruebas**: CPU con un hilo y con todos (y el escalado), copia de memoria, escritura y lectura de disco, y
  la IA activa (tiempo hasta el primer token y tokens por segundo, en 1 a 10 rondas con tres tipos de petición). Cada
  medición se guarda y las tarjetas muestran la diferencia con la anterior (▲ mejor, ▼ peor).
- **Navegador**: FCP, LCP, saltos de diseño (CLS), INP aproximado, tareas largas, memoria JS y nodos del DOM.
- **API**: tiempo de cada ruta en este navegador frente al tiempo del núcleo (cabecera `Server-Timing`), para separar
  la red y el navegador del trabajo del servidor, y las rutas más lentas de todos los clientes.
- **Fluidez**: FPS y fotograma p95 durante 3 s con los efectos completos y con los ligeros.
- **HUD flotante** (Alt+Mayús+P): FPS, tirones, API p50/p95, tareas largas y memoria, encima de cualquier vista.
- **Exportar JSON** con todas las mediciones e **Informe .md** con equipo, mediciones, API y opiniones.

### Aspecto
- Vista previa de los temas de serie sin guardarlos (o *Recorrer*, 4 s cada uno) y vuelta al tuyo.
- Efectos visuales completos, ligeros o automáticos, y disposición escritorio, tablet o móvil, en este dispositivo.
- Galería con todos los componentes (botones, etiquetas, campos, avisos, mensajes del chat, tablas…).
- Contraste WCAG 2.1 (AA y AAA) de las combinaciones de color del tema que estás viendo.

### Opiniones
Nota de 1 a 5 por zona (chat, salón, bolsa, móvil…) y comentario. Se guarda con el tema, la disposición, los efectos,
la pantalla y las métricas del navegador. En modo ADMIN se ve la nota media por zona y las últimas opiniones.

## Terminal

```bash
nexus-office bench                       # CPU, memoria, disco e IA; lo guarda en data/beta/bench.jsonl
nexus-office bench --rondas 5 --tokens 256
nexus-office bench --sin-llm             # solo el equipo
nexus-office bench --solo-llm --json     # solo la IA, en JSON
nexus-office bench --informe             # informe en Markdown
```

## API

| Ruta | Qué hace |
|---|---|
| `GET /api/beta` | versión, canal, compilación, IA activa y recuentos |
| `GET /api/beta/http` · `DELETE` (admin) | tiempos por ruta (n, media, p50, p95, máximo, errores) · poner a cero |
| `GET /api/beta/bench?n=50` · `POST /api/beta/bench` | historial · medir (`rondas`, `max_tokens`, `llm`, `sistema`, `cliente`) |
| `POST /api/beta/opinion` · `GET /api/beta/opiniones` (admin) | guardar una opinión (`zona`, `nota`, `texto`…) · listarlas |
| `GET /api/beta/informe` | informe en Markdown |

Todas las respuestas de la API llevan `Server-Timing: app;dur=<ms>`.
