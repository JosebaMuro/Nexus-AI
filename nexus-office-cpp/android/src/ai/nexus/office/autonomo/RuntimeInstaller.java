package ai.nexus.office.autonomo;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Descompresion y comprobacion del runtime (Python + Nexus-Office) de la APK. Java puro, sin Android, para poder
 * probarlo fuera del movil (packaging/build_apk_autonomo.py --probar-runtime).
 */
final class RuntimeInstaller {
    private RuntimeInstaller() {
    }

    interface Progress {
        /** pct 0-100; what: fase (null = sin cambios). */
        void update(int pct, String what);

        boolean cancelled();
    }

    static class NoSpaceException extends IOException {
        NoSpaceException(String m) {
            super(m);
        }
    }

    /** Comprobacion rapida en cada arranque: marca de completado y los ficheros imprescindibles. */
    static boolean runtimeOk(File dir) {
        String[] must = {".completo", "nexus/web/index.html", "nexus/certs/cacert.pem"};
        for (String m : must) {
            File f = new File(dir, m);
            if (!f.isFile() || (!m.equals(".completo") && f.length() == 0)) return false;
        }
        return true;
    }

    static void extract(File zipFile, final File dest, final long total, final Progress progress) throws Exception {
        final ZipFile zf = new ZipFile(zipFile);
        final String base = dest.getCanonicalPath() + File.separator;
        final List<ZipEntry> entries = new ArrayList<ZipEntry>();
        java.util.Enumeration<? extends ZipEntry> en = zf.entries();
        while (en.hasMoreElements()) entries.add(en.nextElement());
        for (ZipEntry e : entries) {   // primero las carpetas (y validar rutas), luego los ficheros en paralelo
            File out = new File(dest, e.getName());
            if (!out.getCanonicalPath().startsWith(base)) throw new IOException("ruta no valida en el runtime: " + e.getName());
            (e.isDirectory() ? out : out.getParentFile()).mkdirs();
        }
        // los ficheros grandes primero: reparte mejor el trabajo entre hilos
        Collections.sort(entries, new java.util.Comparator<ZipEntry>() {
            @Override
            public int compare(ZipEntry a, ZipEntry b) {
                return Long.compare(b.getSize(), a.getSize());
            }
        });
        int threads = Math.max(2, Math.min(4, Runtime.getRuntime().availableProcessors()));
        progress.update(10, "Descomprimiendo Nexus-Office (" + threads + " hilos)...");
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        final AtomicLong done = new AtomicLong();
        List<Future<?>> jobs = new ArrayList<Future<?>>();
        final int chunk = Math.max(1, entries.size() / (threads * 8));
        for (int i = 0; i < entries.size(); i += chunk) {
            final List<ZipEntry> part = entries.subList(i, Math.min(entries.size(), i + chunk));
            jobs.add(pool.submit(new java.util.concurrent.Callable<Void>() {
                @Override
                public Void call() throws Exception {
                    byte[] buf = new byte[1 << 16];
                    for (ZipEntry e : part) {
                        if (progress.cancelled()) throw new IOException("cancelado");
                        if (e.isDirectory()) continue;
                        InputStream in = zf.getInputStream(e);
                        OutputStream os = new FileOutputStream(new File(dest, e.getName()));
                        int n;
                        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                        os.close();
                        in.close();
                        long d = done.addAndGet(Math.max(0, e.getSize()));
                        progress.update((int) Math.min(99, 10 + d * 89 / Math.max(1, total)), null);
                    }
                    return null;
                }
            }));
        }
        try {
            for (Future<?> f : jobs) f.get();
        } catch (java.util.concurrent.ExecutionException e) {
            Throwable c = e.getCause() == null ? e : e.getCause();
            String m = String.valueOf(c.getMessage());
            if (m.contains("ENOSPC") || m.contains("No space")) throw new NoSpaceException("el movil se ha quedado sin espacio al descomprimir. Libera espacio y pulsa Reintentar.");
            throw new IOException("fallo al descomprimir: " + m);
        } finally {
            pool.shutdownNow();
            zf.close();
        }
    }

    /** {ficheros, bytes} bajo dir. */
    static long[] count(File dir) {
        long[] r = {0, 0};
        File[] kids = dir.listFiles();
        if (kids == null) return r;
        for (File k : kids) {
            if (k.isDirectory()) {
                long[] s = count(k);
                r[0] += s[0];
                r[1] += s[1];
            } else {
                r[0]++;
                r[1] += k.length();
            }
        }
        return r;
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                if (k.isDirectory() && !isSymlink(k)) deleteTree(k);
                else k.delete();
            }
        }
        f.delete();
    }

    private static boolean isSymlink(File f) {
        try {
            return !f.getCanonicalPath().equals(f.getAbsolutePath());
        } catch (IOException e) {
            return true;
        }
    }


    /** Prueba fuera de Android: java ai.nexus.office.autonomo.RuntimeInstaller runtime.zip destino ficheros bytes */
    public static void main(String[] a) throws Exception {
        long t0 = System.currentTimeMillis();
        File dest = new File(a[1]);
        extract(new File(a[0]), dest, Long.parseLong(a[3]), new Progress() {
            int last = -1;

            @Override
            public void update(int pct, String what) {
                if (what != null) System.out.println(what);
                if (pct / 25 != last) {
                    last = pct / 25;
                    System.out.println(pct + " %");
                }
            }

            @Override
            public boolean cancelled() {
                return false;
            }
        });
        long[] c = count(dest);
        System.out.println("ficheros " + c[0] + "/" + a[2] + " bytes " + c[1] + "/" + a[3] + " en "
                + (System.currentTimeMillis() - t0) + " ms");
        new File(dest, ".completo").createNewFile();
        boolean ok = c[0] == Long.parseLong(a[2]) && c[1] == Long.parseLong(a[3]) && runtimeOk(dest);
        System.out.println(ok ? "OK" : "FALLO");
        System.exit(ok ? 0 : 1);
    }
}
