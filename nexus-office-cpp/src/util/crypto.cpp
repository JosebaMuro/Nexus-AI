#include "util/crypto.h"

#include <cstdio>

#include <miniz.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>

namespace nx {

// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void Sha256::block(const unsigned char* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
void Sha256::update(const void* data, size_t n) {
    auto* p = (const unsigned char*)data;
    total += n;
    while (n) {
      size_t take = std::min(n, 64 - len);
      std::memcpy(buf + len, p, take);
      len += take; p += take; n -= take;
      if (len == 64) { block(buf); len = 0; }
    }
}
std::string Sha256::digest() {
    uint64_t bits = total * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char z = 0;
    while (len != 56) update(&z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    update(lb, 8);
    std::string out(32, '\0');
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j) out[4 * i + j] = (char)(h[i] >> (24 - 8 * j));
    return out;
}

std::string sha256(std::string_view d) {
  Sha256 s;
  s.update(d.data(), d.size());
  return s.digest();
}

std::string sha1_hex(std::string_view data) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string m(data);
  uint64_t bits = (uint64_t)data.size() * 8;
  m.push_back((char)0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; --i) m.push_back((char)(bits >> (i * 8)));
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)(uint8_t)m[off + 4 * i] << 24 | (uint32_t)(uint8_t)m[off + 4 * i + 1] << 16 |
             (uint32_t)(uint8_t)m[off + 4 * i + 2] << 8 | (uint32_t)(uint8_t)m[off + 4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  }
  char out[41];
  for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
  return std::string(out, 40);
}

std::string sha256_hex(std::string_view d) { return to_hex(sha256(d)); }

std::string hmac_sha256(std::string_view key, std::string_view msg) {
  std::string k(key);
  if (k.size() > 64) k = sha256(k);
  k.resize(64, '\0');
  std::string ipad(64, 0), opad(64, 0);
  for (int i = 0; i < 64; ++i) { ipad[i] = (char)(k[i] ^ 0x36); opad[i] = (char)(k[i] ^ 0x5c); }
  Sha256 in;
  in.update((const unsigned char*)ipad.data(), 64);
  in.update((const unsigned char*)msg.data(), msg.size());
  std::string inner = in.digest();
  Sha256 out;
  out.update((const unsigned char*)opad.data(), 64);
  out.update((const unsigned char*)inner.data(), inner.size());
  return out.digest();
}

std::string pbkdf2_sha256(std::string_view password, std::string_view salt, int iterations, size_t dklen) {
  // HMAC con la clave precalculada: 200.000 iteraciones en milisegundos
  std::string k(password);
  if (k.size() > 64) k = sha256(k);
  k.resize(64, '\0');
  unsigned char ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) { ipad[i] = (unsigned char)(k[i] ^ 0x36); opad[i] = (unsigned char)(k[i] ^ 0x5c); }
  Sha256 base_in, base_out;
  base_in.update(ipad, 64);
  base_out.update(opad, 64);
  auto prf = [&](const unsigned char* m, size_t n) {
    Sha256 a = base_in;
    a.update(m, n);
    std::string inner = a.digest();
    Sha256 b = base_out;
    b.update((const unsigned char*)inner.data(), 32);
    return b.digest();
  };
  std::string out;
  for (uint32_t blk = 1; out.size() < dklen; ++blk) {
    std::string s(salt);
    s += (char)(blk >> 24); s += (char)(blk >> 16); s += (char)(blk >> 8); s += (char)blk;
    std::string u = prf((const unsigned char*)s.data(), s.size());
    std::string t = u;
    for (int i = 1; i < iterations; ++i) {
      u = prf((const unsigned char*)u.data(), 32);
      for (int j = 0; j < 32; ++j) t[j] ^= u[j];
    }
    out += t;
  }
  out.resize(dklen);
  return out;
}

std::string to_hex(std::string_view bin) {
  static const char* hx = "0123456789abcdef";
  std::string o;
  o.reserve(bin.size() * 2);
  for (unsigned char c : bin) { o += hx[c >> 4]; o += hx[c & 15]; }
  return o;
}

std::string from_hex(std::string_view hex) {
  std::string o;
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    int a = v(hex[i]), b = v(hex[i + 1]);
    if (a < 0 || b < 0) break;
    o += (char)(a * 16 + b);
  }
  return o;
}

std::string random_bytes(size_t n) {
  static std::mutex mu;
  static std::random_device rd;
  static std::mt19937_64 rng(((uint64_t)rd() << 32) ^ rd() ^
                             (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::lock_guard<std::mutex> lk(mu);
  std::string o(n, '\0');
  for (size_t i = 0; i < n; ++i) o[i] = (char)((rng() ^ ((uint64_t)rd() << 7)) & 0xFF);
  return o;
}

std::string token_hex(size_t n) { return to_hex(random_bytes(n)); }

std::string b64url_encode(std::string_view data) {
  std::string s = b64encode(data);
  for (char& c : s) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!s.empty() && s.back() == '=') s.pop_back();
  return s;
}

std::string token_urlsafe(size_t n) { return b64url_encode(random_bytes(n)); }

std::string uuid_hex() {
  std::string b = random_bytes(16);
  b[6] = (char)((b[6] & 0x0F) | 0x40);
  b[8] = (char)((b[8] & 0x3F) | 0x80);
  return to_hex(b);
}

std::string b64encode(std::string_view data) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8) | (unsigned char)data[i + 2];
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
  }
  if (i + 1 == data.size()) {
    uint32_t v = (unsigned char)data[i] << 16;
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
  } else if (i + 2 == data.size()) {
    uint32_t v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8);
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
  }
  return o;
}

std::string b64decode(std::string_view data) {
  auto v = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  std::string o;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : data) {
    int x = v(c);
    if (x < 0) continue;
    acc = (acc << 6) | (uint32_t)x;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      o += (char)((acc >> bits) & 0xFF);
    }
  }
  return o;
}

uint32_t crc32(std::string_view d) {
  return (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)d.data(), d.size());
}

bool consteq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char r = 0;
  for (size_t i = 0; i < a.size(); ++i) r |= (unsigned char)(a[i] ^ b[i]);
  return r == 0;
}

double now() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::tm local_tm(double t) {
  std::time_t tt = (std::time_t)(t < 0 ? now() : t);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  return tm;
}

std::string strftime_local(const char* fmt, double t) {
  std::tm tm = local_tm(t);
  char buf[128];
  size_t n = std::strftime(buf, sizeof buf, fmt, &tm);
  return std::string(buf, n);
}

int weekday_local(double t) {
  std::tm tm = local_tm(t);
  return (tm.tm_wday + 6) % 7;
}

}  // namespace nx
