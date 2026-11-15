"""Terminal IA: consola con sesiones, interprete de codigo con memoria, ficheros con deshacer y un agente que
trabaja con todo ello, para personas (interfaz y `nexus terminal`) y para agentes (herramientas terminal_*).

Reune en una sola pieza lo que hacen Open Terminal (API de ordenes y ficheros), OpenCode (agente de programacion
con modos plan y construir, ordenes personalizadas, edicion por texto exacto) y Open Interpreter (interprete con
estado, AGENTS.md, habilidades). Licencias MIT y Apache-2.0; no se ha copiado codigo.
"""
from .nucleo import MODOS, Terminal  # noqa: F401
