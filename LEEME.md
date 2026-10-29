# Grafo multicapa de la Wikipedia en español

Generado el 2026-10-29 con `nexus-construir grafo-wikipedia` desde la API pública de es.wikipedia.org y wikidata.org. Es un ejemplo de grafo de conocimiento para importar en Nexus-Office (**Ajustes > Gestor de conocimiento > Importar / exportar**, o `graph_import` desde el chat).

- **1884 nodos** y **4899 relaciones**.
- Raíces temáticas: Ciencia, Historia, Geografía, Arte, Tecnología, Economía, Filosofía, Deporte, Música, Literatura, Medicina, Matemáticas, Biología, Física, Química, Informática, Derecho, Política, Gastronomía, Cine, Arquitectura, Astronomía, Educación, Religión, Idiomas, España, América Latina, Empresas, Agricultura, Medio ambiente.

| Capa | Qué contiene | Nodos | Relaciones |
|---|---|---|---|
| `1-temas` | categorías de la Wikipedia y su jerarquía; cada artículo `pertenece_a` sus categorías | 373 | 1758 |
| `2-articulos` | artículos, con el resumen de su introducción y su descripción | 1383 | 0 |
| `3-enlaces` | enlaces `enlaza_a` entre artículos del grafo | 0 | 2623 |
| `4-wikidata` | hechos de Wikidata: `es_un`, `pais`, `situado_en`, `autor`... y atributos como población o fechas | 128 | 518 |

Relaciones más frecuentes: `enlaza_a` (2623), `pertenece_a` (1412), `subcategoria_de` (346), `es_un` (208), `subclase_de` (83), `ocupacion` (56), `pais` (40), `nacionalidad` (26), `genero` (23), `pais_de_origen` (18), `parte_de` (17), `nacio_en` (10), `campo` (10), `murio_en` (7), `nombre_por` (4).

## Ficheros
- `grafo.graphml`: GraphML con los atributos `capa`, `type`, `resumen`, `url`, `wikidata`... Se abre en Gephi o yEd y se importa en Nexus-Office.
- `grafo.json`: el mismo grafo en JSON node-link (D3, NetworkX, sigma.js).

## Cómo importarlo
```
nexus-office grafo importar grafo.graphml --en auto
```
Con `--en auto`, lo común va al grafo general y lo especializado al agente que corresponde. Los atributos de cada nodo (resumen, población, fechas) quedan en su nota y los agentes los usan al responder.

## Licencia
Textos de la Wikipedia en español bajo [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/deed.es) (autores: colaboradores de la Wikipedia; cada nodo lleva su `url`). Datos de Wikidata bajo CC0.
