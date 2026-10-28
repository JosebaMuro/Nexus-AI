#include "util/http.h"

#include <httplib.h>

#include <chrono>
#include <fstream>
#include <thread>

#include "util/log.h"
#include "util/text.h"

namespace nx::http {

const char* UA = "nexus-office/1.0";

static const auto LOG = log::get("net");

std::string Response::header(const std::string& k) const {
  auto it = headers.find(lower(k));
  return it == headers.end() ? "" : it->second;
}

bool tls_available() {
#ifdef NEXUS_HAVE_TLS
  return true;
#else
  return false;
#endif
}

Url parse_url(const std::string& url) {
  Url u;
  size_t p = url.find("://");
  std::string rest = url;
  if (p != std::string::npos) {
    u.scheme = lower(url.substr(0, p));
    rest = url.substr(p + 3);
  } else {
    u.scheme = "http";
  }
  size_t slash = rest.find_first_of("/?#");
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  u.path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (!u.path.empty() && u.path[0] == '?') u.path = "/" + u.path;
  size_t h = u.path.find('#');
  if (h != std::string::npos) u.path.resize(h);
  size_t at = hostport.rfind('@');
  if (at != std::string::npos) hostport = hostport.substr(at + 1);
  if (!hostport.empty() && hostport[0] == '[') {
    size_t e = hostport.find(']');
    u.host = hostport.substr(1, e - 1);
    if (e + 1 < hostport.size() && hostport[e + 1] == ':') u.port = std::atoi(hostport.c_str() + e + 2);
  } else {
    size_t c = hostport.rfind(':');
    if (c != std::string::npos) {
      u.host = hostport.substr(0, c);
      u.port = std::atoi(hostport.c_str() + c + 1);
    } else {
      u.host = hostport;
    }
  }
  if (!u.port) u.port = u.scheme == "https" ? 443 : 80;
  return u;
}

std::string query_string(const std::map<std::string, std::string>& params) {
  std::string q;
  for (auto& [k, v] : params) {
    if (!q.empty()) q += "&";
    q += url_encode(k, true) + "=" + url_encode(v, true);
  }
  return q;
}

namespace {

std::string ca_file() {
  std::string e = env("SSL_CERT_FILE");
  if (!e.empty() && is_file(P(e))) return e;
  fs::path p = paths::package() / "certs" / "cacert.pem";
  if (is_file(p)) return S(p);
  return "";
}

bool no_proxy_for(const std::string& host) {
  if (host == "127.0.0.1" || host == "localhost" || host == "::1") return true;
  std::string np = env("NO_PROXY");
  if (np.empty()) np = env("no_proxy");
  for (auto& t : split(np, ",")) {
    std::string e = lower(trim(t));
    if (e.empty()) continue;
    if (e == "*") return true;
    if (starts_with(e, ".")) e = e.substr(1);
    std::string h = lower(host);
    if (h == e || ends_with(h, "." + e)) return true;
  }
  return false;
}

void configure_proxy(httplib::Client& cli, const Url& u) {
  if (no_proxy_for(u.host)) return;
  std::string pv = u.scheme == "https" ? env("HTTPS_PROXY") : env("HTTP_PROXY");
  if (pv.empty()) pv = u.scheme == "https" ? env("https_proxy") : env("http_proxy");
  if (pv.empty()) pv = env("ALL_PROXY");
  if (pv.empty()) return;
  Url p = parse_url(pv);
  cli.set_proxy(p.host, p.port);
  // credenciales del proxy (usuario:clave@)
  size_t s = pv.find("://");
  std::string rest = s == std::string::npos ? pv : pv.substr(s + 3);
  size_t at = rest.find('@');
  if (at != std::string::npos) {
    std::string cred = rest.substr(0, at);
    size_t c = cred.find(':');
    cli.set_proxy_basic_auth(url_decode(cred.substr(0, c)), c == std::string::npos ? "" : url_decode(cred.substr(c + 1)));
  }
}

Response do_send(const Request& req, int redirects_left) {
  Response out;
  Url u = parse_url(req.url);
  if (u.scheme != "http" && u.scheme != "https") {
    out.error = "esquema no soportado: " + u.scheme;
    return out;
  }
#ifndef NEXUS_HAVE_TLS
  if (u.scheme == "https") {
    out.error = "esta compilacion no incluye HTTPS";
    return out;
  }
#endif
  std::string base = u.scheme + "://" + (u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host) + ":" +
                     std::to_string(u.port);
  httplib::Client cli(base);
  double rt = req.read_timeout < 0 ? req.timeout : req.read_timeout;
  cli.set_connection_timeout(std::chrono::milliseconds((long long)(std::min(req.timeout, 30.0) * 1000)));
  cli.set_read_timeout(std::chrono::milliseconds((long long)(rt * 1000)));
  cli.set_write_timeout(std::chrono::milliseconds((long long)(std::max(req.timeout, 30.0) * 1000)));
  cli.set_follow_location(false);   // las redirecciones se siguen aqui (pueden cambiar de host)
  cli.set_decompress(true);
#ifdef NEXUS_HAVE_TLS
  if (u.scheme == "https") {
    std::string ca = ca_file();
    if (!ca.empty()) cli.set_ca_cert_path(ca);
    cli.enable_server_certificate_verification(true);
  }
#endif
  configure_proxy(cli, u);

  httplib::Request r;
  r.method = req.method;
  r.path = u.path;
  bool has_ua = false;
  for (auto& [k, v] : req.headers) {
    r.headers.emplace(k, v);
    if (lower(k) == "user-agent") has_ua = true;
  }
  if (!has_ua) r.headers.emplace("User-Agent", UA);
  if (!req.body.empty() || req.method == "POST" || req.method == "PUT") {
    r.body = req.body;
    r.headers.emplace("Content-Type", req.content_type.empty() ? "application/octet-stream" : req.content_type);
  }
  bool redirect = false;
  r.response_handler = [&](const httplib::Response& res) {
    out.status = res.status;
    for (auto& [k, v] : res.headers) out.headers[lower(k)] = v;
    if (req.follow && res.status >= 300 && res.status < 400 && res.has_header("Location")) {
      redirect = true;
      return true;   // se lee (y descarta) el cuerpo
    }
    if (req.on_headers && !req.on_headers(out)) return false;
    return true;
  };
  bool stopped = false;
  r.content_receiver = [&](const char* d, size_t n, size_t, size_t) {
    if (redirect) return true;
    if (req.on_data) {
      if (!req.on_data(d, n)) {
        stopped = true;
        return false;
      }
      return true;
    }
    out.body.append(d, n);
    return true;
  };
  auto res = cli.send(r);
  if (res && out.status == 0) {   // 204 y HEAD: httplib no llama a response_handler cuando no hay cuerpo
    out.status = res->status;
    for (auto& [k, v] : res->headers) out.headers[lower(k)] = v;
  }
  if (!res && !stopped && out.status == 0) {
    out.error = httplib::to_string(res.error());
    return out;
  }
  if (redirect) {
    if (redirects_left <= 0) {
      out.error = "demasiadas redirecciones";
      return out;
    }
    std::string loc = out.header("location");
    std::string next;
    if (starts_with(loc, "http://") || starts_with(loc, "https://")) next = loc;
    else if (starts_with(loc, "//")) next = u.scheme + ":" + loc;
    else if (starts_with(loc, "/")) next = base + loc;
    else {
      std::string dir = u.path.substr(0, u.path.rfind('/') + 1);
      next = base + dir + loc;
    }
    Request r2 = req;
    r2.url = next;
    if (out.status == 303 || ((out.status == 301 || out.status == 302) && req.method == "POST")) {
      r2.method = "GET";
      r2.body.clear();
    }
    // no reenviar credenciales a otro host
    if (parse_url(next).host != u.host)
      std::erase_if(r2.headers, [](auto& kv) { return lower(kv.first) == "authorization"; });
    return do_send(r2, redirects_left - 1);
  }
  return out;
}

}  // namespace

Response send(const Request& req) { return do_send(req, 10); }

Response get(const std::string& url, const Headers& h, double timeout) {
  Request r;
  r.url = url;
  r.headers = h;
  r.timeout = timeout;
  return send(r);
}

Response post_json(const std::string& url, const json& body, const Headers& h, double timeout) {
  Request r;
  r.method = "POST";
  r.url = url;
  r.headers = h;
  r.body = jdump(body);
  r.content_type = "application/json";
  r.timeout = timeout;
  return send(r);
}

json get_json(const std::string& url, const Headers& h, const std::map<std::string, std::string>& params,
              double timeout) {
  std::string u = url;
  if (!params.empty()) u += (contains(u, "?") ? "&" : "?") + query_string(params);
  Response r = get(u, h, timeout);
  if (r.status == 0) throw HttpError(0, r.error);
  if (!r.ok()) throw HttpError(r.status, cat("HTTP ", r.status, " en ", url));
  json j = r.json_body();
  if (j.is_discarded()) throw HttpError(r.status, "respuesta JSON invalida de " + url);
  return j;
}

Response stream_lines(Request req, const std::function<bool(const std::string&)>& on_line) {
  std::string buf;
  bool go = true;
  int status = 0;
  std::string err_body;
  auto prev_headers = req.on_headers;
  req.on_headers = [&](const Response& r) {
    status = r.status;
    return prev_headers ? prev_headers(r) : true;
  };
  req.on_data = [&](const char* d, size_t n) {
    if (status >= 400) {
      err_body.append(d, n);
      return true;
    }
    buf.append(d, n);
    size_t pos;
    while (go && (pos = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      buf.erase(0, pos + 1);
      go = on_line(line);
    }
    return go;
  };
  Response r = send(req);
  if (go && !buf.empty() && status < 400) on_line(buf);
  if (status >= 400) r.body = err_body;
  return r;
}

fs::path download(const std::string& url, const fs::path& dest, const Headers& h,
                  const std::function<void(uint64_t, uint64_t)>& progress, const std::function<bool()>& cancel,
                  int retries) {
  mkdirs(dest.parent_path());
  fs::path part = dest;
  part += ".part";
  for (int attempt = 0; attempt < retries; ++attempt) {
    uint64_t have = is_file(part) ? fsize(part) : 0;
    Request r;
    r.url = url;
    r.headers = h;
    if (have) r.headers.push_back({"Range", cat("bytes=", have, "-")});
    r.timeout = 30;
    r.read_timeout = 120;
    std::ofstream fh;
    uint64_t done = have, total = 0;
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    bool cancelled = false, complete = false;
    std::string werr;
    r.on_headers = [&](const Response& res) {
      if (res.status == 416) {
        complete = true;
        return false;
      }
      if (res.status >= 400) return true;
      if (have && res.status != 206) have = 0;   // el servidor no permite reanudar
      done = have;
      total = (uint64_t)std::atoll(res.header("content-length").c_str()) + have;
      fh.open(part, have ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc));
      if (!fh) werr = "no se puede escribir " + S(part);
      return werr.empty();
    };
    r.on_data = [&](const char* d, size_t n) {
      if (cancel && cancel()) {
        cancelled = true;
        return false;
      }
      if (!fh.is_open()) return true;   // cuerpo de error
      fh.write(d, (std::streamsize)n);
      if (!fh) {
        werr = "error escribiendo (disco lleno?)";
        return false;
      }
      done += n;
      auto now = std::chrono::steady_clock::now();
      if (progress && now - last > std::chrono::milliseconds(300)) {
        progress(done, total);
        last = now;
      }
      return true;
    };
    Response res = send(r);
    fh.close();
    if (cancelled) throw Cancelled();
    if (complete) break;
    if (!werr.empty()) throw std::runtime_error(werr);
    std::string fail;
    if (res.status == 0) fail = res.error.empty() ? "error de red" : res.error;
    else if (res.status >= 400) fail = cat("HTTP ", res.status, " al descargar ", url);
    else if (total && done < total) fail = "descarga incompleta";
    if (fail.empty()) {
      if (progress) progress(done, total);
      break;
    }
    if (res.status >= 400 && res.status < 500 && res.status != 408 && res.status != 429)
      throw HttpError(res.status, fail);
    if (attempt == retries - 1) throw HttpError(res.status, fail);
    LOG.warning("reintentando descarga (", fail, ")");
    std::this_thread::sleep_for(std::chrono::seconds(2 * (attempt + 1)));
  }
  std::error_code ec;
  fs::rename(part, dest, ec);
  if (ec) {
    fs::remove(dest, ec);
    fs::rename(part, dest, ec);
    if (ec) throw std::runtime_error("no se pudo mover " + S(part));
  }
  return dest;
}

}  // namespace nx::http
