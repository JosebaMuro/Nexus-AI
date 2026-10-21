// scrypt (RFC 7914), X25519 (Curve25519 de TweetNaCl, dominio publico), AES-256-GCM y certificado ECDSA P-256 propio.
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "util/crypto.h"

#ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace nx {

// ======================================================================== scrypt
namespace {

inline uint32_t rotl(uint32_t a, int b) { return (a << b) | (a >> (32 - b)); }

void salsa20_8(uint32_t B[16]) {
  uint32_t x[16];
  std::memcpy(x, B, sizeof x);
  for (int i = 0; i < 8; i += 2) {
    x[4] ^= rotl(x[0] + x[12], 7);   x[8] ^= rotl(x[4] + x[0], 9);
    x[12] ^= rotl(x[8] + x[4], 13);  x[0] ^= rotl(x[12] + x[8], 18);
    x[9] ^= rotl(x[5] + x[1], 7);    x[13] ^= rotl(x[9] + x[5], 9);
    x[1] ^= rotl(x[13] + x[9], 13);  x[5] ^= rotl(x[1] + x[13], 18);
    x[14] ^= rotl(x[10] + x[6], 7);  x[2] ^= rotl(x[14] + x[10], 9);
    x[6] ^= rotl(x[2] + x[14], 13);  x[10] ^= rotl(x[6] + x[2], 18);
    x[3] ^= rotl(x[15] + x[11], 7);  x[7] ^= rotl(x[3] + x[15], 9);
    x[11] ^= rotl(x[7] + x[3], 13);  x[15] ^= rotl(x[11] + x[7], 18);
    x[1] ^= rotl(x[0] + x[3], 7);    x[2] ^= rotl(x[1] + x[0], 9);
    x[3] ^= rotl(x[2] + x[1], 13);   x[0] ^= rotl(x[3] + x[2], 18);
    x[6] ^= rotl(x[5] + x[4], 7);    x[7] ^= rotl(x[6] + x[5], 9);
    x[4] ^= rotl(x[7] + x[6], 13);   x[5] ^= rotl(x[4] + x[7], 18);
    x[11] ^= rotl(x[10] + x[9], 7);  x[8] ^= rotl(x[11] + x[10], 9);
    x[9] ^= rotl(x[8] + x[11], 13);  x[10] ^= rotl(x[9] + x[8], 18);
    x[12] ^= rotl(x[15] + x[14], 7); x[13] ^= rotl(x[12] + x[15], 9);
    x[14] ^= rotl(x[13] + x[12], 13); x[15] ^= rotl(x[14] + x[13], 18);
  }
  for (int i = 0; i < 16; ++i) B[i] += x[i];
}

// B y Y: 32*r palabras
void blockmix(const uint32_t* B, uint32_t* Y, uint32_t r) {
  uint32_t X[16];
  std::memcpy(X, B + (2 * r - 1) * 16, 64);
  for (uint32_t i = 0; i < 2 * r; ++i) {
    for (int k = 0; k < 16; ++k) X[k] ^= B[i * 16 + k];
    salsa20_8(X);
    // los pares van a la primera mitad y los impares a la segunda
    std::memcpy(Y + ((i & 1) ? (r + i / 2) : (i / 2)) * 16, X, 64);
  }
}

}  // namespace

std::string scrypt(std::string_view password, std::string_view salt, uint64_t n, uint32_t r, uint32_t p, size_t dklen) {
  if (n < 2 || (n & (n - 1))) throw std::invalid_argument("scrypt: n debe ser potencia de 2");
  size_t words = 32 * (size_t)r;
  std::string B = pbkdf2_sha256(password, salt, 1, (size_t)p * 128 * r);
  std::vector<uint32_t> X(words), Y(words), V(words * n);
  for (uint32_t i = 0; i < p; ++i) {
    auto* blk = (unsigned char*)B.data() + (size_t)i * 128 * r;
    for (size_t k = 0; k < words; ++k)
      X[k] = blk[4 * k] | (blk[4 * k + 1] << 8) | (blk[4 * k + 2] << 16) | ((uint32_t)blk[4 * k + 3] << 24);
    for (uint64_t j = 0; j < n; ++j) {
      std::memcpy(&V[j * words], X.data(), words * 4);
      blockmix(X.data(), Y.data(), r);
      X.swap(Y);
    }
    for (uint64_t j = 0; j < n; ++j) {
      uint64_t idx = X[(2 * r - 1) * 16] & (n - 1);
      for (size_t k = 0; k < words; ++k) X[k] ^= V[idx * words + k];
      blockmix(X.data(), Y.data(), r);
      X.swap(Y);
    }
    for (size_t k = 0; k < words; ++k) {
      blk[4 * k] = (unsigned char)X[k];
      blk[4 * k + 1] = (unsigned char)(X[k] >> 8);
      blk[4 * k + 2] = (unsigned char)(X[k] >> 16);
      blk[4 * k + 3] = (unsigned char)(X[k] >> 24);
    }
  }
  return pbkdf2_sha256(password, B, 1, dklen);
}

// ======================================================================== X25519 (TweetNaCl)
namespace {
using gf = int64_t[16];
const gf K121665 = {0xDB41, 1};

void car(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (int64_t)1 << 16;
    int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c << 16;
  }
}
void sel(gf p, gf q, int b) {
  int64_t c = ~(int64_t)(b - 1);
  for (int i = 0; i < 16; ++i) {
    int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}
void pack(unsigned char* o, const gf n) {
  gf m, t;
  for (int i = 0; i < 16; ++i) t[i] = n[i];
  car(t);
  car(t);
  car(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    sel(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = (unsigned char)(t[i] & 0xff);
    o[2 * i + 1] = (unsigned char)(t[i] >> 8);
  }
}
void unpack(gf o, const unsigned char* n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}
void A(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
void Z(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
void M(gf o, const gf a, const gf b) {
  int64_t t[31] = {0};
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  car(o);
  car(o);
}
void Sq(gf o, const gf a) { M(o, a, a); }
void inv(gf o, const gf i) {
  gf c;
  for (int a = 0; a < 16; ++a) c[a] = i[a];
  for (int a = 253; a >= 0; --a) {
    Sq(c, c);
    if (a != 2 && a != 4) M(c, c, i);
  }
  for (int a = 0; a < 16; ++a) o[a] = c[a];
}
void scalarmult(unsigned char* q, const unsigned char* n, const unsigned char* p) {
  unsigned char z[32];
  int64_t x[80];
  gf a, b, c, d, e, f;
  for (int i = 0; i < 31; ++i) z[i] = n[i];
  z[31] = (n[31] & 127) | 64;
  z[0] &= 248;
  unpack(x, p);
  for (int i = 0; i < 16; ++i) {
    b[i] = x[i];
    d[i] = a[i] = c[i] = 0;
  }
  a[0] = d[0] = 1;
  for (int i = 254; i >= 0; --i) {
    int r = (z[i >> 3] >> (i & 7)) & 1;
    sel(a, b, r);
    sel(c, d, r);
    A(e, a, c);
    Z(a, a, c);
    A(c, b, d);
    Z(b, b, d);
    Sq(d, e);
    Sq(f, a);
    M(a, c, a);
    M(c, b, e);
    A(e, a, c);
    Z(a, a, c);
    Sq(b, a);
    Z(c, d, f);
    M(a, c, K121665);
    A(a, a, d);
    M(c, c, a);
    M(a, d, f);
    M(d, b, x);
    Sq(b, e);
    sel(a, b, r);
    sel(c, d, r);
  }
  for (int i = 0; i < 16; ++i) {
    x[i + 16] = a[i];
    x[i + 32] = c[i];
    x[i + 48] = b[i];
    x[i + 64] = d[i];
  }
  inv(x + 32, x + 32);
  M(x + 16, x + 16, x + 32);
  pack(q, x + 16);
}
}  // namespace

std::string x25519_base(std::string_view priv32) {
  if (priv32.size() != 32) throw std::invalid_argument("clave X25519 de 32 bytes");
  unsigned char base[32] = {9}, out[32];
  scalarmult(out, (const unsigned char*)priv32.data(), base);
  return std::string((char*)out, 32);
}

std::pair<std::string, std::string> x25519_keypair() {
  std::string k = random_bytes(32);
  k[0] = (char)((unsigned char)k[0] & 248);   // como `wg genkey`
  k[31] = (char)(((unsigned char)k[31] & 127) | 64);
  return {k, x25519_base(k)};
}

// ======================================================================== AES-GCM y certificado (mbedtls)
#ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
namespace {
struct Gcm {
  mbedtls_gcm_context c;
  explicit Gcm(std::string_view key) {
    mbedtls_gcm_init(&c);
    if (mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, (const unsigned char*)key.data(), (unsigned)key.size() * 8))
      throw std::runtime_error("AES-GCM: clave no valida");
  }
  ~Gcm() { mbedtls_gcm_free(&c); }
};

struct Rng {
  mbedtls_entropy_context ent;
  mbedtls_ctr_drbg_context drbg;
  Rng() {
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    const char* pers = "nexus-office-cert";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char*)pers, std::strlen(pers)))
      throw std::runtime_error("no hay fuente de azar para el certificado");
  }
  ~Rng() {
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
  }
};

std::string utc_stamp(std::time_t t) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
  return buf;
}
}  // namespace

std::string aes_gcm_encrypt(std::string_view key, std::string_view nonce, std::string_view plain, std::string_view aad) {
  Gcm g(key);
  std::string out(plain.size() + 16, '\0');
  auto* o = (unsigned char*)out.data();
  if (mbedtls_gcm_crypt_and_tag(&g.c, MBEDTLS_GCM_ENCRYPT, plain.size(), (const unsigned char*)nonce.data(), nonce.size(),
                                (const unsigned char*)aad.data(), aad.size(), (const unsigned char*)plain.data(), o, 16,
                                o + plain.size()))
    throw std::runtime_error("AES-GCM: fallo al cifrar");
  return out;
}

std::string aes_gcm_decrypt(std::string_view key, std::string_view nonce, std::string_view ct, std::string_view aad) {
  if (ct.size() < 16) throw std::runtime_error("AES-GCM: trama corta");
  Gcm g(key);
  size_t n = ct.size() - 16;
  std::string out(n, '\0');
  if (mbedtls_gcm_auth_decrypt(&g.c, n, (const unsigned char*)nonce.data(), nonce.size(), (const unsigned char*)aad.data(),
                               aad.size(), (const unsigned char*)ct.data() + n, 16, (const unsigned char*)ct.data(),
                               (unsigned char*)out.data()))
    throw std::runtime_error("AES-GCM: etiqueta no valida");
  return out;
}

std::pair<std::string, std::string> selfsigned_cert(const std::string& cn, const std::string& org,
                                                    const std::vector<std::string>& sans, int days) {
  Rng rng;
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  struct Guard {
    mbedtls_pk_context* k;
    mbedtls_x509write_cert* c;
    ~Guard() {
      mbedtls_x509write_crt_free(c);
      mbedtls_pk_free(k);
    }
  } guard{&key, &crt};
  auto check = [](int rc, const char* what) {
    if (rc) throw std::runtime_error(std::string("certificado: ") + what + " (" + std::to_string(rc) + ")");
  };
  check(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)), "pk_setup");
  check(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &rng.drbg), "clave");
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  std::string dn = "CN=" + cn + ",O=" + org;
  check(mbedtls_x509write_crt_set_subject_name(&crt, dn.c_str()), "sujeto");
  check(mbedtls_x509write_crt_set_issuer_name(&crt, dn.c_str()), "emisor");
  std::string serial = random_bytes(16);
  serial[0] = (char)((unsigned char)serial[0] & 0x7F);
  check(mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char*)serial.data(), serial.size()), "serie");
  std::time_t t = std::time(nullptr);
  check(mbedtls_x509write_crt_set_validity(&crt, utc_stamp(t - 86400).c_str(), utc_stamp(t + (std::time_t)days * 86400).c_str()),
        "validez");
  check(mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1), "restricciones");
  mbedtls_asn1_sequence eku{};
  eku.buf.tag = MBEDTLS_ASN1_OID;
  eku.buf.p = (unsigned char*)MBEDTLS_OID_SERVER_AUTH;
  eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
  check(mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku), "uso");
  // nombres alternativos: IP en bruto (4 o 16 bytes) o nombre DNS
  std::vector<std::string> raw;
  std::vector<int> types;
  for (auto& s : sans) {
    unsigned char b[16];
    if (inet_pton(AF_INET, s.c_str(), b) == 1) {
      raw.emplace_back((char*)b, 4);
      types.push_back(MBEDTLS_X509_SAN_IP_ADDRESS);
    } else if (inet_pton(AF_INET6, s.c_str(), b) == 1) {
      raw.emplace_back((char*)b, 16);
      types.push_back(MBEDTLS_X509_SAN_IP_ADDRESS);
    } else {
      raw.push_back(s);
      types.push_back(MBEDTLS_X509_SAN_DNS_NAME);
    }
  }
  std::vector<mbedtls_x509_san_list> list(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    list[i].node.type = types[i];
    list[i].node.san.unstructured_name.p = (unsigned char*)raw[i].data();
    list[i].node.san.unstructured_name.len = raw[i].size();
    list[i].next = i + 1 < raw.size() ? &list[i + 1] : nullptr;
  }
  if (!list.empty()) check(mbedtls_x509write_crt_set_subject_alternative_name(&crt, list.data()), "nombres");
  std::string cert(8192, '\0'), pkey(4096, '\0');
  check(mbedtls_x509write_crt_pem(&crt, (unsigned char*)cert.data(), cert.size(), mbedtls_ctr_drbg_random, &rng.drbg), "firma");
  check(mbedtls_pk_write_key_pem(&key, (unsigned char*)pkey.data(), pkey.size()), "clave PEM");
  cert.resize(std::strlen(cert.c_str()));
  pkey.resize(std::strlen(pkey.c_str()));
  return {cert, pkey};
}

std::string cert_fingerprint(const std::string& pem) {
  mbedtls_x509_crt c;
  mbedtls_x509_crt_init(&c);
  int rc = mbedtls_x509_crt_parse(&c, (const unsigned char*)pem.c_str(), pem.size() + 1);
  if (rc) {
    mbedtls_x509_crt_free(&c);
    throw std::runtime_error("certificado ilegible");
  }
  std::string der((const char*)c.raw.p, c.raw.len);
  mbedtls_x509_crt_free(&c);
  std::string hex = sha256_hex(der);
  for (auto& ch : hex) ch = (char)std::toupper((unsigned char)ch);
  return hex;
}
#else
static std::runtime_error no_tls() { return std::runtime_error("esta compilacion no incluye mbedtls (cifrado no disponible)"); }
std::string aes_gcm_encrypt(std::string_view, std::string_view, std::string_view, std::string_view) { throw no_tls(); }
std::string aes_gcm_decrypt(std::string_view, std::string_view, std::string_view, std::string_view) { throw no_tls(); }
std::pair<std::string, std::string> selfsigned_cert(const std::string&, const std::string&, const std::vector<std::string>&,
                                                    int) {
  throw no_tls();
}
std::string cert_fingerprint(const std::string&) { throw no_tls(); }
#endif

}  // namespace nx
