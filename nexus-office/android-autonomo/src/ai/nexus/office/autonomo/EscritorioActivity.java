package ai.nexus.office.autonomo;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.net.Uri;
import android.net.VpnService;
import android.net.http.SslCertificate;
import android.net.http.SslError;
import android.os.Build;
import android.os.Bundle;
import android.content.pm.PackageManager;
import android.Manifest;
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
import android.webkit.SslErrorHandler;
import android.webkit.URLUtil;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.Socket;
import java.net.URL;
import java.security.MessageDigest;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSession;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * Nexus-Office VPN para Android: cliente del Nexus-Office de tu PC (Windows, Linux o macOS) con la webui completa a pantalla
 * completa. Tres formas de llegar al PC: VPN WireGuard cifrada (recomendada, desde cualquier sitio), HTTPS con
 * certificado anclado (acceso remoto) o la Wi-Fi de casa. Voz nativa, adjuntar ficheros y descargar ficheros.
 */
public class EscritorioActivity extends Activity implements NexusVpnService.Listener {
    private static final String SETUP = "file:///android_asset/escritorio.html";
    private static final int REQ_FILE = 1, REQ_VOICE = 2, REQ_SAVE = 3, REQ_VPN = 4;

    private WebView web;
    private SharedPreferences prefs;
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

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("escritorio", MODE_PRIVATE);
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
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        s.setAllowFileAccess(false);
        // la webui adapta su diseno a la clase de pantalla oficial de Android (movil o tablet)
        s.setUserAgentString(s.getUserAgentString() + " NexusAndroid/" + version() + " (" + form() + "; sw"
                + getResources().getConfiguration().smallestScreenWidthDp + "; vpn)");
        web.addJavascriptInterface(new Bridge(), "NexusApp");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
                Uri u = req.getUrl();
                String saved = prefs.getString("url", "");
                boolean sameServer = !saved.isEmpty() && u.getHost() != null && u.getHost().equals(Uri.parse(saved).getHost());
                if (sameServer || "file".equals(u.getScheme())) return false;
                openExternal(u.toString());   // enlaces externos (fuentes, webs) en el navegador
                return true;
            }

            @Override
            public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
                // certificado propio de Nexus-Office: solo se acepta si su huella SHA-256 es la anclada al emparejar
                String pin = prefs.getString("pin", "");
                if (!pin.isEmpty() && pin.equalsIgnoreCase(certSha256(error.getCertificate()))) {
                    handler.proceed();
                } else {
                    handler.cancel();
                    view.loadUrl(SETUP + "#error=" + Uri.encode("certificado no reconocido: posible suplantacion del servidor. "
                            + "Vuelve a emparejar con un codigo nuevo de tu Nexus-Office."));
                }
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest req, WebResourceError err) {
                if (req.isForMainFrame()) {
                    view.loadUrl(SETUP + "#error=" + Uri.encode(String.valueOf(err.getDescription())));
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
        // descargas normales del servidor (PDF de la biblioteca, salidas...): se guardan donde elija el usuario
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
        NexusVpnService.listener = this;
        String mode = prefs.getString("mode", "");
        if ("vpn".equals(mode) && !prefs.getString("vpn_code", "").isEmpty()) {
            web.loadUrl(SETUP + "#vpn=connecting");
            startVpn();
        } else if (!prefs.getString("remote_urls", "").isEmpty()) {
            connectRemote("", "");
        } else {
            String url = prefs.getString("url", "");
            web.loadUrl(url.isEmpty() ? SETUP : url);
        }
    }

    // ------------------------------------------------------------------ VPN
    private void startVpn() {
        Intent consent = VpnService.prepare(this);
        if (consent != null) {
            startActivityForResult(consent, REQ_VPN);   // Android pide al usuario permiso para crear la VPN (solo la 1.a vez)
        } else {
            onActivityResult(REQ_VPN, RESULT_OK, null);
        }
    }

    @Override
    public void onVpnState(final String state, final String detail) {
        js("window.__nexusVpn && window.__nexusVpn(" + JSONObject.quote(state) + "," + JSONObject.quote(detail) + ")");
        if ("up".equals(state)) {
            try {
                final WgConfig cfg = WgConfig.parse(prefs.getString("vpn_code", ""));
                prefs.edit().putString("url", cfg.url).apply();
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        web.loadUrl(cfg.targetUrl());
                    }
                });
            } catch (Exception ignored) {
            }
        } else if ("error".equals(state)) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    if (web.getUrl() == null || !web.getUrl().startsWith(SETUP)) {
                        web.loadUrl(SETUP + "#error=" + Uri.encode(detail));
                    }
                }
            });
        }
    }

    // ------------------------------------------------------------------ utilidades
    /** Huella SHA-256 (hex en mayusculas) del certificado del servidor. */
    static String certSha256(SslCertificate c) {
        try {
            byte[] der;
            if (Build.VERSION.SDK_INT >= 29) {
                X509Certificate x = c.getX509Certificate();
                der = x == null ? null : x.getEncoded();
            } else {
                der = SslCertificate.saveState(c).getByteArray("x509-certificate");
            }
            return der == null ? "" : sha256Hex(der);
        } catch (Exception e) {
            return "";
        }
    }

    static String sha256Hex(byte[] der) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(der)) sb.append(String.format(Locale.ROOT, "%02X", b));
        return sb.toString();
    }

    /** true si el puerto de la URL acepta conexiones (para elegir entre la direccion de internet y la de casa). */
    private static boolean reachable(String url) {
        try {
            Uri u = Uri.parse(url);
            int port = u.getPort() > 0 ? u.getPort() : ("https".equals(u.getScheme()) ? 443 : 80);
            Socket s = new Socket();
            try {
                s.connect(new InetSocketAddress(u.getHost(), port), 2500);
                return true;
            } finally {
                s.close();
            }
        } catch (Exception e) {
            return false;
        }
    }

    /** Prueba las direcciones guardadas (la ultima que funciono primero) y abre la primera accesible. */
    private void connectRemote(final String pairCode, final String failJs) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                List<String> urls = new ArrayList<String>();
                String last = prefs.getString("url", "");
                if (!last.isEmpty()) urls.add(last);
                try {
                    JSONArray arr = new JSONArray(prefs.getString("remote_urls", "[]"));
                    for (int i = 0; i < arr.length(); i++) if (!urls.contains(arr.getString(i))) urls.add(arr.getString(i));
                } catch (Exception ignored) {
                }
                for (String u : urls) {
                    if (reachable(u)) {
                        prefs.edit().putString("url", u).apply();
                        final String target = pairCode.isEmpty() ? u + "/"
                                : u + "/?pair=" + Uri.encode(pairCode) + "&name=" + Uri.encode(Build.MANUFACTURER + " " + Build.MODEL);
                        loadOnUi(target);
                        return;
                    }
                }
                if (!failJs.isEmpty()) js(failJs);
                else loadOnUi(SETUP + "#error=" + Uri.encode("ninguna direccion de tu Nexus-Office responde (¿esta encendido y con el acceso remoto activo?)"));
            }
        }).start();
    }

    private void loadOnUi(final String url) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                web.loadUrl(url);
            }
        });
    }

    /** Descarga una URL del servidor con la sesion de la webui (cookie) y la guarda con el selector del sistema. */
    private void download(final String url, final String name, final String mime) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
                    if (c instanceof HttpsURLConnection) pinTls((HttpsURLConnection) c);
                    String cookie = CookieManager.getInstance().getCookie(url);
                    if (cookie != null) c.setRequestProperty("Cookie", cookie);
                    c.setConnectTimeout(10000);
                    c.setReadTimeout(60000);
                    InputStream in = c.getInputStream();
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                    in.close();
                    askWhereToSave(name, mime == null || mime.isEmpty() ? "application/octet-stream" : mime, out.toByteArray());
                } catch (Exception e) {
                    js("window.__nexusSaved && window.__nexusSaved(false," + JSONObject.quote(name) + ")");
                }
            }
        }).start();
    }

    /** HTTPS con el certificado propio de Nexus-Office: solo se acepta el de la huella anclada. */
    private void pinTls(HttpsURLConnection c) throws Exception {
        final String pin = prefs.getString("pin", "");
        if (pin.isEmpty()) return;
        TrustManager tm = new X509TrustManager() {
            @Override
            public void checkClientTrusted(X509Certificate[] chain, String auth) {
            }

            @Override
            public void checkServerTrusted(X509Certificate[] chain, String auth) throws java.security.cert.CertificateException {
                try {
                    if (chain == null || chain.length == 0 || !pin.equalsIgnoreCase(sha256Hex(chain[0].getEncoded()))) {
                        throw new java.security.cert.CertificateException("huella no reconocida");
                    }
                } catch (java.security.cert.CertificateException e) {
                    throw e;
                } catch (Exception e) {
                    throw new java.security.cert.CertificateException(e);
                }
            }

            @Override
            public X509Certificate[] getAcceptedIssuers() {
                return new X509Certificate[0];
            }
        };
        SSLContext ctx = SSLContext.getInstance("TLS");
        ctx.init(null, new TrustManager[]{tm}, null);
        c.setSSLSocketFactory(ctx.getSocketFactory());
        c.setHostnameVerifier(new HostnameVerifier() {
            @Override
            public boolean verify(String host, SSLSession session) {
                return true;   // la identidad la garantiza la huella anclada, no el nombre
            }
        });
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
    protected void onPause() {
        super.onPause();
        CookieManager.getInstance().flush();   // la llave del dispositivo (cookie) queda guardada
    }

    /** "tablet" si el lado corto de la pantalla mide al menos 600 dp (criterio oficial de Android). */
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
                    .put("density", m.density).put("sdk", Build.VERSION.SDK_INT).put("version", version()).put("app", "vpn")
                    .put("orientation", c.orientation == Configuration.ORIENTATION_LANDSCAPE ? "landscape" : "portrait").toString();
        } catch (Exception e) {
            return "{}";
        }
    }

    /** Colorea las barras del sistema con el tema de la webui (iconos oscuros si el tema es claro). */
    private void setBars(int color, boolean light) {
        getWindow().setStatusBarColor(color);
        getWindow().setNavigationBarColor(color);
        View d = getWindow().getDecorView();
        int f = d.getSystemUiVisibility();
        f = light ? f | View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR : f & ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
        if (Build.VERSION.SDK_INT >= 26) {
            f = light ? f | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : f & ~View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        }
        d.setSystemUiVisibility(f);
        web.setBackgroundColor(color);
    }

    @Override
    public void onConfigurationChanged(Configuration cfg) {
        super.onConfigurationChanged(cfg);   // giro, plegables, multiventana: la webui recalcula su diseno
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
        // primero la webui: cierra hojas, dialogos o imagenes ampliadas y vuelve al chat; despues, atras o salir
        web.evaluateJavascript("window.__nexusBack ? window.__nexusBack() : false", new ValueCallback<String>() {
            @Override
            public void onReceiveValue(String v) {
                if ("true".equals(v)) return;
                if (web.canGoBack()) web.goBack();
                else finish();
            }
        });
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_FILE && fileCallback != null) {
            Uri[] picked = null;
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
        } else if (req == REQ_VPN) {
            if (res == RESULT_OK) {
                startService(new Intent(this, NexusVpnService.class).setAction(NexusVpnService.ACTION_CONNECT));
            } else {
                onVpnState("error", "Sin permiso de VPN: acepta el aviso de Android para conectar con tu PC.");
            }
        }
    }

    @Override
    protected void onDestroy() {
        if (NexusVpnService.listener == this) NexusVpnService.listener = null;
        if (recognizer != null) recognizer.destroy();
        if (tts != null) tts.shutdown();
        super.onDestroy();
    }

    /** "rgba(r,g,b,a)" -> "#rrggbb" (las barras del sistema son opacas). */
    private static String rgbaToHex(String rgba) {
        String[] p = rgba.substring(rgba.indexOf('(') + 1, rgba.indexOf(')')).split(",");
        return String.format(Locale.ROOT, "#%02x%02x%02x", Integer.parseInt(p[0].trim()), Integer.parseInt(p[1].trim()), Integer.parseInt(p[2].trim()));
    }

    /** Puente JavaScript (setup.html y webui de Nexus-Office). */
    private class Bridge {
        @JavascriptInterface
        public String saved() {
            return prefs.getString("url", "");
        }

        @JavascriptInterface
        public String mode() {
            return prefs.getString("mode", "");
        }

        @JavascriptInterface
        public void open(final String url) {
            prefs.edit().putString("url", url).putString("mode", "lan").apply();
            loadOnUi(url);
        }

        /** Vuelve a la IA del propio movil (MainActivity sigue abierta debajo). */
        @JavascriptInterface
        public void local() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    finish();
                }
            });
        }

        @JavascriptInterface
        public void reset() {
            loadOnUi(SETUP);
        }

        /** Codigo NEXUSV1:... del panel VPN del PC. Devuelve un mensaje de error o "" si arranca la conexion. */
        @JavascriptInterface
        public String openVpn(String code) {
            try {
                WgConfig cfg = WgConfig.parse(code);
                prefs.edit().putString("vpn_code", cfg.toCode()).putString("mode", "vpn").remove("vpn_last_ep").apply();
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        startVpn();
                    }
                });
                return "";
            } catch (Exception e) {
                return e.getMessage() == null ? "Codigo VPN no valido" : e.getMessage();
            }
        }

        @JavascriptInterface
        public void reconnectVpn() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    startVpn();
                }
            });
        }

        @JavascriptInterface
        public String vpnState() {
            try {
                String name = "";
                if (!prefs.getString("vpn_code", "").isEmpty()) name = WgConfig.parse(prefs.getString("vpn_code", "")).name;
                return new JSONObject().put("state", NexusVpnService.state).put("detail", NexusVpnService.detail)
                        .put("endpoint", NexusVpnService.endpoint).put("configured", !name.isEmpty()).put("name", name)
                        .put("stats", NexusVpnService.stats()).toString();
            } catch (Exception e) {
                return "{}";
            }
        }

        @JavascriptInterface
        public void vpnStop() {
            NexusVpnService.stop();
            prefs.edit().putString("mode", "").apply();
            loadOnUi(SETUP);
        }

        /** Codigo NEXUSR1:... del panel de acceso remoto. Devuelve un mensaje de error o "" si arranca la conexion. */
        @JavascriptInterface
        public String openRemote(String code) {
            try {
                String body = code.trim().substring("NEXUSR1:".length());
                JSONObject o = new JSONObject(new String(Base64.decode(body, Base64.URL_SAFE | Base64.NO_PADDING | Base64.NO_WRAP), "UTF-8"));
                JSONArray urls = o.getJSONArray("u");
                String pin = o.getString("f").replace(":", "").toUpperCase(Locale.ROOT);
                if (urls.length() == 0 || pin.length() != 64) return "Codigo incompleto: genera otro en tu Nexus-Office.";
                prefs.edit().putString("remote_urls", urls.toString()).putString("pin", pin).putString("mode", "remote").remove("url").apply();
                connectRemote(o.getString("c"), "window.__nexusRemoteFail && window.__nexusRemoteFail("
                        + JSONObject.quote("Ninguna direccion responde. Comprueba que el acceso remoto esta activo y el puerto abierto.") + ")");
                return "";
            } catch (Exception e) {
                return "Codigo no valido: copialo entero desde tu Nexus-Office.";
            }
        }

        @JavascriptInterface
        public void forget() {
            NexusVpnService.stop();
            prefs.edit().remove("url").remove("remote_urls").remove("pin").remove("vpn_code").remove("vpn_last_ep").remove("mode").apply();
            CookieManager.getInstance().removeAllCookies(null);
            loadOnUi(SETUP);
        }

        @JavascriptInterface
        public String localIp() {
            try {
                for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                    if (!ni.isUp() || ni.isLoopback()) continue;
                    for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                        if (a instanceof Inet4Address && a.isSiteLocalAddress()) return a.getHostAddress();
                    }
                }
            } catch (Exception ignored) {
            }
            return "";
        }

        /** Busca servidores Nexus-Office en la subred /24 (resultado asincrono en window.__nexusScan). */
        @JavascriptInterface
        public void scan(final int port) {
            new Thread(new Runnable() {
                @Override
                public void run() {
                    final List<String> found = Collections.synchronizedList(new ArrayList<String>());
                    String ip = localIp();
                    if (!ip.isEmpty()) {
                        String prefix = ip.substring(0, ip.lastIndexOf('.') + 1);
                        ExecutorService pool = Executors.newFixedThreadPool(48);
                        for (int i = 1; i < 255; i++) {
                            final String host = prefix + i;
                            pool.execute(new Runnable() {
                                @Override
                                public void run() {
                                    probe(host, port, found);
                                }
                            });
                        }
                        pool.shutdown();
                        try {
                            pool.awaitTermination(15, TimeUnit.SECONDS);
                        } catch (InterruptedException ignored) {
                        }
                    }
                    JSONArray arr = new JSONArray();
                    for (String f : found) {
                        try {
                            arr.put(new JSONObject(f));
                        } catch (Exception ignored) {
                        }
                    }
                    js("window.__nexusScan && window.__nexusScan(" + arr + ", " + JSONObject.quote(ip) + ")");
                }
            }).start();
        }

        private void probe(String host, int port, List<String> found) {
            try {
                Socket sock = new Socket();
                try {
                    sock.connect(new InetSocketAddress(host, port), 300);
                } finally {
                    sock.close();
                }
            } catch (Exception e) {
                return;
            }
            try {
                HttpURLConnection c = (HttpURLConnection) new URL("http://" + host + ":" + port + "/api/ping").openConnection();
                c.setConnectTimeout(800);
                c.setReadTimeout(1200);
                BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream()));
                StringBuilder sb = new StringBuilder();
                String line;
                while ((line = r.readLine()) != null) sb.append(line);
                JSONObject o = new JSONObject(sb.toString());
                if (o.optBoolean("nexus")) {
                    found.add(new JSONObject().put("host", host).put("port", port)
                            .put("name", o.optString("name", "Nexus-Office")).put("version", o.optString("version")).toString());
                }
            } catch (Exception ignored) {
            }
        }

        /** Reconocimiento de voz del sistema; el texto llega a window.__nexusVoice. */
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

        /** Sintesis de voz del sistema (respaldo cuando el servidor no tiene voz local). */
        @JavascriptInterface
        public boolean canListenLive() {
            return SpeechRecognizer.isRecognitionAvailable(EscritorioActivity.this);
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

        /** Respuesta haptica corta (no necesita permisos). */
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
                        EscritorioActivity.this.setBars(c, light);
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

        /** Guarda un fichero generado en la webui (codigo, respuesta...): el sistema pregunta donde. */
        @JavascriptInterface
        public void saveFile(String name, String mime, String base64) {
            try {
                askWhereToSave(name == null || name.isEmpty() ? "nexus.txt" : name, mime == null ? "text/plain" : mime,
                        Base64.decode(base64, Base64.DEFAULT));
            } catch (Exception e) {
                js("window.__nexusSaved && window.__nexusSaved(false," + JSONObject.quote(String.valueOf(name)) + ")");
            }
        }

        @JavascriptInterface
        public void openTermux() {
            Intent i = getPackageManager().getLaunchIntentForPackage("com.termux");
            if (i != null) startActivity(i);
            else openExternal("https://f-droid.org/packages/com.termux/");
        }

        @JavascriptInterface
        public void openUrl(String url) {
            openExternal(url);
        }
    }
}
