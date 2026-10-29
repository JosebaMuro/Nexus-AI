// Hash, PBKDF2 (PIN de administrador compatible con la version Python), aleatorios, base64 y CRC32.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nx {

std::string sha256(std::string_view data);                 // binario (32 bytes)
// SHA-256 por partes (ficheros grandes)
class Sha256 {
 public:
  void update(const void* data, size_t n);
  std::string digest();                                     // binario (32 bytes); se llama una sola vez
 private:
  void block(const unsigned char* p);
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  unsigned char buf[64];
  size_t len = 0;
  uint64_t total = 0;
};
std::string sha256_hex(std::string_view data);
std::string sha1_hex(std::string_view data);   // solo para claves de cache e identificadores (no para seguridad)
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

// ---------------------------------------------------------------- cifrado (crypto_ext.cpp)
// Red COOP (scrypt + AES-256-GCM), VPN (claves Curve25519 de WireGuard) y certificado propio del acceso remoto
// (ECDSA P-256). AES-GCM y el certificado usan mbedtls; scrypt y X25519 van escritos aqui.
namespace nx {
std::string scrypt(std::string_view password, std::string_view salt, uint64_t n, uint32_t r, uint32_t p, size_t dklen);
std::string aes_gcm_encrypt(std::string_view key, std::string_view nonce, std::string_view plain, std::string_view aad);
// lanza std::runtime_error si la etiqueta no cuadra
std::string aes_gcm_decrypt(std::string_view key, std::string_view nonce, std::string_view ct_tag, std::string_view aad);
std::string x25519_base(std::string_view priv32);                     // clave publica (32 bytes)
std::pair<std::string, std::string> x25519_keypair();                 // (privada, publica) en bruto
// certificado autofirmado de servidor: (cert PEM, clave PEM). sans: IPs o nombres DNS
std::pair<std::string, std::string> selfsigned_cert(const std::string& cn, const std::string& org,
                                                    const std::vector<std::string>& sans, int days);
std::string cert_fingerprint(const std::string& cert_pem);            // SHA-256 del DER en hexadecimal (mayusculas)
}  // namespace nx
