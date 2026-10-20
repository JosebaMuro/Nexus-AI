package ai.nexus.office.autonomo;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

/** Arranque al encender el movil (si el usuario lo activo en Sistema > Arranque automatico). */
public class BootReceiver extends BroadcastReceiver {
    static final String PREFS = "servicio";
    static final String KEY = "arrancar_al_encender";

    @Override
    public void onReceive(Context ctx, Intent intent) {
        String a = intent == null ? null : intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(a) && !"android.intent.action.LOCKED_BOOT_COMPLETED".equals(a)
                && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) return;
        if (!ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY, false)) return;
        Intent i = new Intent(ctx, NexusServerService.class);
        try {
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i);
            else ctx.startService(i);
        } catch (Exception ignored) {
            // el sistema puede negarlo (ahorro de bateria extremo): Nexus-Office arrancara al abrir la app
        }
    }
}
