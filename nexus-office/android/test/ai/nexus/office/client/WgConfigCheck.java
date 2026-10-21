package ai.nexus.office.client;

import java.net.InetAddress;

/**
 * Comprobacion de WgConfig en el PC (sin Android): java WgConfigCheck NEXUSV1:... -> imprime la configuracion UAPI
 * que la app pasaria a wireguard-go para la primera direccion del PC, y la URL que abriria.
 * La usan tests/test_android_vpn.py y packaging/test_vpn_e2e.sh.
 */
public final class WgConfigCheck {
    public static void main(String[] args) throws Exception {
        WgConfig c = WgConfig.parse(args[0]);
        WgConfig again = WgConfig.parse(c.toCode());
        check(again.privateKey.equals(c.privateKey) && again.endpoints.equals(c.endpoints), "toCode/parse");
        check(WgConfig.hex(c.privateKey).length() == 64, "clave privada de 32 bytes");
        check(WgConfig.lastHandshake("public_key=ab\nlast_handshake_time_sec=1712\nrx_bytes=1\n") == 1712, "handshake");
        check(WgConfig.hostPort("[2001:db8::1]:51820")[0].equals("2001:db8::1"), "ipv6");
        check(WgConfig.hostPort("casa.example.org")[1].equals("51820"), "puerto por defecto");
        String[] hp = WgConfig.hostPort(c.endpoints.get(0));
        String resolved = WgConfig.formatEndpoint(InetAddress.getByName(hp[0]), hp[1]);
        System.out.print(c.uapi(resolved));
        System.out.println("#url=" + c.targetUrl());
        System.out.println("#address=" + c.addressIp() + "/" + c.addressPrefix());
        System.out.println("#route=" + c.routeIp() + "/" + c.routePrefix());
    }

    private static void check(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }
}
