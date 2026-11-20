package a2i.webkit;

import a2i.boot.Neutral;

import android.graphics.Rect;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;
import android.view.A2IWindowManager;
import android.view.View;
import android.webkit.DownloadListener;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewProvider;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * android.webkit.WebViewProvider backed by the host platform's web view.
 *
 * The page is not drawn by Android: the host puts its own web view on screen, in front of
 * the Android screen, at the place where the WebView widget is, and handles touches and
 * typing in it itself. This class keeps the two in step. It tells the host where the
 * widget is and whether it can be seen, forwards what the app asks of the WebView
 * (loading, navigation, JavaScript), and turns what the host reports back (navigation,
 * progress, titles, errors) into the WebViewClient and WebChromeClient callbacks the app
 * registered.
 *
 * Because the page is in front of the Android screen, nothing Android draws can appear on
 * top of it. While another window (a dialog, a menu) is above the WebView's window, the
 * host view is therefore hidden.
 *
 * All methods are called on Android's main thread.
 */
public final class HostWebView {
    private static final String TAG = "a2i.WebView";

    /** The host platform's web views, addressed by id. Implemented by the iOS host. */
    public interface Host {
        void create(int id);

        void destroy(int id);

        /** Where the web view is on the Android screen, in pixels, and whether it is to be seen. */
        void setFrame(int id, int x, int y, int width, int height, boolean visible);

        void configure(int id, String userAgent, boolean javaScriptEnabled);

        /** {@code headers} alternates names and values. */
        void loadUrl(int id, String url, String[] headers);

        void loadData(int id, String data, String mimeType, String encoding, String baseUrl);

        /** The result arrives as {@link #EVENT_SCRIPT_RESULT} with {@code callback}. */
        void evaluateJavascript(int id, String script, int callback);

        /** One of the COMMAND_* constants. */
        void command(int id, int command);

        /** Makes {@code window.<name>} exist in pages; calls on it arrive as {@link #EVENT_SCRIPT_CALL}. */
        void addJavascriptObject(int id, String name);

        /** Answers {@link #EVENT_NAVIGATE}. */
        void navigationDecision(int id, int token, boolean allow);
    }

    public static final int COMMAND_GO_BACK = 1;
    public static final int COMMAND_GO_FORWARD = 2;
    public static final int COMMAND_RELOAD = 3;
    public static final int COMMAND_STOP = 4;

    // Events from the host; see dispatchEvent for their arguments.
    public static final int EVENT_PAGE_STARTED = 1;
    public static final int EVENT_PAGE_FINISHED = 2;
    public static final int EVENT_PROGRESS = 3;
    public static final int EVENT_TITLE = 4;
    public static final int EVENT_ERROR = 5;
    public static final int EVENT_SCRIPT_RESULT = 6;
    public static final int EVENT_SCRIPT_CALL = 7;
    public static final int EVENT_NAVIGATE = 8;
    public static final int EVENT_HISTORY = 9;
    public static final int EVENT_PAGE_VISIBLE = 10;

    private static final SparseArray<HostWebView> sViews = new SparseArray<>();
    private static int sNextId = 1;

    private final Host mHost;
    private final int mId;
    private final WebView mWebView;
    private final A2IWebSettings mSettings = new A2IWebSettings();
    private final WebViewProvider.ViewDelegate mViewDelegate;
    private final WebViewProvider.ScrollDelegate mScrollDelegate =
            Neutral.proxy(WebViewProvider.ScrollDelegate.class, null);
    private final Map<String, Object> mJavascriptObjects = new HashMap<>();
    private final SparseArray<ValueCallback<String>> mScriptCallbacks = new SparseArray<>();
    private final Rect mFrame = new Rect();
    private WebViewClient mClient = new WebViewClient();
    private WebChromeClient mChromeClient;
    private DownloadListener mDownloadListener;
    private String mUrl;
    private String mOriginalUrl;
    private String mTitle;
    private int mProgress = 100;
    private boolean mCanGoBack;
    private boolean mCanGoForward;
    private boolean mShown;
    private boolean mAttached;
    private boolean mPaused;
    private boolean mDestroyed;
    private int mNextScriptCallback = 1;

    HostWebView(Host host, WebView webView, WebView.PrivateAccess access) {
        mHost = host;
        mId = sNextId++;
        mWebView = webView;
        mViewDelegate = Neutral.proxy(WebViewProvider.ViewDelegate.class, new Delegate(access));
        sViews.put(mId, this);
        host.create(mId);
    }

    // --- Geometry ---------------------------------------------------------------------------

    /** Re-evaluates every web view's place on screen; called when the window stack changes. */
    static void updateAll() {
        for (int i = 0; i < sViews.size(); i++) {
            sViews.valueAt(i).updateFrame();
        }
    }

    private void updateFrame() {
        if (mDestroyed) {
            return;
        }
        boolean shown = mAttached && mWebView.isShown() && mWebView.getWindowVisibility() == View.VISIBLE
                && mWebView.getWidth() > 0 && mWebView.getHeight() > 0
                && A2IWindowManager.getInstance().isInTopWindow(mWebView);
        int[] location = new int[2];
        if (shown) {
            mWebView.getLocationOnScreen(location);
        }
        Rect frame = new Rect(location[0], location[1], location[0] + mWebView.getWidth(),
                location[1] + mWebView.getHeight());
        if (shown != mShown || (shown && !frame.equals(mFrame))) {
            mShown = shown;
            mFrame.set(frame);
            mHost.setFrame(mId, frame.left, frame.top, frame.width(), frame.height(), shown);
        }
    }

    /** The view-level half of the provider: the base class forwarding plus the hooks that track geometry. */
    public final class Delegate extends WebViewSupport.ViewDelegate {
        Delegate(WebView.PrivateAccess access) {
            super(access);
        }

        public void onDraw(android.graphics.Canvas canvas) {
            // Drawn whenever its window is; the cheapest reliable moment to notice that
            // the widget moved (a parent scrolled, a layout changed).
            updateFrame();
        }

        public void onAttachedToWindow() {
            mAttached = true;
            updateFrame();
        }

        public void onDetachedFromWindow() {
            mAttached = false;
            updateFrame();
        }

        public void onVisibilityChanged(View changedView, int visibility) {
            updateFrame();
        }

        public void onWindowVisibilityChanged(int visibility) {
            updateFrame();
        }

        public void onSizeChanged(int w, int h, int oldWidth, int oldHeight) {
            updateFrame();
        }
    }

    // --- WebViewProvider: what the app asks of the WebView ------------------------------------

    public void init(Map<String, Object> javaScriptInterfaces, boolean privateBrowsing) {
        if (javaScriptInterfaces != null) {
            for (Map.Entry<String, Object> entry : javaScriptInterfaces.entrySet()) {
                addJavascriptInterface(entry.getValue(), entry.getKey());
            }
        }
    }

    public WebSettings getSettings() {
        return mSettings;
    }

    public WebViewProvider.ViewDelegate getViewDelegate() {
        return mViewDelegate;
    }

    public WebViewProvider.ScrollDelegate getScrollDelegate() {
        return mScrollDelegate;
    }

    private void applySettings() {
        mHost.configure(mId, mSettings.getUserAgentString(), mSettings.getJavaScriptEnabled());
    }

    public void loadUrl(String url) {
        loadUrl(url, null);
    }

    public void loadUrl(String url, Map<String, String> additionalHttpHeaders) {
        if (mDestroyed || url == null) {
            return;
        }
        if (url.startsWith("javascript:")) {
            evaluateJavaScript(Uri.decode(url.substring("javascript:".length())), null);
            return;
        }
        List<String> headers = new ArrayList<>();
        if (additionalHttpHeaders != null) {
            for (Map.Entry<String, String> header : additionalHttpHeaders.entrySet()) {
                headers.add(header.getKey());
                headers.add(header.getValue());
            }
        }
        applySettings();
        mOriginalUrl = url;
        mHost.loadUrl(mId, url, headers.toArray(new String[0]));
    }

    public void loadData(String data, String mimeType, String encoding) {
        loadDataWithBaseURL(null, data, mimeType, encoding, null);
    }

    public void loadDataWithBaseURL(String baseUrl, String data, String mimeType, String encoding,
            String historyUrl) {
        if (mDestroyed) {
            return;
        }
        applySettings();
        mHost.loadData(mId, data != null ? data : "", mimeType != null ? mimeType : "text/html",
                encoding != null ? encoding : "utf-8", baseUrl);
    }

    public void evaluateJavaScript(String script, ValueCallback<String> resultCallback) {
        if (mDestroyed) {
            return;
        }
        int callback = 0;
        if (resultCallback != null) {
            callback = mNextScriptCallback++;
            mScriptCallbacks.put(callback, resultCallback);
        }
        mHost.evaluateJavascript(mId, script, callback);
    }

    public void stopLoading() {
        command(COMMAND_STOP);
    }

    public void reload() {
        applySettings();
        command(COMMAND_RELOAD);
    }

    public boolean canGoBack() {
        return mCanGoBack;
    }

    public void goBack() {
        command(COMMAND_GO_BACK);
    }

    public boolean canGoForward() {
        return mCanGoForward;
    }

    public void goForward() {
        command(COMMAND_GO_FORWARD);
    }

    public boolean canGoBackOrForward(int steps) {
        return steps < 0 ? mCanGoBack : steps > 0 ? mCanGoForward : true;
    }

    public void goBackOrForward(int steps) {
        for (int i = 0; i < Math.abs(steps); i++) {
            command(steps < 0 ? COMMAND_GO_BACK : COMMAND_GO_FORWARD);
        }
    }

    private void command(int command) {
        if (!mDestroyed) {
            mHost.command(mId, command);
        }
    }

    public String getUrl() {
        return mUrl;
    }

    public String getOriginalUrl() {
        return mOriginalUrl != null ? mOriginalUrl : mUrl;
    }

    public String getTitle() {
        return mTitle;
    }

    public int getProgress() {
        return mProgress;
    }

    public float getScale() {
        return mWebView.getResources().getDisplayMetrics().density;
    }

    public void onPause() {
        mPaused = true;
    }

    public void onResume() {
        mPaused = false;
    }

    public boolean isPaused() {
        return mPaused;
    }

    public void destroy() {
        if (!mDestroyed) {
            mDestroyed = true;
            sViews.remove(mId);
            mHost.destroy(mId);
        }
    }

    public void setWebViewClient(WebViewClient client) {
        mClient = client != null ? client : new WebViewClient();
    }

    public WebViewClient getWebViewClient() {
        return mClient;
    }

    public void setWebChromeClient(WebChromeClient client) {
        mChromeClient = client;
    }

    public WebChromeClient getWebChromeClient() {
        return mChromeClient;
    }

    public void setDownloadListener(DownloadListener listener) {
        mDownloadListener = listener;
    }

    /**
     * Exposes an object's @JavascriptInterface methods to pages as {@code window.<name>}.
     * Calls from the page are delivered asynchronously, so, unlike on Android, a page
     * cannot use a method's return value.
     */
    public void addJavascriptInterface(Object object, String name) {
        if (object != null && name != null && !mDestroyed) {
            mJavascriptObjects.put(name, object);
            mHost.addJavascriptObject(mId, name);
        }
    }

    public void removeJavascriptInterface(String name) {
        mJavascriptObjects.remove(name);
    }

    public View findHierarchyView(String className, int hashCode) {
        return null;
    }

    // --- Events from the host ---------------------------------------------------------------

    /** Delivers a host event to the web view it is for. Call on Android's main thread. */
    public static void dispatchEvent(int id, int event, String a, String b, int n) {
        HostWebView view = sViews.get(id);
        if (view != null) {
            view.onEvent(event, a, b, n);
        }
    }

    private void onEvent(int event, String a, String b, int n) {
        switch (event) {
            case EVENT_PAGE_STARTED:  // a: URL
                mUrl = a;
                mProgress = 10;
                mClient.onPageStarted(mWebView, a, null);
                break;
            case EVENT_PAGE_VISIBLE:  // a: URL
                mClient.onPageCommitVisible(mWebView, a);
                break;
            case EVENT_PAGE_FINISHED:  // a: URL
                mUrl = a;
                mProgress = 100;
                if (mChromeClient != null) {
                    mChromeClient.onProgressChanged(mWebView, 100);
                }
                mClient.onPageFinished(mWebView, a);
                break;
            case EVENT_PROGRESS:  // n: percent
                mProgress = n;
                if (mChromeClient != null) {
                    mChromeClient.onProgressChanged(mWebView, n);
                }
                break;
            case EVENT_TITLE:  // a: title
                mTitle = a;
                if (mChromeClient != null) {
                    mChromeClient.onReceivedTitle(mWebView, a);
                }
                break;
            case EVENT_HISTORY:  // a: current URL; n: bit 0 can go back, bit 1 can go forward
                if (a != null) {
                    mUrl = a;
                }
                mCanGoBack = (n & 1) != 0;
                mCanGoForward = (n & 2) != 0;
                break;
            case EVENT_ERROR: {  // a: description; b: failing URL; n: WebViewClient.ERROR_* code
                final String description = a;
                final int code = n;
                mClient.onReceivedError(mWebView, new Request(b, true, false), new WebResourceError() {
                    @Override
                    public int getErrorCode() {
                        return code;
                    }

                    @Override
                    public CharSequence getDescription() {
                        return description;
                    }
                });
                break;
            }
            case EVENT_SCRIPT_RESULT: {  // a: the result as JSON; n: callback
                ValueCallback<String> callback = mScriptCallbacks.get(n);
                mScriptCallbacks.remove(n);
                if (callback != null) {
                    callback.onReceiveValue(a != null ? a : "null");
                }
                break;
            }
            case EVENT_SCRIPT_CALL:  // a: object name; b: JSON {"method": ..., "args": [...]}
                callJavascriptObject(a, b);
                break;
            case EVENT_NAVIGATE: {  // a: URL; b: "redirect" or "link"; n: token to answer with
                boolean override = mClient.shouldOverrideUrlLoading(mWebView,
                        new Request(a, true, "redirect".equals(b)));
                if (!mDestroyed) {
                    mHost.navigationDecision(mId, n, !override);
                }
                break;
            }
            default:
                break;
        }
    }

    private void callJavascriptObject(String name, String call) {
        Object object = mJavascriptObjects.get(name);
        if (object == null) {
            return;
        }
        try {
            JSONObject json = new JSONObject(call);
            String methodName = json.getString("method");
            JSONArray args = json.optJSONArray("args");
            int count = args != null ? args.length() : 0;
            for (Method method : object.getClass().getMethods()) {
                if (!method.getName().equals(methodName) || method.getParameterCount() != count
                        || !method.isAnnotationPresent(JavascriptInterface.class)) {
                    continue;
                }
                Class<?>[] types = method.getParameterTypes();
                Object[] values = new Object[count];
                for (int i = 0; i < count; i++) {
                    values[i] = convert(args.isNull(i) ? null : args.get(i), types[i]);
                }
                // Android calls these on a background thread; so does this.
                new Thread(() -> {
                    try {
                        method.invoke(object, values);
                    } catch (ReflectiveOperationException | RuntimeException e) {
                        Log.w(TAG, "JavaScript interface " + name + "." + methodName + " failed", e);
                    }
                }, "JavaBridge").start();
                return;
            }
            Log.w(TAG, "page called unknown method " + name + "." + methodName + "/" + count);
        } catch (JSONException e) {
            Log.w(TAG, "malformed call to " + name, e);
        }
    }

    private static Object convert(Object value, Class<?> type) {
        if (type == String.class) {
            return value != null ? value.toString() : null;
        }
        if (type == boolean.class || type == Boolean.class) {
            return value instanceof Boolean ? value : Boolean.FALSE;
        }
        double number = value instanceof Number ? ((Number) value).doubleValue() : 0;
        if (type == int.class || type == Integer.class) return (int) number;
        if (type == long.class || type == Long.class) return (long) number;
        if (type == float.class || type == Float.class) return (float) number;
        if (type == double.class || type == Double.class) return number;
        return value;
    }

    private static final class Request implements WebResourceRequest {
        private final Uri mUri;
        private final boolean mMainFrame;
        private final boolean mRedirect;

        Request(String url, boolean mainFrame, boolean redirect) {
            mUri = Uri.parse(url != null ? url : "about:blank");
            mMainFrame = mainFrame;
            mRedirect = redirect;
        }

        @Override
        public Uri getUrl() {
            return mUri;
        }

        @Override
        public boolean isForMainFrame() {
            return mMainFrame;
        }

        @Override
        public boolean isRedirect() {
            return mRedirect;
        }

        @Override
        public boolean hasGesture() {
            return !mRedirect;
        }

        @Override
        public String getMethod() {
            return "GET";
        }

        @Override
        public Map<String, String> getRequestHeaders() {
            return Collections.emptyMap();
        }
    }

    static Handler mainHandler() {
        return new Handler(Looper.getMainLooper());
    }
}
