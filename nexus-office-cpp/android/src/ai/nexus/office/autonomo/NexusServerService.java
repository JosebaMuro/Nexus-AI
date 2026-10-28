package ai.nexus.office.autonomo;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.os.BatteryManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.system.Os;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Servicio en primer plano que mantiene Nexus-Office sirviendo en 127.0.0.1:PORT.
 *
 * La APK lleva un Python 3.12 para ARM64 (musl) con Nexus-Office y sus dependencias en assets/runtime.zip, y en la
 * carpeta de bibliotecas nativas el cargador de musl (libnexusld.so) y llama-server. La primera vez (y tras
 * cada actualizacion) se descomprime el runtime en los datos de la app. Android no deja ejecutar ficheros de
 * los datos de la app, pero si cargarlos: por eso se ejecuta el cargador (que esta entre las bibliotecas
 * nativas) y es el quien carga el interprete.
 *
 * Primer arranque sin cuelgues:
 *  - comprueba el espacio libre antes de empezar y lo dice claramente si falta;
 *  - descomprime en paralelo en una carpeta temporal, comprueba ficheros y bytes contra assets/runtime.json y
 *    solo entonces la da por buena (si la app se cierra a medias, la siguiente vez empieza de cero);
 *  - en cada arranque comprueba que el runtime sigue completo y, si no, lo vuelve a preparar;
 *  - vigila que Nexus-Office responda: si no lo hace en {@link #BOOT_TIMEOUT_MS} lo para y lo explica;
 *  - si Nexus-Office se cierra al arrancar, reintenta una vez (reparando el runtime si el registro indica que esta
 *    danado).
 */
public class NexusServerService extends Service {
    public static final int PORT = 8775;
    public static final String ACTION_STOP = "ai.nexus.office.autonomo.STOP";
    public static final String ACTION_REPAIR = "ai.nexus.office.autonomo.REPAIR";
    public static final String ACTION_RETRY = "ai.nexus.office.autonomo.RETRY";
    private static final String CHANNEL = "nexus";
    static final long BOOT_TIMEOUT_MS = 4 * 60 * 1000L;

    /** Estado visible para la actividad: "preparing:<pct>", "starting", "up", "stopped" o "error:<detalle>". */
    public static volatile String state = "stopped";
    /** Que esta haciendo ahora mismo (fase o ultima linea del registro de Nexus-Office), para la pantalla de arranque. */
    public static volatile String detail = "";
    private static Process process;
    private static Thread starter;
    private static volatile boolean cancel;
    private PowerManager.WakeLock wakeLock;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_STOP.equals(action)) {
            stopServer();
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        foreground();
        if (ACTION_REPAIR.equals(action) || ACTION_RETRY.equals(action)) stopServer();
        startServer(ACTION_REPAIR.equals(action));
        return START_STICKY;
    }

    private void foreground() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.channel), NotificationManager.IMPORTANCE_LOW));
        int imm = PendingIntent.FLAG_IMMUTABLE;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), imm);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, NexusServerService.class).setAction(ACTION_STOP), imm);
        Notification n = new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_menu_compass)
                .setContentTitle("Nexus-Office esta activo")
                .setContentText("Tu asistente funciona en este movil, sin conexion con el PC")
                .setContentIntent(open)
                .addAction(new Notification.Action.Builder(null, "Detener", stop).build())
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(1, n);
        }
    }

    private synchronized void startServer(final boolean repair) {
        if (process != null && process.isAlive()) return;
        if (starter != null && starter.isAlive()) {
            if (!cancel) return;               // ya esta arrancando
            try {
                starter.join(10000);           // un arranque cancelado (Reintentar): esperamos a que suelte
            } catch (InterruptedException ignored) {
            }
            if (starter.isAlive()) return;
        }
        cancel = false;
        state = "starting";
        detail = "";
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nexus:server");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire(6 * 60 * 60 * 1000L);   // limite de seguridad: 6 h sin tocar la app
        starter = new Thread(new Runnable() {
            @Override
            public void run() {
                boolean fix = repair;
                for (int attempt = 1; ; attempt++) {
                    try {
                        File runtime = ensureRuntime(fix);
                        if (cancel) return;
                        Result r = launch(runtime);
                        if (cancel || r.stopped) return;
                        if (r.everUp || attempt >= 2 || r.hung) {
                            state = "error:" + r.message;
                            return;
                        }
                        fix = looksDamaged(r.message);   // se cerro al arrancar: segundo intento, reparando si hace falta
                        detail = fix ? "Reparando el entorno de ejecucion..." : "Reintentando...";
                    } catch (Throwable t) {
                        if (cancel) return;
                        if (attempt >= 2 || t instanceof RuntimeInstaller.NoSpaceException) {
                            state = "error:" + t.getMessage();
                            return;
                        }
                        fix = true;
                        detail = "Reparando el entorno de ejecucion...";
                    }
                }
            }
        }, "nexus-start");
        starter.start();
    }

    static boolean looksDamaged(String log) {
        String l = log == null ? "" : log;
        return l.contains("ModuleNotFoundError") || l.contains("ImportError") || l.contains("No such file")
                || l.contains("bad magic") || l.contains("Error loading shared library") || l.contains("EOFError")
                || l.contains("SyntaxError") || l.contains("zipimport");
    }

    // ------------------------------------------------------------------ preparacion del runtime
    /** files/runtime-<version>-<fecha de instalacion>: una APK actualizada (aunque repita version) trae su runtime. */
    private File runtimeDir() throws Exception {
        PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
        return new File(getFilesDir(), "runtime-" + pi.getLongVersionCode() + "-" + pi.lastUpdateTime);
    }

    private File ensureRuntime(boolean force) throws Exception {
        File dir = runtimeDir();
        if (!force && RuntimeInstaller.runtimeOk(dir)) {
            linkMusl(dir);
            return dir;
        }
        state = "preparing:0";
        detail = "Comprobando el espacio libre...";
        File files = getFilesDir();
        File[] old = files.listFiles();
        if (old != null) {   // versiones viejas, restos de una preparacion interrumpida o danada
            for (File f : old) {
                if (f.getName().startsWith("runtime-") || f.getName().startsWith("runtime.zip")) RuntimeInstaller.deleteTree(f);
            }
        }
        JSONObject meta = new JSONObject(readAsset("runtime.json"));
        long bytes = meta.getLong("bytes"), zipLen = getAssets().openFd("runtime.zip").getLength();
        long need = bytes + zipLen + 64L * 1024 * 1024, free = files.getUsableSpace();
        if (free < need) {
            throw new RuntimeInstaller.NoSpaceException("falta espacio. Nexus-Office necesita " + (need >> 20) + " MB libres para prepararse y hay "
                    + (free >> 20) + " MB. Libera espacio y pulsa Reintentar.");
        }

        // 1) copia del zip (rapida, sin descomprimir) para poder leerlo con ZipFile y repartirlo entre varios hilos
        detail = "Copiando el entorno de ejecucion...";
        File zip = new File(files, "runtime.zip.tmp");
        InputStream in = getAssets().open("runtime.zip");
        OutputStream os = new FileOutputStream(zip);
        byte[] buf = new byte[1 << 17];
        long copied = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            if (cancel) break;
            os.write(buf, 0, n);
            copied += n;
            state = "preparing:" + (copied * 10 / Math.max(1, zipLen));
        }
        os.close();
        in.close();
        if (cancel) {
            zip.delete();
            throw new IOException("cancelado");
        }

        // 2) descompresion en paralelo en una carpeta temporal
        File tmp = new File(files, dir.getName() + ".partial");
        RuntimeInstaller.extract(zip, tmp, bytes, new RuntimeInstaller.Progress() {
            @Override
            public void update(int pct, String what) {
                state = "preparing:" + pct;
                if (what != null) detail = what;
            }

            @Override
            public boolean cancelled() {
                return cancel;
            }
        });
        zip.delete();

        // 3) verificacion contra runtime.json y paso a la carpeta definitiva
        detail = "Comprobando lo descomprimido...";
        long[] got = RuntimeInstaller.count(tmp);
        if (got[0] != meta.getLong("files") || got[1] != bytes) {
            RuntimeInstaller.deleteTree(tmp);
            throw new IOException("el entorno de ejecucion se ha descomprimido incompleto (" + got[0] + " de "
                    + meta.getLong("files") + " ficheros). Pulsa Reintentar.");
        }
        new File(tmp, ".completo").createNewFile();
        if (!tmp.renameTo(dir)) throw new IOException("no se pudo preparar " + dir.getName());
        linkMusl(dir);
        detail = "";
        return dir;
    }

    /** Las extensiones de los paquetes enlazan con la libc de musl por su nombre de Alpine. */
    private void linkMusl(File dir) throws Exception {
        // la edicion C++ no usa la libc de musl
    }

    private String readAsset(String name) throws IOException {
        InputStream in = getAssets().open(name);
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        in.close();
        return b.toString("UTF-8");
    }

    // ------------------------------------------------------------------ arranque y vigilancia de Nexus-Office
    private static class Result {
        boolean everUp, stopped, hung;
        String message = "";
    }

    /** La variante de llama-server mas rapida que admite esta CPU (i8mm > dotprod > ARMv8 basico). */
    private static String llamaBinary(String libs) {
        String feats = "";
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/proc/cpuinfo"));
            String l;
            while ((l = r.readLine()) != null) {
                if (l.toLowerCase().startsWith("features")) feats += " " + l.substring(l.indexOf(':') + 1) + " ";
            }
            r.close();
        } catch (IOException e) {
            // sin /proc/cpuinfo: la variante basica
        }
        List<String> names = new ArrayList<String>();
        if (feats.contains(" i8mm ") && feats.contains(" asimddp ")) names.add("libllama-server-i8mm.so");
        if (feats.contains(" asimddp ")) names.add("libllama-server-dotprod.so");
        names.add("libllama-server.so");
        for (String n : names) {
            File f = new File(libs, n);
            if (f.isFile()) return f.getAbsolutePath();
        }
        return null;
    }

    private Result launch(File runtime) throws Exception {
        state = "starting";
        detail = "Arrancando Nexus-Office...";
        String libs = getApplicationInfo().nativeLibraryDir;
        File data = getFilesDir();
        File tmp = new File(getCacheDir(), "tmp");
        tmp.mkdirs();
        // edicion C++: el servidor es un ejecutable nativo (libnexusoffice.so) y los recursos estan en el runtime
        List<String> cmd = new ArrayList<String>();
        cmd.add(new File(libs, "libnexusoffice.so").getAbsolutePath());
        cmd.add("run");
        cmd.add("--no-browser");
        cmd.add("--host");
        cmd.add("127.0.0.1");
        cmd.add("--port");
        cmd.add(String.valueOf(PORT));
        ProcessBuilder pb = new ProcessBuilder(cmd);
        Map<String, String> env = pb.environment();
        env.put("HOME", data.getAbsolutePath());
        env.put("TMPDIR", tmp.getAbsolutePath());
        env.put("LANG", "C.UTF-8");
        env.put("NEXUS_ROOT", runtime.getAbsolutePath());
        env.put("NEXUS_DATA_DIR", new File(data, "nexus").getAbsolutePath());
        env.put("NEXUS_ANDROID", "1");
        String llama = llamaBinary(libs);
        if (llama != null) {
            env.put("NEXUS_RUNTIME_LLAMA", llama);
            env.put("NEXUS_RUNTIME_LLAMA_CPU", llama);
        }
        // llama.cpp con GPU (Vulkan/OpenCL) y NPU (Hexagon): lo usa si la sonda de hardware las encuentra
        File gpu = new File(libs, "libllama-server-gpu.so");
        if (gpu.isFile()) env.put("NEXUS_RUNTIME_LLAMA_GPU", gpu.getAbsolutePath());
        env.put("NEXUS_ANDROID_LIBS", libs);
        env.put("NEXUS_DNS", dnsServers());
        env.put("NEXUS_DEVICE_STATE", new File(data, "device_state.json").getAbsolutePath());
        File info = new File(data, "device_info.json");
        writeDeviceInfo(info);
        env.put("NEXUS_DEVICE_INFO", info.getAbsolutePath());
        pb.directory(data);
        pb.redirectErrorStream(true);
        File log = new File(data, "nexus.log");
        if (log.length() > 0) log.renameTo(new File(data, "nexus.prev.log"));   // el anterior, por si hay que mirarlo
        pb.redirectOutput(log);
        final Process p = pb.start();
        process = p;
        reportDeviceState(p, new File(data, "device_state.json"));
        Result r = new Result();
        long t0 = System.currentTimeMillis();
        while (p.isAlive()) {   // vigilancia: hasta que Nexus-Office responde, o no responde a tiempo
            if (ping()) {
                r.everUp = true;
                state = "up";
                detail = "";
                break;
            }
            if (process != p) break;
            if (System.currentTimeMillis() - t0 > BOOT_TIMEOUT_MS) {
                r.hung = true;
                r.message = "Nexus-Office no ha respondido en " + (BOOT_TIMEOUT_MS / 60000) + " minutos. Ultimas lineas: " + tail(log, 500);
                p.destroy();
                if (!p.waitFor(4, TimeUnit.SECONDS)) p.destroyForcibly();
                return r;
            }
            String last = lastLine(log);
            if (last.length() > 0) detail = last;
            p.waitFor(1, TimeUnit.SECONDS);
        }
        int code = p.waitFor();   // Nexus-Office en marcha hasta que se detenga o falle
        if (process != p || "stopped".equals(state)) {
            r.stopped = true;
            return r;
        }
        r.message = (r.everUp ? "Nexus-Office se cerro" : "Nexus-Office se cerro al arrancar") + " (codigo " + code
                + (code > 128 ? ", senal " + (code - 128) : "") + "). " + tail(log, 600);
        if (r.everUp && code == 0) {
            r.stopped = true;
            state = "stopped";
        }
        return r;
    }

    private static boolean ping() {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL("http://127.0.0.1:" + PORT + "/api/ping").openConnection();
            c.setConnectTimeout(500);
            c.setReadTimeout(1500);
            int code = c.getResponseCode();
            c.disconnect();
            return code == 200;
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * Cada 30 s escribe la temperatura (estado termico de Android), la bateria y el ahorro de energia para el
     * autoajuste de Nexus-Office (nexus/android_tune.py): con el movil caliente o sin bateria usa menos hilos.
     */
    /** SoC y GPU del movil para que Nexus-Office elija CPU/GPU/NPU (nexus/hardware/android_accel.py). La GPU se nombra con
     *  EGL (un contexto de 1x1 fuera de pantalla) y la version de Vulkan sale de las caracteristicas del sistema. */
    private void writeDeviceInfo(File out) {
        JSONObject j = new JSONObject();
        try {
            j.put("manufacturer", Build.MANUFACTURER);
            j.put("model", Build.MODEL);
            j.put("hardware", Build.HARDWARE);
            j.put("board", Build.BOARD);
            j.put("sdk", Build.VERSION.SDK_INT);
            if (Build.VERSION.SDK_INT >= 31) {
                j.put("soc_manufacturer", Build.SOC_MANUFACTURER);
                j.put("soc_model", Build.SOC_MODEL);
            }
            for (FeatureInfo f : getPackageManager().getSystemAvailableFeatures()) {
                if (PackageManager.FEATURE_VULKAN_HARDWARE_VERSION.equals(f.name))
                    j.put("vulkan_version", (f.version >> 22) + "." + ((f.version >> 12) & 0x3ff));
                if (PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL.equals(f.name)) j.put("vulkan_level", f.version);
            }
        } catch (Exception ignored) {
        }
        EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
        try {
            dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            int[] ver = new int[2];
            if (EGL14.eglInitialize(dpy, ver, 0, ver, 1)) {
                EGLConfig[] cfg = new EGLConfig[1];
                int[] n = new int[1];
                int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                        EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
                if (EGL14.eglChooseConfig(dpy, attrs, 0, cfg, 0, 1, n, 0) && n[0] > 0) {
                    EGLContext ctx = EGL14.eglCreateContext(dpy, cfg[0], EGL14.EGL_NO_CONTEXT,
                            new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                    EGLSurface surf = EGL14.eglCreatePbufferSurface(dpy, cfg[0],
                            new int[]{EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
                    if (EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) {
                        j.put("gl_renderer", GLES20.glGetString(GLES20.GL_RENDERER));
                        j.put("gl_vendor", GLES20.glGetString(GLES20.GL_VENDOR));
                        j.put("gl_version", GLES20.glGetString(GLES20.GL_VERSION));
                        EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    }
                    EGL14.eglDestroySurface(dpy, surf);
                    EGL14.eglDestroyContext(dpy, ctx);
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (dpy != EGL14.EGL_NO_DISPLAY) EGL14.eglTerminate(dpy);
        }
        try {
            OutputStream os = new FileOutputStream(out);
            os.write(j.toString().getBytes("UTF-8"));
            os.close();
        } catch (IOException ignored) {
        }
    }

    private void reportDeviceState(final Process p, final File out) {
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
                File tmp = new File(out.getPath() + ".tmp");
                while (p.isAlive()) {
                    try {
                        Intent b = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                        int level = b == null ? -1 : b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                        int scale = b == null ? 100 : b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                        int plugged = b == null ? 0 : b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
                        int thermal = Build.VERSION.SDK_INT >= 29 ? pm.getCurrentThermalStatus() : 0;
                        String json = "{\"t\": " + System.currentTimeMillis()
                                + ", \"thermal\": " + thermal
                                + ", \"battery\": " + (level < 0 ? "null" : String.valueOf(level * 100 / Math.max(1, scale)))
                                + ", \"charging\": " + (plugged != 0)
                                + ", \"power_save\": " + pm.isPowerSaveMode() + "}";
                        OutputStream os = new FileOutputStream(tmp);
                        os.write(json.getBytes("UTF-8"));
                        os.close();
                        tmp.renameTo(out);
                    } catch (Exception ignored) {
                    }
                    try {
                        Thread.sleep(30000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }, "nexus-estado");
        t.setDaemon(true);
        t.start();
    }

    /** Servidores DNS de la red activa: el Python de la app no puede preguntar al DNS del sistema. */
    private String dnsServers() {
        StringBuilder sb = new StringBuilder();
        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            Network net = cm.getActiveNetwork();
            LinkProperties lp = net == null ? null : cm.getLinkProperties(net);
            if (lp != null) {
                for (InetAddress a : lp.getDnsServers()) {
                    if (sb.length() > 0) sb.append(',');
                    sb.append(a.getHostAddress());
                }
            }
        } catch (Exception ignored) {
        }
        return sb.toString();
    }

    static String tail(File f, int max) {
        try {
            RandomAccessFile r = new RandomAccessFile(f, "r");
            long len = r.length();
            r.seek(Math.max(0, len - max));
            byte[] b = new byte[(int) (len - r.getFilePointer())];
            r.readFully(b);
            r.close();
            return new String(b, "UTF-8").trim();
        } catch (IOException e) {
            return "";
        }
    }

    /** Ultima linea del registro, sin la hora ni el nivel, para la pantalla de arranque. */
    static String lastLine(File f) {
        String t = tail(f, 300);
        int nl = t.lastIndexOf('\n');
        String line = (nl >= 0 ? t.substring(nl + 1) : t).trim();
        line = line.replaceFirst("^\\d\\d:\\d\\d:\\d\\d\\s+[A-Z]+\\s+", "");
        return line.length() > 140 ? line.substring(0, 140) + "..." : line;
    }

    private void stopServer() {
        cancel = true;
        state = "stopped";
        final Process p = process;
        process = null;
        if (p != null) {
            p.destroy();   // SIGTERM: Nexus-Office cierra limpiamente (y con el, llama-server)
            try {
                if (!p.waitFor(4, TimeUnit.SECONDS)) p.destroyForcibly();
            } catch (InterruptedException ignored) {
            }
        }
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
    }

    @Override
    public void onDestroy() {
        stopServer();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
