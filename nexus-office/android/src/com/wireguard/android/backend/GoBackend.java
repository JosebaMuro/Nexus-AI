package com.wireguard.android.backend;

/**
 * Enlace minimo con libwg-go.so (wireguard-go para Android, del paquete oficial com.wireguard.android:tunnel,
 * licencia Apache 2.0). Los nombres de clase y metodo deben ser exactamente estos: la biblioteca nativa
 * exporta Java_com_wireguard_android_backend_GoBackend_*.
 */
public final class GoBackend {
    private static boolean loaded;

    private GoBackend() {
    }

    public static synchronized void load() {
        if (!loaded) {
            System.loadLibrary("wg-go");
            loaded = true;
        }
    }

    /** Arranca el tunel sobre el descriptor tun (Go se queda con el). Devuelve un manejador >= 0 o un error < 0. */
    public static native int wgTurnOn(String ifName, int tunFd, String settings);

    public static native void wgTurnOff(int handle);

    /** Sockets UDP del tunel, para excluirlos de la propia VPN con VpnService.protect(). */
    public static native int wgGetSocketV4(int handle);

    public static native int wgGetSocketV6(int handle);

    /** Estado en formato UAPI de WireGuard (last_handshake_time_sec, rx_bytes, tx_bytes...). */
    public static native String wgGetConfig(int handle);

    public static native String wgVersion();
}
