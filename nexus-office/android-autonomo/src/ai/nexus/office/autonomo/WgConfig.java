package ai.nexus.office.autonomo;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.UnsupportedEncodingException;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * Configuracion de la VPN de Nexus-Office (codigo NEXUSV1:... que genera el PC) y su traduccion al formato
 * UAPI de WireGuard que entiende libwg-go. Java puro (sin clases de Android) para poder probarlo en el PC.
 */
public final class WgConfig {
    public static final String PREFIX = "NEXUSV1:";

    public final String name;
    public final String address;      // 10.78.0.2/32 (direccion del movil dentro del tunel)
    public final String privateKey;   // base64, clave privada del movil
    public final String serverKey;    // base64, clave publica del PC
    public final String psk;          // base64, clave precompartida
    public final String route;        // 10.78.0.1/32 (solo el PC va por el tunel)
    public final String url;          // http://10.78.0.1:8775
    public final String lanKey;       // clave de red de Nexus-Office
    public final List<String> endpoints;

    public WgConfig(String name, String address, String privateKey, String serverKey, String psk, String route,
                    String url, String lanKey, List<String> endpoints) {
        this.name = name;
        this.address = address;
        this.privateKey = privateKey;
        this.serverKey = serverKey;
        this.psk = psk;
        this.route = route;
        this.url = url;
        this.lanKey = lanKey;
        this.endpoints = Collections.unmodifiableList(new ArrayList<String>(endpoints));
    }

    /** Lee un codigo NEXUSV1:... (lanza excepcion con un mensaje para el usuario si no es valido). */
    public static WgConfig parse(String code) throws Exception {
        String c = code == null ? "" : code.replaceAll("\\s+", "");
        if (!c.startsWith(PREFIX)) throw new IllegalArgumentException("El codigo VPN empieza por NEXUSV1:");
        JSONObject o = new JSONObject(new String(base64(c.substring(PREFIX.length())), "UTF-8"));
        if (o.optInt("v", 0) != 1) throw new IllegalArgumentException("Version de codigo no soportada: actualiza la app");
        JSONArray e = o.getJSONArray("e");
        List<String> eps = new ArrayList<String>();
        for (int i = 0; i < e.length(); i++) eps.add(e.getString(i));
        if (eps.isEmpty()) throw new IllegalArgumentException("El codigo no trae ninguna direccion del PC");
        WgConfig w = new WgConfig(o.optString("n", "Nexus-Office"), o.getString("a"), o.getString("k"), o.getString("s"),
                o.optString("p", ""), o.optString("r", "10.78.0.1/32"), o.getString("u"), o.optString("key", ""), eps);
        if (base64(w.privateKey).length != 32 || base64(w.serverKey).length != 32) {
            throw new IllegalArgumentException("Claves incompletas: genera otro codigo en tu Nexus-Office");
        }
        return w;
    }

    public String toCode() {
        try {
            JSONObject o = new JSONObject().put("v", 1).put("n", name).put("a", address).put("k", privateKey).put("s", serverKey)
                    .put("p", psk).put("r", route).put("u", url).put("key", lanKey).put("e", new JSONArray(endpoints));
            return PREFIX + base64UrlEncode(o.toString().getBytes("UTF-8"));
        } catch (Exception ex) {
            throw new IllegalStateException(ex);
        }
    }

    /** Configuracion UAPI para wgTurnOn con el endpoint ya resuelto a IP ("1.2.3.4:51820" o "[::1]:51820"). */
    public String uapi(String resolvedEndpoint) {
        StringBuilder sb = new StringBuilder();
        sb.append("private_key=").append(hex(privateKey)).append('\n');
        sb.append("replace_peers=true\n");
        sb.append("public_key=").append(hex(serverKey)).append('\n');
        if (psk != null && !psk.isEmpty()) sb.append("preshared_key=").append(hex(psk)).append('\n');
        sb.append("endpoint=").append(resolvedEndpoint).append('\n');
        sb.append("persistent_keepalive_interval=25\n");
        sb.append("replace_allowed_ips=true\n");
        sb.append("allowed_ip=").append(route).append('\n');
        return sb.toString();
    }

    public String targetUrl() {
        String base = url.endsWith("/") ? url : url + "/";
        if (lanKey == null || lanKey.isEmpty()) return base;
        try {
            return base + "?key=" + URLEncoder.encode(lanKey, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            return base;
        }
    }

    public String addressIp() {
        return address.split("/")[0];
    }

    public int addressPrefix() {
        return prefix(address, 32);
    }

    public String routeIp() {
        return route.split("/")[0];
    }

    public int routePrefix() {
        return prefix(route, 32);
    }

    private static int prefix(String cidr, int def) {
        int i = cidr.indexOf('/');
        return i < 0 ? def : Integer.parseInt(cidr.substring(i + 1));
    }

    /** {"host", "puerto"} de "host:puerto", "[v6]:puerto" o "host" (51820 por defecto). */
    public static String[] hostPort(String endpoint) {
        String e = endpoint.trim();
        if (e.startsWith("[")) {
            int end = e.indexOf(']');
            String host = e.substring(1, end);
            String port = e.length() > end + 2 ? e.substring(end + 2) : "51820";
            return new String[]{host, port};
        }
        int i = e.lastIndexOf(':');
        if (i > 0 && e.indexOf(':') == i) return new String[]{e.substring(0, i), e.substring(i + 1)};
        if (i > 0) return new String[]{e, "51820"};   // IPv6 sin corchetes y sin puerto
        return new String[]{e, "51820"};
    }

    public static String formatEndpoint(InetAddress a, String port) {
        String h = a.getHostAddress();
        int pct = h.indexOf('%');
        if (pct >= 0) h = h.substring(0, pct);
        return (a instanceof Inet6Address ? "[" + h + "]" : h) + ":" + port;
    }

    /** Segundos de la ultima negociacion con el PC (0 = todavia ninguna) a partir de la salida de wgGetConfig. */
    public static long lastHandshake(String uapiGet) {
        if (uapiGet == null) return 0;
        for (String line : uapiGet.split("\n")) {
            if (line.startsWith("last_handshake_time_sec=")) {
                try {
                    return Long.parseLong(line.substring("last_handshake_time_sec=".length()).trim());
                } catch (NumberFormatException e) {
                    return 0;
                }
            }
        }
        return 0;
    }

    public static long counter(String uapiGet, String key) {
        long total = 0;
        if (uapiGet == null) return 0;
        for (String line : uapiGet.split("\n")) {
            if (line.startsWith(key + "=")) {
                try {
                    total += Long.parseLong(line.substring(key.length() + 1).trim());
                } catch (NumberFormatException ignored) {
                }
            }
        }
        return total;
    }

    // ------------------------------------------------------------------ base64 / hex sin dependencias (API 24)
    static String hex(String b64) {
        byte[] b = base64(b64);
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) sb.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return sb.toString();
    }

    static byte[] base64(String s) {
        String t = s.trim().replace('-', '+').replace('_', '/');
        while (t.endsWith("=")) t = t.substring(0, t.length() - 1);
        int[] val = new int[128];
        java.util.Arrays.fill(val, -1);
        String abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < abc.length(); i++) val[abc.charAt(i)] = i;
        byte[] out = new byte[t.length() * 3 / 4];
        int buf = 0, bits = 0, n = 0;
        for (int i = 0; i < t.length(); i++) {
            char ch = t.charAt(i);
            int v = ch < 128 ? val[ch] : -1;
            if (v < 0) throw new IllegalArgumentException("base64 no valido");
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[n++] = (byte) ((buf >> bits) & 0xff);
            }
        }
        if (n == out.length) return out;
        byte[] r = new byte[n];
        System.arraycopy(out, 0, r, 0, n);
        return r;
    }

    static String base64UrlEncode(byte[] data) {
        String abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        StringBuilder sb = new StringBuilder();
        int i = 0;
        for (; i + 2 < data.length; i += 3) {
            int v = ((data[i] & 0xff) << 16) | ((data[i + 1] & 0xff) << 8) | (data[i + 2] & 0xff);
            sb.append(abc.charAt(v >> 18)).append(abc.charAt((v >> 12) & 63)).append(abc.charAt((v >> 6) & 63)).append(abc.charAt(v & 63));
        }
        int rest = data.length - i;
        if (rest == 1) {
            int v = (data[i] & 0xff) << 16;
            sb.append(abc.charAt(v >> 18)).append(abc.charAt((v >> 12) & 63));
        } else if (rest == 2) {
            int v = ((data[i] & 0xff) << 16) | ((data[i + 1] & 0xff) << 8);
            sb.append(abc.charAt(v >> 18)).append(abc.charAt((v >> 12) & 63)).append(abc.charAt((v >> 6) & 63));
        }
        return sb.toString();
    }
}
