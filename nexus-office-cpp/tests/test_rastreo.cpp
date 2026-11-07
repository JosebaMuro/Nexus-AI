// Rastreador web: listas de URLs y cola de rastreos
#include "check.h"
#include "rag/web.h"
#include "util/json.h"

using namespace nx;
namespace cj = nx::rag::crawl_jobs;

TEST("rastreo: lista de URLs (lineas, comas, comentarios, sin esquema y repetidas)") {
  std::vector<std::string> bad;
  auto urls = cj::parse_url_list(
      "# documentacion del proyecto\n"
      "https://docs.ejemplo.com/guia/\n"
      "\n"
      "  http://otra-web.es/manual , wiki.ejemplo.org/ayuda; https://docs.ejemplo.com/guia\n"
      "// otra nota\n"
      "<https://tres.example.net/a?b=1>\tnada-de-url\r\n"
      "ftp://servidor.es/fichero\n"
      "http://localhost:8080/docs\n",
      &bad);
  CHECK_EQ(urls.size(), (size_t)5);
  if (urls.size() == 5) {
    CHECK_EQ(urls[0], std::string("https://docs.ejemplo.com/guia/"));
    CHECK_EQ(urls[1], std::string("http://otra-web.es/manual"));
    CHECK_EQ(urls[2], std::string("https://wiki.ejemplo.org/ayuda"));
    CHECK_EQ(urls[3], std::string("https://tres.example.net/a?b=1"));
    CHECK_EQ(urls[4], std::string("http://localhost:8080/docs"));
  }
  CHECK_EQ(bad.size(), (size_t)2);   // «nada-de-url» y el ftp://
  CHECK(cj::parse_url_list("").empty());
  CHECK(cj::parse_url_list("# solo comentarios\n\n").empty());
}

TEST("rastreo: un trabajo en cola sale de ella al pausarlo") {
  json job = cj::create("https://docs.ejemplo.com/", 10, 2, "path", "", true, 50, 1000000, true);
  std::string id = job["id"];
  CHECK_EQ(jstr(cj::get(id), "status"), std::string(cj::QUEUED));
  CHECK(!cj::is_running(id));
  CHECK(cj::stop(id));
  json j = cj::get(id);
  CHECK_EQ(jstr(j, "status"), std::string(cj::PAUSED));
  CHECK_EQ(jstr(j, "reason"), std::string("sacado de la cola"));
  CHECK(!cj::stop(id));   // ya no esta ni en marcha ni en cola
  json normal = cj::create("https://docs.ejemplo.com/b/", 10, 2, "path", "", true, 50, 1000000);
  CHECK_EQ(jstr(normal, "status"), std::string(cj::RUNNING));
}
