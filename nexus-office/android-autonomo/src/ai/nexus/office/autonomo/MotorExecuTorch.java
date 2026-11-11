package ai.nexus.office.autonomo;

import android.util.Log;
import org.json.JSONObject;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.security.SecureRandom;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Puente de ExecuTorch para Agente IA (Python). ExecuTorch solo tiene API Java en Android (LlmModule, con XNNPACK en
 * CPU y el delegado QNN para la NPU Hexagon), y el Python de la app no puede usarla: este servidor minimo en
 * 127.0.0.1 la sirve. Cada peticion lleva una clave aleatoria (cabecera X-Nexus-Clave) que solo conoce el proceso de
 * Python, para que otras apps del movil no puedan usarlo.
 *
 *   POST /executorch/cargar   {"modelo": ".pte", "tokenizer": "...", "temperatura": 0.7}
 *   POST /executorch/generar  {"prompt": "...", "max_tokens": N, "temperatura": T, "ctx": N}  -> texto por trozos
 *   POST /executorch/parar
 *   POST /executorch/cerrar
 *   GET  /estado
 *
 * Las clases de ExecuTorch se usan por reflexion: la APK compila igual sin ellas (entonces /estado lo dice).
 */
final class MotorExecuTorch {
    private static final String TAG = "NexusExecuTorch";
    private static final String LLM = "org.pytorch.executorch.extension.llm.LlmModule";
    private static final String CALLBACK = "org.pytorch.executorch.extension.llm.LlmCallback";

    private final String clave;
    private ServerSocket server;
    private Object modulo;
    private String cargado = "";
    private final Object lock = new Object();
    private static boolean nativo;

    MotorExecuTorch() {
        byte[] b = new byte[24];
        new SecureRandom().nextBytes(b);
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format(Locale.ROOT, "%02x", x));
        clave = sb.toString();
    }

    String clave() {
        return clave;
    }

    /** Arranca el servidor y devuelve su URL (http://127.0.0.1:puerto). */
    synchronized String arrancar() throws Exception {
        if (server != null && !server.isClosed()) return url();
        server = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                while (server != null && !server.isClosed()) {
                    try {
                        final Socket s = server.accept();
                        Thread h = new Thread(new Runnable() {
                            @Override
                            public void run() {
                                atender(s);
                            }
                        }, "executorch-peticion");
                        h.setDaemon(true);
                        h.start();
                    } catch (Exception e) {
                        if (server == null || server.isClosed()) return;
                    }
                }
            }
        }, "executorch-puente");
        t.setDaemon(true);
        t.start();
        return url();
    }

    String url() {
        return "http://127.0.0.1:" + server.getLocalPort();
    }

    synchronized void parar() {
        try {
            if (server != null) server.close();
        } catch (Exception ignored) {
        }
        server = null;
        cerrarModulo();
    }

    static boolean incluido() {
        try {
            Class.forName(LLM);
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    // ------------------------------------------------------------------ HTTP minimo
    private void atender(Socket s) {
        try {
            s.setSoTimeout(0);
            InputStream in = s.getInputStream();
            OutputStream out = s.getOutputStream();
            String linea = leerLinea(in);
            if (linea == null) return;
            String[] partes = linea.split(" ");
            String metodo = partes[0], ruta = partes.length > 1 ? partes[1] : "/";
            Map<String, String> cab = new HashMap<String, String>();
            String l;
            while ((l = leerLinea(in)) != null && !l.isEmpty()) {
                int i = l.indexOf(':');
                if (i > 0) cab.put(l.substring(0, i).trim().toLowerCase(Locale.ROOT), l.substring(i + 1).trim());
            }
            int n = 0;
            try {
                n = Integer.parseInt(cab.containsKey("content-length") ? cab.get("content-length") : "0");
            } catch (NumberFormatException ignored) {
            }
            byte[] cuerpo = new byte[Math.max(0, n)];
            int leido = 0;
            while (leido < n) {
                int r = in.read(cuerpo, leido, n - leido);
                if (r < 0) break;
                leido += r;
            }
            if (!clave.equals(cab.get("x-nexus-clave"))) {
                responder(out, 403, "clave incorrecta");
                return;
            }
            JSONObject j = n > 0 ? new JSONObject(new String(cuerpo, "UTF-8")) : new JSONObject();
            if (ruta.equals("/estado")) {
                JSONObject e = new JSONObject();
                e.put("incluido", incluido());
                e.put("cargado", cargado);
                responder(out, 200, e.toString());
            } else if (ruta.equals("/executorch/cargar") && metodo.equals("POST")) {
                try {
                    cargar(j.getString("modelo"), j.getString("tokenizer"), (float) j.optDouble("temperatura", 0.7));
                    responder(out, 200, "{\"ok\":true}");
                } catch (Throwable t) {
                    Log.w(TAG, "no carga", t);
                    responder(out, 500, causa(t));
                }
            } else if (ruta.equals("/executorch/generar") && metodo.equals("POST")) {
                generar(out, j.getString("prompt"), j.optInt("max_tokens", 1024), j.optInt("ctx", 4096),
                        (float) j.optDouble("temperatura", 0.7));
            } else if (ruta.equals("/executorch/parar")) {
                Object m = modulo;
                if (m != null) invocar(m, "stop");
                responder(out, 200, "{\"ok\":true}");
            } else if (ruta.equals("/executorch/cerrar")) {
                cerrarModulo();
                responder(out, 200, "{\"ok\":true}");
            } else {
                responder(out, 404, "no existe");
            }
        } catch (Throwable t) {
            Log.w(TAG, "peticion", t);
        } finally {
            try {
                s.close();
            } catch (Exception ignored) {
            }
        }
    }

    private static String leerLinea(InputStream in) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') break;
            if (c != '\r') b.write(c);
        }
        if (c < 0 && b.size() == 0) return null;
        return b.toString("UTF-8");
    }

    private static void responder(OutputStream out, int codigo, String cuerpo) throws Exception {
        byte[] b = cuerpo.getBytes("UTF-8");
        String cab = "HTTP/1.1 " + codigo + " " + (codigo == 200 ? "OK" : "Error") + "\r\nContent-Type: "
                + (cuerpo.startsWith("{") ? "application/json" : "text/plain; charset=utf-8")
                + "\r\nContent-Length: " + b.length + "\r\nConnection: close\r\n\r\n";
        out.write(cab.getBytes("UTF-8"));
        out.write(b);
        out.flush();
    }

    private static void trozo(OutputStream out, String texto) throws Exception {
        byte[] b = texto.getBytes("UTF-8");
        out.write((Integer.toHexString(b.length) + "\r\n").getBytes("UTF-8"));
        out.write(b);
        out.write("\r\n".getBytes("UTF-8"));
        out.flush();
    }

    private static String causa(Throwable t) {
        while (t.getCause() != null) t = t.getCause();
        return t.getClass().getSimpleName() + ": " + t.getMessage();
    }

    // ------------------------------------------------------------------ ExecuTorch por reflexion
    private static synchronized void cargarNativo() throws Exception {
        if (nativo) return;
        // fbjni carga sus bibliotecas con el NativeLoader de SoLoader: hay que iniciarlo con el cargador del sistema
        try {
            Class<?> nl = Class.forName("com.facebook.soloader.nativeloader.NativeLoader");
            Boolean ya = (Boolean) nl.getMethod("isInitialized").invoke(null);
            if (!ya) {
                Class<?> sd = Class.forName("com.facebook.soloader.nativeloader.SystemDelegate");
                Class<?> del = Class.forName("com.facebook.soloader.nativeloader.NativeLoaderDelegate");
                nl.getMethod("init", del).invoke(null, sd.getConstructor().newInstance());
            }
        } catch (ClassNotFoundException ignored) {
        }
        System.loadLibrary("fbjni");
        System.loadLibrary("executorch");
        nativo = true;
    }

    private void cargar(String pte, String tokenizer, float temperatura) throws Exception {
        synchronized (lock) {
            if (pte.equals(cargado) && modulo != null) return;
            cerrarModulo();
            if (!incluido()) throw new IllegalStateException("esta APK no trae ExecuTorch");
            cargarNativo();
            Class<?> c = Class.forName(LLM);
            Object m = c.getConstructor(String.class, String.class, float.class).newInstance(pte, tokenizer, temperatura);
            Object r = invocar(m, "load");
            if (r instanceof Integer && ((Integer) r) != 0) throw new IllegalStateException("load() = " + r);
            modulo = m;
            cargado = pte;
            Log.i(TAG, "cargado " + pte);
        }
    }

    private void cerrarModulo() {
        synchronized (lock) {
            if (modulo != null) {
                try {
                    invocar(modulo, "close");
                } catch (Throwable ignored) {
                }
            }
            modulo = null;
            cargado = "";
        }
    }

    private static Object invocar(Object o, String nombre) throws Exception {
        for (Method m : o.getClass().getMethods())
            if (m.getName().equals(nombre) && m.getParameterTypes().length == 0) return m.invoke(o);
        return null;
    }

    private void generar(OutputStream out, String prompt, int maxTokens, int ctx, float temperatura) throws Exception {
        final Object m = modulo;
        if (m == null) {
            responder(out, 409, "no hay modelo cargado");
            return;
        }
        final Class<?> cb = Class.forName(CALLBACK);
        final LinkedBlockingQueue<String> cola = new LinkedBlockingQueue<String>();
        final String FIN = "\u0000fin";
        final String[] error = {null};
        final Object proxy = Proxy.newProxyInstance(cb.getClassLoader(), new Class<?>[]{cb}, new InvocationHandler() {
            @Override
            public Object invoke(Object p, Method metodo, Object[] args) {
                String n = metodo.getName();
                if (n.equals("onResult") && args != null && args.length > 0 && args[0] != null) cola.add((String) args[0]);
                else if (n.equals("onError") && args != null && args.length > 1) error[0] = String.valueOf(args[1]);
                else if (n.equals("hashCode")) return System.identityHashCode(p);
                else if (n.equals("equals")) return p == args[0];
                else if (n.equals("toString")) return "LlmCallback";
                return null;
            }
        });
        // seq_len de ExecuTorch = prompt + respuesta: se limita al contexto
        final int seq = Math.max(64, Math.min(ctx, prompt.length() / 3 + maxTokens));
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                synchronized (lock) {
                    try {
                        Method g = m.getClass().getMethod("generate", String.class, int.class, cb, boolean.class);
                        g.invoke(m, prompt, seq, proxy, false);
                    } catch (Throwable e) {
                        error[0] = causa(e);
                    } finally {
                        cola.add(FIN);
                    }
                }
            }
        }, "executorch-gen");
        t.start();
        out.write(("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nTransfer-Encoding: chunked\r\n"
                + "Connection: close\r\n\r\n").getBytes("UTF-8"));
        out.flush();
        boolean cortado = false;
        while (true) {
            String x = cola.poll(600, TimeUnit.SECONDS);
            if (x == null || x == FIN) break;
            if (cortado) continue;
            try {
                trozo(out, x);
            } catch (Exception e) {   // el cliente cerro: se para la generacion
                cortado = true;
                try {
                    invocar(m, "stop");
                } catch (Throwable ignored) {
                }
            }
        }
        if (!cortado) {
            if (error[0] != null) trozo(out, "\n[ExecuTorch: " + error[0] + "]");
            out.write("0\r\n\r\n".getBytes("UTF-8"));
            out.flush();
        }
    }
}
