package ai.nexus.office.autonomo;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.net.ConnectivityManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.RecognizerIntent;
import android.speech.tts.UtteranceProgressListener;
import android.speech.SpeechRecognizer;
import android.speech.RecognitionListener;
import android.speech.tts.TextToSpeech;
import android.util.Base64;
import android.util.DisplayMetrics;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.webkit.CookieManager;
import android.webkit.DownloadListener;
import android.webkit.JavascriptInterface;
import android.webkit.URLUtil;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Locale;

/**
 * Nexus-Office autonomo para Android: arranca Nexus-Office completo dentro del movil (servicio NexusServerService con el Python
 * y el llama.cpp que lleva la APK) y muestra su webui a pantalla completa. No necesita PC; solo descarga, una
 * vez, el modelo de IA adecuado para este movil.
 */
public class MainActivity extends Activity {
    private static final String HOME = "http://127.0.0.1:" + NexusServerService.PORT + "/";
    private static final String BOOT = "file:///android_asset/arranque.html";
    private static final int REQ_FILE = 1, REQ_VOICE = 2, REQ_SAVE = 3, REQ_NOTIF = 4;

    private WebView web;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private ValueCallback<Uri[]> fileCallback;
    private TextToSpeech tts;
    private boolean ttsReady;
    // modo en vivo del chat: reconocimiento continuo sin dialogo (SpeechRecognizer) y aviso al acabar de hablar
    private SpeechRecognizer recognizer;
    private String liveLang = "";
    private long lastLevel;
    private static final int REQ_MIC = 7;
    private byte[] pendingSave;
    private String pendingSaveName = "";
    private long bootStarted;
    private boolean modelOffered;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.parseColor("#04060c"));
        getWindow().setNavigationBarColor(Color.parseColor("#04060c"));
        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#04060c"));
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setDatabaseEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setAllowFileAccess(false);
        s.setUserAgentString(s.getUserAgentString() + " NexusAndroid/" + version() + " (" + form() + "; sw"
                + getResources().getConfiguration().smallestScreenWidthDp + "; autonomo)");
        web.addJavascriptInterface(new Bridge(), "NexusApp");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
                Uri u = req.getUrl();
                if ("127.0.0.1".equals(u.getHost()) || "file".equals(u.getScheme())) return false;
                openExternal(u.toString());   // enlaces externos (fuentes, webs) en el navegador
                return true;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest req, WebResourceError err) {
                if (req.isForMainFrame() && req.getUrl().toString().startsWith(HOME)) {
                    view.loadUrl(BOOT);   // el servidor aun arranca o se ha parado: vuelve a la pantalla de espera
                    waitForServer();
                }
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> cb, FileChooserParams params) {
                if (fileCallback != null) fileCallback.onReceiveValue(null);
                fileCallback = cb;
                try {
                    Intent i = params.createIntent();
                    i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, params.getMode() == FileChooserParams.MODE_OPEN_MULTIPLE);
                    startActivityForResult(i, REQ_FILE);
                } catch (ActivityNotFoundException e) {
                    fileCallback = null;
                    return false;
                }
                return true;
            }
        });
        // descargas del servidor local (PDF de la biblioteca, salidas...): se guardan donde elija el usuario
        web.setDownloadListener(new DownloadListener() {
            @Override
            public void onDownloadStart(String url, String ua, String disposition, String mime, long length) {
                download(url, URLUtil.guessFileName(url, disposition, mime), mime);
            }
        });

        tts = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int status) {
                ttsReady = status == TextToSpeech.SUCCESS;
                if (ttsReady) tts.setLanguage(Locale.getDefault());
                if (ttsReady) tts.setOnUtteranceProgressListener(new UtteranceProgressListener() {
                    @Override
                    public void onStart(String id) {
                    }

                    @Override
                    public void onDone(String id) {
                        js("window.__nexusSpoken && window.__nexusSpoken()");
                    }

                    @Override
                    public void onError(String id) {
                        js("window.__nexusSpoken && window.__nexusSpoken()");
                    }
                });
            }
        });
        CookieManager.getInstance().setAcceptCookie(true);

        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIF);
        }
        startServer(null);
        web.loadUrl(BOOT);
        waitForServer();
    }

    private void startServer(String action) {
        String st = NexusServerService.state;
        if (action != null || st.startsWith("error:") || "stopped".equals(st)) NexusServerService.state = "starting";   // sin estado viejo
        Intent i = new Intent(this, NexusServerService.class);
        if (action != null) i.setAction(action);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(i);
        else startService(i);
    }

    /** Sondea /api/ping hasta que Nexus-Office responde; la primera vez tarda mas (Python descomprime sus paquetes). */
    private void waitForServer() {
        bootStarted = System.currentTimeMillis();
        new Thread(new Runnable() {
            @Override
            public void run() {
                while (!isFinishing()) {
                    String st = NexusServerService.state;
                    if (st.startsWith("error:")) {
                        bootStatus("error", st.substring(6), "");
                        return;
                    }
                    if (ping()) {
                        ui.post(new Runnable() {
                            @Override
                            public void run() {
                                web.loadUrl(HOME);
                            }
                        });
                        offerModel();
                        return;
                    }
                    String what = NexusServerService.detail;
                    if (st.startsWith("preparing:")) bootStatus("preparing", st.substring(10), what);
                    else bootStatus("starting", String.valueOf((System.currentTimeMillis() - bootStarted) / 1000), what);
                    try {
                        Thread.sleep(700);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }, "nexus-wait").start();
    }

    // ------------------------------------------------------------------ modelo de IA adecuado al movil
    /**
     * Todo va dentro de la APK salvo los modelos: Nexus-Office elige el mejor para la RAM y la CPU de este movil
     * (/api/models/recommended, marcado "best") y solo se descarga ese. Con Wi-Fi empieza solo; con datos
     * moviles pregunta antes.
     */
    private void offerModel() {
        if (modelOffered) return;
        modelOffered = true;
        try {
            JSONObject rec = new JSONObject(httpGet(HOME + "api/models/recommended"));
            JSONArray items = rec.getJSONArray("items");
            boolean haveLlm = false;
            JSONObject best = null;
            for (int i = 0; i < items.length(); i++) {
                JSONObject m = items.getJSONObject(i);
                if (!"llm".equals(m.optString("kind"))) continue;
                if (m.optBoolean("installed")) haveLlm = true;
                if (m.optBoolean("best")) best = m;
            }
            if (haveLlm) {
                offerTuningAdvice();
                return;
            }
            if (best == null || best.optBoolean("installed")) return;
            final String id = best.getString("id");
            final String label = best.optString("name", id) + " (" + best.optInt("size_mb") + " MB)";
            ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            if (!cm.isActiveNetworkMetered()) {
                downloadModel(id, label);
                return;
            }
            ui.post(new Runnable() {
                @Override
                public void run() {
                    new AlertDialog.Builder(MainActivity.this)
                            .setTitle("Descargar la IA de Nexus-Office")
                            .setMessage("El modelo adecuado para este movil es " + label + ". Estas usando datos moviles: "
                                    + "¿lo descargo ahora o esperas a tener Wi-Fi? (tambien puedes hacerlo en Modelos)")
                            .setPositiveButton("Descargar", new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    new Thread(new Runnable() {
                                        @Override
                                        public void run() {
                                            downloadModel(id, label);
                                        }
                                    }).start();
                                }
                            })
                            .setNegativeButton("Con Wi-Fi", null)
                            .show();
                }
            });
        } catch (Exception ignored) {   // sin modelo Nexus-Office sigue funcionando con su nucleo integrado
        }
    }

    /**
     * Autoajuste (nexus/android_tune.py): tras medir la velocidad real, Nexus-Office puede aconsejar un modelo mas pequeno
     * (si va lento) o mayor (si sobra velocidad y RAM). Se pregunta una vez; si se rechaza, no se repite en 7 dias.
     */
    private void offerTuningAdvice() throws Exception {
        JSONObject t = new JSONObject(httpGet(HOME + "api/android/autoajuste"));
        JSONObject adv = t.optJSONObject("advice");
        if (adv == null || !t.isNull("pending_switch")) return;   // ya hay un cambio en marcha
        final String id = adv.getString("id");
        final String pref = "rechazado-" + adv.optString("from") + "-" + id;
        final android.content.SharedPreferences sp = getSharedPreferences("autoajuste", MODE_PRIVATE);
        if (System.currentTimeMillis() - sp.getLong(pref, 0) < 7L * 24 * 3600 * 1000) return;
        final boolean installed = adv.optBoolean("installed");
        final String msg = "Con " + adv.optString("from") + " " + adv.optString("reason") + ". ¿Cambio a "
                + adv.optString("name", id) + (installed ? "" : " (" + adv.optInt("size_mb") + " MB, se descarga y se activa al terminar)") + "?";
        ui.post(new Runnable() {
            @Override
            public void run() {
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("Nexus-Office se ha ajustado a tu movil")
                        .setMessage(msg)
                        .setPositiveButton("Cambiar", new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface d, int w) {
                                new Thread(new Runnable() {
                                    @Override
                                    public void run() {
                                        post(HOME + "api/android/autoajuste/modelo/" + id);
                                    }
                                }).start();
                                Toast.makeText(MainActivity.this, installed ? "Modelo cambiado" : "Descargando; mira el progreso en Modelos",
                                        Toast.LENGTH_LONG).show();
                            }
                        })
                        .setNegativeButton("Ahora no", new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface d, int w) {
                                sp.edit().putLong(pref, System.currentTimeMillis()).apply();
                            }
                        })
                        .show();
            }
        });
    }

    private static int post(String url) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.getOutputStream().write("{}".getBytes("UTF-8"));
            int code = c.getResponseCode();
            c.disconnect();
            return code;
        } catch (Exception e) {
            return -1;
        }
    }

    private void downloadModel(String id, final String label) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(HOME + "api/models/recommended/" + id).openConnection();
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            c.getOutputStream().write("{}".getBytes("UTF-8"));
            int code = c.getResponseCode();
            c.disconnect();
            if (code == 200) {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        Toast.makeText(MainActivity.this, "Descargando " + label + ". Mira el progreso en Modelos.", Toast.LENGTH_LONG).show();
                    }
                });
            }
        } catch (Exception ignored) {
        }
    }

    private static String httpGet(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(2000);
        c.setReadTimeout(15000);
        InputStream in = c.getInputStream();
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        byte[] b = new byte[8192];
        int n;
        while ((n = in.read(b)) > 0) buf.write(b, 0, n);
        in.close();
        return buf.toString("UTF-8");
    }

    private static boolean ping() {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(HOME + "api/ping").openConnection();
            c.setConnectTimeout(500);
            c.setReadTimeout(1500);
            int code = c.getResponseCode();
            c.disconnect();
            return code == 200;
        } catch (Exception e) {
            return false;
        }
    }

    private void bootStatus(String state, String detail, String what) {
        js("window.__nexusBoot && window.__nexusBoot(" + JSONObject.quote(state) + "," + JSONObject.quote(detail) + ","
                + JSONObject.quote(what == null ? "" : what) + ")");
    }

    // ------------------------------------------------------------------ descargas y ficheros
    private void download(final String url, final String name, final String mime) {
        final String cookies = CookieManager.getInstance().getCookie(url);
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
                    if (cookies != null) c.setRequestProperty("Cookie", cookies);
                    InputStream in = c.getInputStream();
                    ByteArrayOutputStream buf = new ByteArrayOutputStream();
                    byte[] b = new byte[65536];
                    int n;
                    while ((n = in.read(b)) > 0) buf.write(b, 0, n);
                    in.close();
                    askWhereToSave(name, mime == null || mime.isEmpty() ? "application/octet-stream" : mime, buf.toByteArray());
                } catch (Exception e) {
                    js("window.__nexusSaved && window.__nexusSaved(false," + JSONObject.quote(name) + ")");
                }
            }
        }, "nexus-download").start();
    }

    private void askWhereToSave(final String name, final String mime, final byte[] data) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                pendingSave = data;
                pendingSaveName = name;
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType(mime.split(";")[0].trim());
                i.putExtra(Intent.EXTRA_TITLE, name);
                try {
                    startActivityForResult(i, REQ_SAVE);
                } catch (ActivityNotFoundException e) {
                    pendingSave = null;
                    js("window.__nexusSaved && window.__nexusSaved(false," + JSONObject.quote(name) + ")");
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_FILE && fileCallback != null) {
            Uri[] picked;
            if (res == RESULT_OK && data != null && data.getClipData() != null) {
                ClipData cd = data.getClipData();
                picked = new Uri[cd.getItemCount()];
                for (int i = 0; i < cd.getItemCount(); i++) picked[i] = cd.getItemAt(i).getUri();
            } else {
                picked = WebChromeClient.FileChooserParams.parseResult(res, data);
            }
            fileCallback.onReceiveValue(picked);
            fileCallback = null;
        } else if (req == REQ_VOICE) {
            String text = "";
            if (res == RESULT_OK && data != null) {
                ArrayList<String> r = data.getStringArrayListExtra(RecognizerIntent.EXTRA_RESULTS);
                if (r != null && !r.isEmpty()) text = r.get(0);
            }
            js("window.__nexusVoice && window.__nexusVoice(" + JSONObject.quote(text) + ")");
        } else if (req == REQ_SAVE) {
            boolean ok = false;
            if (res == RESULT_OK && data != null && data.getData() != null && pendingSave != null) {
                try {
                    OutputStream out = getContentResolver().openOutputStream(data.getData());
                    out.write(pendingSave);
                    out.close();
                    ok = true;
                } catch (Exception ignored) {
                }
            }
            pendingSave = null;
            js("window.__nexusSaved && window.__nexusSaved(" + ok + "," + JSONObject.quote(pendingSaveName) + ")");
        }
    }

    // ------------------------------------------------------------------ ventana
    private String form() {
        return getResources().getConfiguration().smallestScreenWidthDp >= 600 ? "tablet" : "phone";
    }

    private String version() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "1";
        }
    }

    private String deviceJson() {
        Configuration c = getResources().getConfiguration();
        DisplayMetrics m = getResources().getDisplayMetrics();
        try {
            return new JSONObject().put("form", form()).put("sw", c.smallestScreenWidthDp).put("w", c.screenWidthDp).put("h", c.screenHeightDp)
                    .put("density", m.density).put("sdk", Build.VERSION.SDK_INT).put("version", version()).put("app", "autonomo")
                    .put("orientation", c.orientation == Configuration.ORIENTATION_LANDSCAPE ? "landscape" : "portrait").toString();
        } catch (Exception e) {
            return "{}";
        }
    }

    private void setBars(int color, boolean light) {
        getWindow().setStatusBarColor(color);
        getWindow().setNavigationBarColor(color);
        View d = getWindow().getDecorView();
        int f = d.getSystemUiVisibility();
        f = light ? f | View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR : f & ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
        f = light ? f | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : f & ~View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        d.setSystemUiVisibility(f);
        web.setBackgroundColor(color);
    }

    @Override
    public void onConfigurationChanged(Configuration cfg) {
        super.onConfigurationChanged(cfg);
        js("window.__nexusConfig && window.__nexusConfig(" + deviceJson() + ")");
    }

    private void openExternal(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException ignored) {
        }
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        super.onRequestPermissionsResult(req, perms, res);
        if (req == REQ_MIC) {
            if (res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED) startLive();
            else js("window.__nexusLiveError && window.__nexusLiveError('permiso de micrófono denegado')");
        }
    }

    private void startLive() {
        if (!SpeechRecognizer.isRecognitionAvailable(this)) {
            js("window.__nexusLiveError && window.__nexusLiveError('este móvil no tiene reconocimiento de voz')");
            return;
        }
        if (recognizer == null) {
            recognizer = SpeechRecognizer.createSpeechRecognizer(this);
            recognizer.setRecognitionListener(new RecognitionListener() {
                @Override
                public void onReadyForSpeech(Bundle b) {
                    js("window.__nexusLiveState && window.__nexusLiveState('ready')");
                }

                @Override
                public void onBeginningOfSpeech() {
                    js("window.__nexusLiveState && window.__nexusLiveState('speech')");
                }

                @Override
                public void onRmsChanged(float db) {
                    long now = System.currentTimeMillis();
                    if (now - lastLevel < 90) return;
                    lastLevel = now;
                    js("window.__nexusLiveLevel && window.__nexusLiveLevel(" + Math.max(0f, Math.min(1f, (db + 2f) / 12f)) + ")");
                }

                @Override
                public void onBufferReceived(byte[] buf) {
                }

                @Override
                public void onEndOfSpeech() {
                    js("window.__nexusLiveState && window.__nexusLiveState('end')");
                }

                @Override
                public void onError(int code) {
                    js("window.__nexusLiveError && window.__nexusLiveError(" + code + ")");
                }

                @Override
                public void onResults(Bundle b) {
                    ArrayList<String> r = b.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
                    String text = r != null && !r.isEmpty() ? r.get(0) : "";
                    js("window.__nexusLiveResult && window.__nexusLiveResult(" + JSONObject.quote(text) + ")");
                }

                @Override
                public void onPartialResults(Bundle b) {
                    ArrayList<String> r = b.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
                    if (r != null && !r.isEmpty())
                        js("window.__nexusLivePartial && window.__nexusLivePartial(" + JSONObject.quote(r.get(0)) + ")");
                }

                @Override
                public void onEvent(int type, Bundle b) {
                }
            });
        }
        Intent i = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        i.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
        if (!liveLang.isEmpty()) i.putExtra(RecognizerIntent.EXTRA_LANGUAGE, liveLang);
        i.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);
        i.putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true);   // sin internet si el movil tiene el paquete de voz
        recognizer.startListening(i);
    }

    private void js(final String code) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                web.evaluateJavascript(code, null);
            }
        });
    }

    @Override
    public void onBackPressed() {
        web.evaluateJavascript("window.__nexusBack ? window.__nexusBack() : false", new ValueCallback<String>() {
            @Override
            public void onReceiveValue(String v) {
                if ("true".equals(v)) return;
                if (web.canGoBack()) web.goBack();
                else moveTaskToBack(true);   // Nexus-Office sigue disponible en segundo plano (notificacion para detenerlo)
            }
        });
    }

    @Override
    protected void onPause() {
        super.onPause();
        CookieManager.getInstance().flush();
    }

    @Override
    protected void onDestroy() {
        if (recognizer != null) recognizer.destroy();
        if (tts != null) tts.shutdown();
        super.onDestroy();
    }

    private static String rgbaToHex(String rgba) {
        String[] p = rgba.substring(rgba.indexOf('(') + 1, rgba.indexOf(')')).split(",");
        return String.format(Locale.ROOT, "#%02x%02x%02x", Integer.parseInt(p[0].trim()), Integer.parseInt(p[1].trim()), Integer.parseInt(p[2].trim()));
    }

    /** Puente JavaScript (arranque.html y webui de Nexus-Office). */
    private class Bridge {
        @JavascriptInterface
        public String mode() {
            return "autonomo";
        }

        /** En la app autonoma "cambiar de servidor" reinicia el Nexus-Office del movil. */
        /** Conectar con el Nexus-Office del ordenador (red local, internet o VPN WireGuard). */
        @JavascriptInterface
        public void desktop() {
            startActivity(new Intent(MainActivity.this, EscritorioActivity.class));
        }

        @JavascriptInterface
        public void reset() {
            restart(NexusServerService.ACTION_RETRY);
        }

        /** Pantalla de arranque: vuelve a arrancar Nexus-Office (para el arranque colgado o fallido, si lo hay). */
        @JavascriptInterface
        public void retry() {
            restart(NexusServerService.ACTION_RETRY);
        }

        /** Pantalla de arranque: borra el entorno de ejecucion y lo vuelve a descomprimir de la APK. */
        @JavascriptInterface
        public void repair() {
            restart(NexusServerService.ACTION_REPAIR);
        }

        /** Las ultimas lineas del registro de Nexus-Office, para copiarlas si algo falla. */
        @JavascriptInterface
        public String log() {
            File d = getFilesDir();
            return NexusServerService.tail(new File(d, "nexus.prev.log"), 2000) + "\n----\n"
                    + NexusServerService.tail(new File(d, "nexus.log"), 6000);
        }

        private void restart(final String action) {
            ui.post(new Runnable() {
                @Override
                public void run() {
                    web.loadUrl(BOOT);
                    startServer(action);
                    waitForServer();
                }
            });
        }

        @JavascriptInterface
        public void listen(String lang) {
            Intent i = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
            i.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
            if (lang != null && !lang.isEmpty()) i.putExtra(RecognizerIntent.EXTRA_LANGUAGE, lang);
            i.putExtra(RecognizerIntent.EXTRA_PROMPT, "Habla con Nexus-Office");
            try {
                startActivityForResult(i, REQ_VOICE);
            } catch (ActivityNotFoundException e) {
                js("window.__nexusVoice && window.__nexusVoice('')");
            }
        }

        @JavascriptInterface
        public boolean canListenLive() {
            return SpeechRecognizer.isRecognitionAvailable(MainActivity.this);
        }

        @JavascriptInterface
        public void listenLive(String lang) {
            liveLang = lang == null ? "" : lang;
            if (Build.VERSION.SDK_INT >= 23 && checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_MIC);
                return;
            }
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    startLive();
                }
            });
        }

        @JavascriptInterface
        public void stopListening() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (recognizer != null) recognizer.cancel();
                }
            });
        }

        @JavascriptInterface
        public boolean speak(String text, float rate) {
            if (!ttsReady) return false;
            tts.setSpeechRate(rate > 0 ? rate : 1f);
            tts.speak(text, TextToSpeech.QUEUE_FLUSH, null, "nexus");
            return true;
        }

        @JavascriptInterface
        public void stopSpeaking() {
            if (ttsReady) tts.stop();
        }

        @JavascriptInterface
        public String device() {
            return deviceJson();
        }

        @JavascriptInterface
        public void haptic() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    web.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                }
            });
        }

        @JavascriptInterface
        public void setBars(final String color, final boolean light) {
            try {
                final int c = Color.parseColor(color.trim().startsWith("rgba") ? rgbaToHex(color) : color.trim());
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        MainActivity.this.setBars(c, light);
                    }
                });
            } catch (Exception ignored) {
            }
        }

        @JavascriptInterface
        public void copy(String text) {
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("Nexus-Office", text));
        }

        @JavascriptInterface
        public void saveFile(String name, String mime, String base64) {
            try {
                askWhereToSave(name == null || name.isEmpty() ? "nexus.txt" : name, mime == null ? "text/plain" : mime,
                        Base64.decode(base64, Base64.DEFAULT));
            } catch (Exception e) {
                js("window.__nexusSaved && window.__nexusSaved(false," + JSONObject.quote(String.valueOf(name)) + ")");
            }
        }

        /** Sistema > Arranque automatico: Nexus-Office como servicio al encender el movil. */
        @JavascriptInterface
        public boolean bootStart() {
            return getSharedPreferences(BootReceiver.PREFS, MODE_PRIVATE).getBoolean(BootReceiver.KEY, false);
        }

        @JavascriptInterface
        public void setBootStart(boolean on) {
            getSharedPreferences(BootReceiver.PREFS, MODE_PRIVATE).edit().putBoolean(BootReceiver.KEY, on).apply();
        }

        @JavascriptInterface
        public void openUrl(String url) {
            openExternal(url);
        }
    }
}
