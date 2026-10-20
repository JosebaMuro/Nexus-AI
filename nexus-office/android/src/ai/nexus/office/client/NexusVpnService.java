package ai.nexus.office.client;

import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.VpnService;
import android.os.ParcelFileDescriptor;

import com.wireguard.android.backend.GoBackend;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * Tunel WireGuard de la app hacia el PC con Nexus-Office (VpnService + wireguard-go).
 * Solo enruta la direccion del PC dentro del tunel y solo para esta app: el resto del movil no se toca.
 * Prueba las direcciones del PC (la ultima que funciono primero) hasta que hay negociacion.
 */
public class NexusVpnService extends VpnService {
    public static final String ACTION_CONNECT = "ai.nexus.office.client.VPN_CONNECT";
    public static final String ACTION_STOP = "ai.nexus.office.client.VPN_STOP";
    private static final long HANDSHAKE_WAIT_MS = 7000;

    public interface Listener {
        void onVpnState(String state, String detail);
    }

    static volatile Listener listener;
    static volatile String state = "off";       // off | connecting | up | error
    static volatile String detail = "";
    static volatile String endpoint = "";
    private static volatile int handle = -1;
    private static NexusVpnService instance;
    private Thread worker;

    static String stats() {
        int h = handle;
        if (h < 0) return "";
        try {
            String cfg = GoBackend.wgGetConfig(h);
            return "rx=" + WgConfig.counter(cfg, "rx_bytes") + " tx=" + WgConfig.counter(cfg, "tx_bytes")
                    + " handshake=" + WgConfig.lastHandshake(cfg);
        } catch (Throwable t) {
            return "";
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            disconnect();
            report("off", "");
            stopSelf();
            return START_NOT_STICKY;
        }
        final SharedPreferences prefs = getSharedPreferences("nexus", MODE_PRIVATE);
        final String code = prefs.getString("vpn_code", "");
        if (code.isEmpty()) {
            stopSelf();
            return START_NOT_STICKY;
        }
        if (worker != null && worker.isAlive()) return START_STICKY;
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    connect(WgConfig.parse(code), prefs);
                } catch (Throwable t) {
                    report("error", "Configuracion VPN no valida: " + t.getMessage());
                }
            }
        }, "nexus-vpn");
        worker.start();
        return START_STICKY;    // si Android cierra el servicio, lo vuelve a levantar con la misma configuracion
    }

    private void connect(WgConfig cfg, SharedPreferences prefs) {
        disconnect();
        try {
            GoBackend.load();
        } catch (Throwable t) {
            report("error", "Este movil no puede cargar WireGuard (" + t.getMessage() + ")");
            return;
        }
        List<String> eps = new ArrayList<String>(cfg.endpoints);
        String last = prefs.getString("vpn_last_ep", "");
        if (eps.remove(last)) eps.add(0, last);
        String why = "";
        for (String ep : eps) {
            report("connecting", ep);
            String[] hp = WgConfig.hostPort(ep);
            List<InetAddress> addrs = new ArrayList<InetAddress>();
            try {
                for (InetAddress a : InetAddress.getAllByName(hp[0])) {
                    if (a instanceof Inet4Address) addrs.add(0, a);
                    else addrs.add(a);
                }
            } catch (Exception e) {
                why = "no se encuentra " + hp[0];
                continue;
            }
            for (InetAddress a : addrs) {
                String resolved = WgConfig.formatEndpoint(a, hp[1]);
                int h = start(cfg, resolved);
                if (h < 0) {
                    why = h == -9999 ? "permiso de VPN retirado" : "WireGuard no pudo arrancar (" + h + ")";
                    if (h == -9999) {
                        report("error", why);
                        return;
                    }
                    continue;
                }
                if (waitHandshake(h, cfg)) {
                    handle = h;
                    endpoint = ep;
                    prefs.edit().putString("vpn_last_ep", ep).apply();
                    report("up", ep);
                    return;
                }
                GoBackend.wgTurnOff(h);
                why = "el PC no responde en " + ep;
            }
        }
        report("error", "No se pudo conectar por la VPN: " + why + ". Comprueba que el tunel esta activo en el PC "
                + "(Red COOP > VPN) y que el puerto UDP esta abierto en el router si estas fuera de casa.");
    }

    /** Crea la interfaz tun y arranca wireguard-go. -9999 si el usuario retiro el permiso de VPN. */
    private int start(WgConfig cfg, String resolvedEndpoint) {
        Builder b = new Builder();
        b.setSession("Nexus-Office VPN");
        b.addAddress(cfg.addressIp(), cfg.addressPrefix());
        b.addRoute(cfg.routeIp(), cfg.routePrefix());
        b.setMtu(1280);
        b.setBlocking(true);
        try {
            b.addAllowedApplication(getPackageName());     // solo el trafico de esta app entra en el tunel
        } catch (Exception ignored) {
        }
        Intent open = new Intent(this, MainActivity.class);
        b.setConfigureIntent(PendingIntent.getActivity(this, 0, open, 0x04000000 /* FLAG_IMMUTABLE */));
        ParcelFileDescriptor tun;
        try {
            tun = b.establish();
        } catch (Exception e) {
            return -9998;
        }
        if (tun == null) return -9999;
        int h = GoBackend.wgTurnOn("nexus", tun.detachFd(), cfg.uapi(resolvedEndpoint));
        if (h >= 0) {
            protect(GoBackend.wgGetSocketV4(h));
            protect(GoBackend.wgGetSocketV6(h));
        }
        return h;
    }

    private boolean waitHandshake(int h, final WgConfig cfg) {
        long end = System.currentTimeMillis() + HANDSHAKE_WAIT_MS;
        int tick = 0;
        while (System.currentTimeMillis() < end) {
            if (WgConfig.lastHandshake(GoBackend.wgGetConfig(h)) > 0) return true;
            if (tick++ % 4 == 0) {   // un poco de trafico hacia el PC fuerza la negociacion
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try (Socket s = new Socket()) {
                            s.connect(new InetSocketAddress(cfg.routeIp(), portOf(cfg.url)), 1500);
                        } catch (Exception ignored) {
                        }
                    }
                }).start();
            }
            try {
                Thread.sleep(250);
            } catch (InterruptedException e) {
                return false;
            }
        }
        return WgConfig.lastHandshake(GoBackend.wgGetConfig(h)) > 0;
    }

    static int portOf(String url) {
        try {
            int p = new java.net.URL(url).getPort();
            return p > 0 ? p : 80;
        } catch (Exception e) {
            return 8775;
        }
    }

    private static void disconnect() {
        int h = handle;
        handle = -1;
        if (h >= 0) {
            try {
                GoBackend.wgTurnOff(h);
            } catch (Throwable ignored) {
            }
        }
    }

    private static void report(String s, String d) {
        state = s;
        detail = d;
        Listener l = listener;
        if (l != null) l.onVpnState(s, d);
    }

    static boolean running() {
        return handle >= 0;
    }

    static void stop() {
        NexusVpnService svc = instance;
        disconnect();
        report("off", "");
        if (svc != null) svc.stopSelf();
    }

    @Override
    public void onRevoke() {
        disconnect();
        report("off", "Permiso de VPN retirado");
        super.onRevoke();
    }

    @Override
    public void onDestroy() {
        disconnect();
        instance = null;
        super.onDestroy();
    }
}
