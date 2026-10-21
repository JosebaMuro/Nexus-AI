// Hash, PBKDF2 (PIN de administrador compatible con la version Python), aleatorios, base64 y CRC32.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace nx {

std::string sha256(std::string_view data);                 // binario (32 bytes)
std::string sha256_hex(std::string_view data);
std::string hmac_sha256(std::string_view key, std::string_view msg);
std::string pbkdf2_sha256(std::string_view password, std::string_view salt, int iterations, size_t dklen = 32);
std::string to_hex(std::string_view bin);
std::string from_hex(std::string_view hex);
std::string random_bytes(size_t n);
std::string token_hex(size_t nbytes);                       // secrets.token_hex
std::string token_urlsafe(size_t nbytes);                   // secrets.token_urlsafe
std::string uuid_hex();                                     // uuid.uuid4().hex
std::string b64encode(std::string_view data);
std::string b64decode(std::string_view data);               // tolera saltos de linea y relleno ausente
std::string b64url_encode(std::string_view data);
uint32_t crc32(std::string_view data);                      // zlib.crc32
bool consteq(std::string_view a, std::string_view b);       // secrets.compare_digest

// tiempo
double now();                                               // time.time()
std::string strftime_local(const char* fmt, double t = -1); // time.strftime
int weekday_local(double t = -1);                            // 0 = lunes (datetime.weekday)

}  // namespace nx
