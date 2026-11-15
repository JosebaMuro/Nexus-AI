package ai.nexus.office.autonomo;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;

/**
 * GPS del movil para la ubicacion en tiempo real (sin internet): escribe la ultima posicion en
 * files/nexus/ubicacion.json, que lee el nucleo de Agente IA (nexus/devices/ubicacion.py, fuente «app»).
 * La usa la pantalla mientras esta abierta y el servicio en segundo plano si Android lo permite (permiso
 * «Permitir siempre»). Ademas del GPS escucha el proveedor pasivo: las posiciones que ya piden otras apps.
 */
final class GpsMovil {
    private static Location ultima;   // compartida entre la pantalla y el servicio
    private final Context ctx;
    private LocationManager mgr;
    private LocationListener listener;

    GpsMovil(Context ctx) {
        this.ctx = ctx.getApplicationContext();
    }

    static boolean permiso(Context c) {
        return Build.VERSION.SDK_INT < 23
                || c.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
                || c.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    /** Permiso de ubicacion en segundo plano (Android 10+): sin el, Android corta el GPS al salir de la app. */
    static boolean permisoFondo(Context c) {
        if (!permiso(c)) return false;
        return Build.VERSION.SDK_INT < 29
                || c.checkSelfPermission(Manifest.permission.ACCESS_BACKGROUND_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    /** El usuario lo activo en Ajustes > Ubicacion (se recuerda entre arranques). */
    static boolean activa(Context c) {
        return c.getSharedPreferences("nexus", Context.MODE_PRIVATE).getBoolean("gps_movil", false);
    }

    static void recordar(Context c, boolean on) {
        c.getSharedPreferences("nexus", Context.MODE_PRIVATE).edit().putBoolean("gps_movil", on).apply();
    }

    boolean escuchando() {
        return listener != null;
    }

    void iniciar() {
        if (listener != null || !permiso(ctx)) return;
        mgr = (LocationManager) ctx.getSystemService(Context.LOCATION_SERVICE);
        if (mgr == null) return;
        listener = new LocationListener() {
            @Override
            public void onLocationChanged(Location l) {
                guardar(l);
            }

            @Override
            public void onStatusChanged(String p, int s, Bundle b) {
            }

            @Override
            public void onProviderEnabled(String p) {
            }

            @Override
            public void onProviderDisabled(String p) {
            }
        };
        try {
            for (String p : new String[]{LocationManager.GPS_PROVIDER, LocationManager.PASSIVE_PROVIDER}) {
                if (!mgr.getAllProviders().contains(p)) continue;
                mgr.requestLocationUpdates(p, 3000L, 2f, listener, Looper.getMainLooper());
            }
            for (String p : mgr.getAllProviders()) {
                Location l = mgr.getLastKnownLocation(p);
                if (l != null) guardar(l);
            }
        } catch (SecurityException | IllegalArgumentException e) {
            parar();
        }
    }

    void parar() {
        if (mgr != null && listener != null) {
            try {
                mgr.removeUpdates(listener);
            } catch (SecurityException ignored) {
            }
        }
        listener = null;
    }

    private void guardar(Location l) {
        synchronized (GpsMovil.class) {
            // una posicion vieja o mucho peor que la que ya hay no sustituye a la buena
            Location u = ultima;
            if (u != null) {
                long dt = l.getTime() - u.getTime();
                if (dt < 0) return;
                if (dt < 30000 && l.hasAccuracy() && u.hasAccuracy() && l.getAccuracy() > u.getAccuracy() * 3) return;
            }
            ultima = l;
        }
        final JSONObject o = new JSONObject();
        try {
            o.put("lat", l.getLatitude());
            o.put("lon", l.getLongitude());
            if (l.hasAccuracy()) o.put("precision", (double) l.getAccuracy());
            if (l.hasAltitude()) o.put("altitud", l.getAltitude());
            if (l.hasSpeed()) o.put("velocidad", (double) l.getSpeed());
            if (l.hasBearing()) o.put("rumbo", (double) l.getBearing());
            o.put("proveedor", l.getProvider() == null ? "" : l.getProvider());
            o.put("t", l.getTime());
        } catch (Exception e) {
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                File dir = new File(ctx.getFilesDir(), "nexus");
                dir.mkdirs();
                File dst = new File(dir, "ubicacion.json"), tmp = new File(dir, "ubicacion.json.tmp");
                try (FileOutputStream out = new FileOutputStream(tmp)) {
                    out.write(o.toString().getBytes("UTF-8"));
                } catch (Exception e) {
                    return;
                }
                if (!tmp.renameTo(dst)) tmp.delete();
            }
        }).start();
    }
}
