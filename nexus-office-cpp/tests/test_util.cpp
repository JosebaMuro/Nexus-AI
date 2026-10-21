#include "check.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/proc.h"
#include "util/rx.h"
#include "util/text.h"
#include "util/yaml.h"

using namespace nx;

TEST("texto: normalize y tokens") {
  CHECK_EQ(normalize("Árbol CAMIÓN Ñandú"), std::string("arbol camion nandu"));
  auto t = tokens("¿Qué tal, Señor_X? 42");
  CHECK_EQ(t.size(), 4u);
  CHECK_EQ(t[1], std::string("tal"));
  CHECK_EQ(ulen("añoß"), 4u);
  CHECK_EQ(usub("España", 2, 5), std::string("pañ"));
  CHECK_EQ(usub("España", -3), std::string("aña"));
}

TEST("regex: semantica de re") {
  Rx w(R"(\b\w+ción\b)", Rx::I);
  auto m = w.search("La CANCIÓN y la acción");
  CHECK(m.has_value());
  CHECK_EQ(Rx(R"((\d+)-(\d+))").sub("1-2 y 30-40", R"(\2:\1)"), std::string("2:1 y 40:30"));
  CHECK_EQ(Rx(R"(,\s*)").split("a, b,c").size(), 3u);
  CHECK_EQ(Rx(R"((?<=@)\w+)").findall("x@uno y@dos").size(), 2u);
  CHECK_EQ(Rx("x*").sub("abc", "-"), std::string("-a-b-c-"));
}

TEST("yaml: carga y volcado ida y vuelta") {
  json j = yaml::load("a: 'x: y'\nb:\n- '1'\n- 2\n- si\nc:\n  d: |\n    linea1\n    linea2\n  e: []\nf:\n- g: 1\n  h: 'yes'\ni: null\nj: 0.5\nk: yes\n");
  CHECK_EQ(j["a"].get<std::string>(), std::string("x: y"));
  CHECK(j["b"][0].is_string());
  CHECK(j["b"][1].is_number_integer());
  CHECK_EQ(j["c"]["d"].get<std::string>(), std::string("linea1\nlinea2\n"));
  CHECK(j["c"]["e"].is_array());
  CHECK(j["i"].is_null());
  CHECK_EQ(j["j"].get<double>(), 0.5);
  CHECK(j["k"].get<bool>());
  CHECK_EQ(j["f"][0]["h"].get<std::string>(), std::string("yes"));
  std::string d = yaml::dump(j);
  CHECK_EQ(yaml::load(d), j);
  json k = {{"txt", "uno\ndos"}, {"vacio", ""}, {"num", "007"}, {"lista", json::array({json::object(), "a"})}};
  CHECK_EQ(yaml::load(yaml::dump(k)), k);
  bool threw = false;
  try {
    yaml::load("a: [1, 2");
  } catch (...) {
    threw = true;
  }
  CHECK(threw);
}

TEST("crypto: pbkdf2 compatible con Python") {
  CHECK_EQ(to_hex(pbkdf2_sha256("1234", from_hex("00112233445566778899aabbccddeeff"), 200000)),
           std::string("98cd56f19733a361768b76706415776c6f3e161aef907b7e88c5ea3fe5639ab7"));
  CHECK_EQ(crc32("hola mundo"), 2900312582u);
  CHECK_EQ(b64decode(b64encode("ñandú\x01")), std::string("ñandú\x01"));
  CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

#ifndef _WIN32
TEST("procesos: run con captura, entrada y tiempo limite") {
  auto r = proc::run({"sh", "-c", "cat; echo err >&2; exit 3"}, 10, {.input = "hola"});
  CHECK(r.started);
  CHECK_EQ(r.code, 3);
  CHECK_EQ(r.out, std::string("hola"));
  CHECK_EQ(r.err, std::string("err\n"));
  auto t = proc::run({"sleep", "5"}, 0.3);
  CHECK(t.timed_out);
  auto n = proc::run({"no-existe-este-programa"}, 1);
  CHECK(!n.started);
}
#endif

TEST("http: url") {
  auto u = http::parse_url("https://huggingface.co:8443/api/models?search=x#y");
  CHECK_EQ(u.host, std::string("huggingface.co"));
  CHECK_EQ(u.port, 8443);
  CHECK_EQ(u.path, std::string("/api/models?search=x"));
  CHECK_EQ(http::query_string({{"q", "a b"}, {"z", "ñ"}}), std::string("q=a+b&z=%C3%B1"));
}
