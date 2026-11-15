"""Mundo: informacion publica al dia, ordenada por categorias y paises, para que los agentes la consulten.

Las fuentes (nexus/mundo/fuentes.py) se piden cada cierto tiempo, regulable por fuente y con un factor general,
y se guardan en una base local. Inspirado en gods-eye-view (MIT) y en OpenTerminal de ErTasselli (MIT); no se ha
copiado codigo de esos proyectos.
"""
from .servicio import Mundo, SinClave  # noqa: F401
from .fuentes import CATEGORIAS, FUENTES, Fuente  # noqa: F401
