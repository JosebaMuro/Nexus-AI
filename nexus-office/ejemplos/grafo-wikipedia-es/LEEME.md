# Grafo multicapa de la Wikipedia en español

Generado el 2026-10-27 con `tools/wikipedia_grafo.py` desde la API pública de es.wikipedia.org y wikidata.org. Es un ejemplo de grafo de conocimiento para importar en Nexus-Office (**Ajustes > Gestor de conocimiento > Importar / exportar**, o `graph_import` desde el chat).

- **2251 nodos** y **7007 relaciones**.
- Raíces temáticas: Ciencia, Historia, Geografía, Arte, Tecnología, Economía, Filosofía, Deporte, Música, Literatura, Medicina, Matemáticas, Biología, Física, Química, Informática, Derecho, Política, Gastronomía, Cine, Arquitectura, Astronomía, Educación, Religión, Idiomas, España, América Latina, Empresas, Agricultura, Medio ambiente.

| Capa | Qué contiene | Nodos | Relaciones |
|---|---|---|---|
| `1-temas` | categorías de la Wikipedia y su jerarquía; cada artículo `pertenece_a` sus categorías | 373 | 1758 |
| `2-articulos` | artículos, con el resumen de su introducción y su descripción | 1383 | 0 |
| `3-enlaces` | enlaces `enlaza_a` entre artículos del grafo | 0 | 2624 |
| `4-wikidata` | hechos de Wikidata: `es_un`, `pais`, `situado_en`, `autor`... y atributos como población o fechas | 495 | 2625 |

Relaciones más frecuentes: `enlaza_a` (2624), `pertenece_a` (1412), `es_un` (983), `subclase_de` (441), `ocupacion` (355), `subcategoria_de` (346), `pais` (157), `nacionalidad` (143), `parte_de` (78), `estudio_en` (78), `campo` (67), `trabajo_para` (59), `pais_de_origen` (54), `nacio_en` (51), `genero` (39).

## Ficheros
- `grafo.graphml`: GraphML con los atributos `capa`, `type`, `resumen`, `url`, `wikidata`... Se abre en Gephi o yEd y se importa en Nexus-Office.
- `grafo.json`: el mismo grafo en JSON node-link (D3, NetworkX, sigma.js).

## Cómo importarlo
```
nexus-office grafo importar grafo.graphml --en auto        # edición C++
python -m nexus grafo importar grafo.graphml --en auto      # edición Python
```
Con `--en auto`, lo común va al grafo general y lo especializado al agente que corresponde. Los atributos de cada nodo (resumen, población, fechas) quedan en su nota y los agentes los usan al responder.

## Licencia
Textos de la Wikipedia en español bajo [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/deed.es) (autores: colaboradores de la Wikipedia; cada nodo lleva su `url`). Datos de Wikidata bajo CC0.
