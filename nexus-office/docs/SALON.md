# Salón virtual de agentes

El salón es a la vez una **visualización** (los agentes sentados alrededor de una mesa holográfica, con burbujas de diálogo y haces de luz cuando delegan) y un **mecanismo de aprendizaje colectivo**.

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
