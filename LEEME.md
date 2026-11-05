# Grafo multicapa de la Wikipedia en español

Generado el 2026-11-05 con `nexus-construir grafo-wikipedia` desde la API pública de es.wikipedia.org y wikidata.org. Es un ejemplo de grafo de conocimiento para importar en Agente IA (**Ajustes > Gestor de conocimiento > Importar / exportar**, o `graph_import` desde el chat).

- **2247 nodos** y **6980 relaciones**.
- Raíces temáticas: Ciencia, Historia, Geografía, Arte, Tecnología, Economía, Filosofía, Deporte, Música, Literatura, Medicina, Matemáticas, Biología, Física, Química, Informática, Derecho, Política, Gastronomía, Cine, Arquitectura, Astronomía, Educación, Religión, Idiomas, España, América Latina, Empresas, Agricultura, Medio ambiente.

| Capa | Qué contiene | Nodos | Relaciones |
|---|---|---|---|
| `1-temas` | categorías de la Wikipedia y su jerarquía; cada artículo `pertenece_a` sus categorías | 373 | 1755 |
| `2-articulos` | artículos, con el resumen de su introducción y su descripción | 1379 | 0 |
| `3-enlaces` | enlaces `enlaza_a` entre artículos del grafo | 0 | 2616 |
| `4-wikidata` | hechos de Wikidata: `es_un`, `pais`, `situado_en`, `autor`... y atributos como población o fechas | 495 | 2609 |

Relaciones más frecuentes: `enlaza_a` (2616), `pertenece_a` (1409), `es_un` (978), `subclase_de` (442), `subcategoria_de` (346), `ocupacion` (346), `pais` (161), `nacionalidad` (140), `parte_de` (82), `estudio_en` (78), `campo` (61), `trabajo_para` (59), `pais_de_origen` (54), `nacio_en` (50), `genero` (39).

## Ficheros
- `grafo.graphml`: GraphML con los atributos `capa`, `type`, `resumen`, `url`, `wikidata`... Se abre en Gephi o yEd y se importa en Agente IA.
- `grafo.json`: el mismo grafo en JSON node-link (D3, NetworkX, sigma.js).

## Cómo importarlo
```
nexus-office grafo importar grafo.graphml --en auto
```
Con `--en auto`, lo común va al grafo general y lo especializado al agente que corresponde. Los atributos de cada nodo (resumen, población, fechas) quedan en su nota y los agentes los usan al responder.

## Licencia
Textos de la Wikipedia en español bajo [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/deed.es) (autores: colaboradores de la Wikipedia; cada nodo lleva su `url`). Datos de Wikidata bajo CC0.
