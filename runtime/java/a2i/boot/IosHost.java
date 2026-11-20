package a2i.boot;

import a2i.webkit.HostWebView;
import a2i.webkit.WebViewSupport;

import android.app.A2IActivityTaskManager;
import android.app.ActivityThread;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Handler;
import android.util.Log;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The iOS app's entry points into the Android side. The native host (runtime/host) calls
 * {@link #run} on the thread it dedicates to Android and the event methods ({@link #touch},
 * {@link #back}, {@link #setForeground}) from the UI thread, and implements the native
 * methods.
 */
public final class IosHost {
    private IosHost() {}

    /**
     * Starts the app and runs Android's main loop on the calling thread. Does not return
     * while the app is running.
     *
     * @param width window width in pixels (the iOS view's size times its scale)
     * @param densityDpi 160 times the iOS screen scale
     */
    public static void run(String androidRoot, String nativeLibraryDir, String dataDir, String apkPath,
            int width, int height, int densityDpi) throws Exception {
        AppRuntime.Config config = new AppRuntime.Config();
        config.androidRoot = androidRoot;
        config.nativeLibraryDir = nativeLibraryDir;
        config.dataDir = dataDir;
        config.apkPath = apkPath;
        config.appLibraryDir = nativeLibraryDir;  // the packer puts them next to the runtime's
        config.width = width;
        config.height = height;
        config.densityDpi = densityDpi;

        AppRuntime.setExternalStarter(new ExternalActivities(dataDir));
        WebViewSupport.setHost(new WebViews(), new Cookies());
        AppRuntime.setKeyboard(new InputMethodService.Keyboard() {
            @Override
            public void show(int inputType, int imeOptions, int fieldBottom) {
                showKeyboard(inputType, imeOptions, fieldBottom);
            }

            @Override
            public void hide() {
                hideKeyboard();
            }
        });
        final int[] pixels = new int[width * height];
        AppRuntime.run(config, (Bitmap frame) -> {
            // ARGB ints. (Handing the native side the bitmap's memory directly would avoid
            // this conversion, but the prebuilt graphics library offers no usable way.)
            long t0 = System.nanoTime();
            frame.getPixels(pixels, 0, width, 0, 0, width, height);
            long t1 = System.nanoTime();
            presentFrame(pixels, width, height);
            FrameStats.add(FrameStats.READ_PIXELS, t1 - t0);
            FrameStats.add(FrameStats.PRESENT, System.nanoTime() - t1);
            FrameStats.frame();
        }, null);
    }

    /** Delivers a touch from the iOS view; {@code action} is a MotionEvent.ACTION_* value. */
    public static void touch(int action, float x, float y) {
        AppRuntime.injectTouch(action, x, y);
    }

    /**
     * The back gesture: iOS has no back key, so the host maps a swipe from the left edge
     * to it. As on Android, it first puts the keyboard away if that is showing.
     */
    public static void back() {
        AppRuntime.withInputMethod(inputMethod -> {
            if (inputMethod.isKeyboardShown()) {
                inputMethod.dismiss();
            } else {
                AppRuntime.injectKey(android.view.KeyEvent.KEYCODE_BACK);
            }
        });
    }

    /**
     * Text from the iOS keyboard. With {@code composing}, it is the text of an unfinished
     * composition (replacing the previous one); otherwise it is final.
     */
    public static void keyboardText(String text, boolean composing) {
        AppRuntime.withInputMethod(inputMethod -> {
            if (composing) {
                inputMethod.setComposingText(text);
            } else {
                inputMethod.commitText(text);
            }
        });
    }

    /** The iOS keyboard's delete key. */
    public static void keyboardDelete() {
        AppRuntime.withInputMethod(InputMethodService::deleteBackward);
    }

    /** The iOS keyboard's return key. */
    public static void keyboardReturn() {
        AppRuntime.withInputMethod(InputMethodService::enter);
    }

    /**
     * Shows the iOS keyboard for a field with the given EditorInfo.inputType and
     * imeOptions, keeping the field's bottom edge (a y coordinate in pixels) in view.
     * Implemented by the host.
     */
    private static native void showKeyboard(int inputType, int imeOptions, int fieldBottom);

    /** Hides the iOS keyboard. Implemented by the host. */
    private static native void hideKeyboard();

    /** The iOS app left the screen (false) or came back (true). Blocks until the app has reacted. */
    public static void setForeground(boolean foreground) {
        AppRuntime.setForeground(foreground, 3000);
    }

    /**
     * Intents that no activity of the app takes are answered with what iOS offers for the
     * same purpose: links and mail/phone/message addresses are opened by the system, and
     * text or a file to share goes to the share sheet.
     */
    private static final class ExternalActivities implements A2IActivityTaskManager.ExternalStarter {
        private final File mShareDir;

        ExternalActivities(String dataDir) {
            mShareDir = new File(dataDir, "shared");
        }

        private static boolean isShare(Intent intent) {
            return Intent.ACTION_SEND.equals(intent.getAction())
                    && (intent.hasExtra(Intent.EXTRA_STREAM) || intent.hasExtra(Intent.EXTRA_TEXT));
        }

        private static boolean isAddress(Intent intent) {
            String action = intent.getAction();
            Uri data = intent.getData();
            if (data == null || data.getScheme() == null) {
                return false;
            }
            if (!Intent.ACTION_VIEW.equals(action) && !Intent.ACTION_SENDTO.equals(action)
                    && !Intent.ACTION_DIAL.equals(action)) {
                return false;
            }
            switch (data.getScheme()) {
                case "http":
                case "https":
                case "mailto":
                case "tel":
                case "sms":
                    return true;
                default:
                    return false;
            }
        }

        @Override
        public boolean canStartActivity(Intent intent) {
            return isShare(intent) || isAddress(intent);
        }

        @Override
        public Instrumentation.ActivityResult startActivity(Intent intent) {
            if (isAddress(intent)) {
                openUrl(intent.getData().toString());
                return null;
            }
            String subject = intent.getStringExtra(Intent.EXTRA_SUBJECT);
            CharSequence text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT);
            Uri stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            String file = null;
            if (stream != null) {
                try {
                    file = copyForSharing(stream, intent.getType());
                } catch (IOException | RuntimeException e) {
                    Log.e("a2i.IosHost", "cannot read " + stream + " for sharing", e);
                }
            }
            if (file != null || text != null) {
                share(text != null ? text.toString() : null, file, subject);
            }
            return null;
        }

        /**
         * The share sheet takes files, not content: URIs, so the content is read from the
         * app's provider into a file of the right type.
         */
        private String copyForSharing(Uri uri, String mimeType) throws IOException {
            Context context = ActivityThread.currentApplication();
            String name = uri.getLastPathSegment();
            if (name == null || name.isEmpty()) {
                name = "shared";
            }
            name = name.replace('/', '_');
            if (name.indexOf('.') < 0) {
                String extension = MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType);
                if (extension != null) {
                    name += "." + extension;
                }
            }
            mShareDir.mkdirs();
            File file = new File(mShareDir, name);
            try (InputStream in = context.getContentResolver().openInputStream(uri);
                    OutputStream out = new FileOutputStream(file)) {
                if (in == null) {
                    throw new IOException("no content");
                }
                byte[] buffer = new byte[64 * 1024];
                for (int n; (n = in.read(buffer)) > 0;) {
                    out.write(buffer, 0, n);
                }
            }
            return file.getPath();
        }
    }

    // --- Web views: WKWebView, see runtime/host/webview.m ---------------------------------

    /** a2i.webkit.HostWebView.Host, on the host's natives. */
    private static final class WebViews implements HostWebView.Host {
        @Override
        public void create(int id) {
            webViewCreate(id);
        }

        @Override
        public void destroy(int id) {
            webViewDestroy(id);
        }

        @Override
        public void setFrame(int id, int x, int y, int width, int height, boolean visible) {
            webViewSetFrame(id, x, y, width, height, visible);
        }

        @Override
        public void configure(int id, String userAgent, boolean javaScriptEnabled) {
            webViewConfigure(id, userAgent, javaScriptEnabled);
        }

        @Override
        public void loadUrl(int id, String url, String[] headers) {
            webViewLoadUrl(id, url, headers);
        }

        @Override
        public void loadData(int id, String data, String mimeType, String encoding, String baseUrl) {
            webViewLoadData(id, data, mimeType, encoding, baseUrl);
        }

        @Override
        public void evaluateJavascript(int id, String script, int callback) {
            webViewEvaluate(id, script, callback);
        }

        @Override
        public void command(int id, int command) {
            webViewCommand(id, command);
        }

        @Override
        public void addJavascriptObject(int id, String name) {
            webViewAddJavascriptObject(id, name);
        }

        @Override
        public void navigationDecision(int id, int token, boolean allow) {
            webViewNavigationDecision(id, token, allow);
        }
    }

    private static final class Cookies implements WebViewSupport.CookieHost {
        @Override
        public void setCookie(String domain, String path, String name, String value, boolean secure,
                long expiresAt) {
            webViewSetCookie(domain, path, name, value, secure, expiresAt);
        }

        @Override
        public void clearCookies() {
            webViewClearCookies();
        }

        @Override
        public void requestCookies() {
            webViewRequestCookies();
        }
    }

    /**
     * Something happened in a web view (a2i.webkit.HostWebView.EVENT_*). Called by the
     * host on the UI thread; handled on Android's main thread, in order.
     */
    public static void webViewEvent(int id, int event, String a, String b, int n) {
        Handler handler = AppRuntime.mainHandler();
        if (handler != null) {
            handler.post(() -> HostWebView.dispatchEvent(id, event, a, b, n));
        }
    }

    /**
     * The web views' cookies, in the format of WebViewSupport.onHostCookies. Called by
     * the host on the UI thread, and applied at once: a page event that follows must
     * already see them.
     */
    public static void webViewCookies(String cookies) {
        WebViewSupport.onHostCookies(cookies);
    }

    private static native void webViewCreate(int id);
    private static native void webViewDestroy(int id);
    private static native void webViewSetFrame(int id, int x, int y, int width, int height, boolean visible);
    private static native void webViewConfigure(int id, String userAgent, boolean javaScriptEnabled);
    private static native void webViewLoadUrl(int id, String url, String[] headers);
    private static native void webViewLoadData(int id, String data, String mimeType, String encoding,
            String baseUrl);
    private static native void webViewEvaluate(int id, String script, int callback);
    private static native void webViewCommand(int id, int command);
    private static native void webViewAddJavascriptObject(int id, String name);
    private static native void webViewNavigationDecision(int id, int token, boolean allow);
    private static native void webViewSetCookie(String domain, String path, String name, String value,
            boolean secure, long expiresAt);
    private static native void webViewClearCookies();
    private static native void webViewRequestCookies();

    /** Opens a URL with the iOS app registered for it. Implemented by the host. */
    private static native void openUrl(String url);

    /** Presents the iOS share sheet for text, a file, or both. Implemented by the host. */
    private static native void share(String text, String filePath, String subject);

    /** Shows a frame of the window. Implemented by the host; called on Android's main thread. */
    private static native void presentFrame(int[] argbPixels, int width, int height);
}
