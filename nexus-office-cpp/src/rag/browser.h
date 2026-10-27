// Leer paginas como un navegador (nexus/rag/browser.py): Chrome, Edge, Chromium o Brave sin interfaz ejecutan el
// JavaScript y Nexus lee el DOM final.
//
// Muchas webs (React, Vue, Angular, tiendas, documentacion moderna...) mandan un HTML casi vacio y pintan el contenido
// con JavaScript. find() busca un navegador del equipo (o rag.browser_path) sin instalar nada; needs_browser() decide
// si una pagina lo necesita; render() la abre en --headless y devuelve el DOM; maybe_render() es lo que usan el
// rastreador, «URL a PDF» y la herramienta web de los agentes segun rag.browser: auto, siempre o nunca.
//
// Seguridad: el navegador sale por un proxy local (net::GuardProxy) que solo deja direcciones publicas, salvo
// rag.allow_private_urls. Perfil propio de Nexus por hueco, limpio antes de cada lectura.
#pragma once
#include <optional>
#include <string>
#include <utility>

#include "util/json.h"

namespace nx::rag::browser {

constexpr int BUDGET_MS = 8000;   // tiempo virtual que se deja al JavaScript antes de leer el DOM

std::string find(const std::string& preferred = "");   // "" si no hay navegador (Android, servidores minimos...)
bool needs_browser(const std::string& raw);
// DOM final tras ejecutar el JavaScript; lanza std::runtime_error si no hay navegador o falla
std::string render(const std::string& url, std::string exe = "", int budget_ms = BUDGET_MS,
                   std::optional<bool> allow_private = std::nullopt);
std::string mode();   // auto | siempre | nunca
// (html, leida_con_navegador): usa el navegador si el modo lo pide y hay uno; si no, devuelve lo descargado
std::pair<std::string, bool> maybe_render(const std::string& url, const std::string& raw, std::string how = "");
json info();          // {modo, navegador, disponible}

}  // namespace nx::rag::browser
