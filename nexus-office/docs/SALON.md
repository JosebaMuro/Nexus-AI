# Salón virtual de agentes

El salón es a la vez una **visualización** (los agentes sentados alrededor de una mesa holográfica, con burbujas de diálogo y haces de luz cuando delegan) y un **mecanismo de aprendizaje colectivo**.

## Despachos (2.2)
La pestaña **DESPACHOS**, junto a **SALÓN**, muestra un despacho por agente (una, dos o tres columnas según la
pantalla) con una escena animada de lo que hace **de verdad** en ese momento. La actividad llega del servidor
(`GET /api/agentes/actividad` y el evento `agent_activity` del WebSocket):

| Actividad | Escena |
|---|---|
| Pensando | Mano en la barbilla, sorbos de café y bocadillo de pensamiento |
| Escribiendo, programando, calculando | Teclea; la pantalla de la pared muestra texto, código o cifras |
| Buscando en internet, leyendo | Navega o pasa páginas de un documento |
| Dibujando o editando imágenes | Lápiz sobre la tableta gráfica |
| Componiendo música | Cascos puestos y notas que salen |
| Preguntando al Profesor (IA online) | Habla por teléfono |
| Delegando | Pasa la tarea al compañero |
| En reunión | Sale por la puerta; cartel *EN EL SALÓN* |
| Sin tarea | Se levanta a la cafetera y vuelve |

La ventana de cada despacho sigue la **hora real** (día, atardecer, noche) y la barra de abajo dice qué hace y en qué tarea.
La pestaña elegida se recuerda en ese dispositivo.

## Personajes: mujer u hombre (2.2)
Cada personaje del salón y de los despachos, **también el tuyo**, puede ser mujer u hombre. Cambia el peinado, la
complexión, la barba (solo hombre), las pestañas y pendientes (solo mujer) y los labios.

- Agentes: *Agentes → editar → Sexo*, o el campo `sexo: mujer|hombre` del YAML. De serie: Nexus, Artista y Profesor
  son mujeres; Investigador, Programador y Forjador, hombres. Vacío = se elige por el nombre.
- Tú: *Ajustes → Tu personaje* (`user.sexo`).

## Qué se registra
- **Informes de tarea**: cada petición resuelta deja en el salón el agente, los temas y las herramientas usadas (y si fallaron).
- **Delegaciones**: cuando un agente encarga una subtarea a otro, aparece un haz animado entre ambos.
- **Retroalimentación**: tus 👍/👎 suman o restan puntos al agente en esos temas.

## Directorio de habilidades
Con esos datos se construye una matriz *agente × tema* (éxitos suavizados por Laplace). Se usa para:
1. **Enrutado automático**: `palabras clave + 0,8 × habilidad aprendida`.
2. **Delegación**: el prompt de cada agente incluye “Lo aprendido en el salón sobre tus compañeros”.

## Reuniones
Se convocan manualmente (con tema opcional) o automáticamente cada `lounge.meet_every_tasks` tareas (8 por defecto):
1. Presentaciones (con LLM, cada agente habla en personaje; sin él, a partir de sus datos).
2. Repaso de tareas recientes: si otro agente tenía mejor encaje, lo dice (“puedes delegarme esto”).
3. **Detección de carencias**: temas que se repiten sin especialista o con resultados flojos.
4. **Propuesta de nuevo agente** (nombre, descripción, prompt, palabras clave, herramientas). Queda **pendiente de aprobación** de un administrador; al aprobarse, el agente se presenta en el salón y el enrutado empieza a usarlo.

Puedes hablar en el salón: responde el agente con mejor encaje para lo que digas.
