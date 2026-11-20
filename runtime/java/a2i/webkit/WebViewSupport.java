package a2i.webkit;

import a2i.boot.Neutral;

import android.content.Context;
import android.os.Build;
import android.view.View;
import android.webkit.CookieManager;
import android.webkit.GeolocationPermissions;
import android.webkit.WebSettings;
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewDatabase;
import android.webkit.WebViewFactoryProvider;
import android.webkit.WebViewProvider;

import java.lang.reflect.Field;

/**
 * The WebView implementation.
 *
 * android.webkit.WebView is a shell: everything it does is delegated to a provider, which
 * on a device is Chromium, loaded from its own APK. This class installs the provider used
 * here. Pages are shown by the host platform's own web view ({@link HostWebView}); where
 * there is no host (the command-line launcher), a WebView can be created and laid out but
 * stays blank. The cookie store is kept in step with the host's, so that an app can use
 * what a page set (a login, a passed bot check) in its own HTTP requests.
 */
public final class WebViewSupport {
    private WebViewSupport() {}

    /** What connects web views and cookies to the host platform; all null without one. */
    public interface CookieHost {
        void setCookie(String domain, String path, String name, String value, boolean secure, long expiresAt);

        void clearCookies();

        /** Asks for the host's cookies; they arrive through {@link #onHostCookies}. */
        void requestCookies();
    }

    private static HostWebView.Host sHost;
    private static CookieHost sCookieHost;
    private static final A2ICookieManager sCookies = new A2ICookieManager();

    /** Connects the host platform's web view. Call before {@link #install}. */
    public static void setHost(HostWebView.Host host, CookieHost cookieHost) {
        sHost = host;
        sCookieHost = cookieHost;
    }

    static CookieHost cookieHost() {
        return sCookieHost;
    }

    /**
     * The host's cookies, one per line: domain, path, name, value, "1" if secure, expiry
     * in milliseconds (0 for a session cookie), "1" if for the exact host only, separated
     * by tabs. May be called on any thread.
     */
    public static void onHostCookies(String cookies) {
        sCookies.mergeFromHost(cookies);
    }

    /** Makes this the process's WebView provider. Call before the app starts. */
    public static void install() throws ReflectiveOperationException {
        if (sHost != null) {
            android.view.A2IWindowManager.setOnWindowsChanged(HostWebView::updateAll);
        }
        if (sCookieHost != null) {
            sCookieHost.requestCookies();
        }
        Field provider = Class.forName("android.webkit.WebViewFactory").getDeclaredField("sProviderInstance");
        provider.setAccessible(true);
        provider.set(null, Neutral.proxy(WebViewFactoryProvider.class, new Factory()));
    }

    /**
     * What Chrome on Android sends, for this Android version. Apps hand it to their own
     * HTTP clients so that servers treat them like the WebView.
     */
    static String defaultUserAgent() {
        return "Mozilla/5.0 (Linux; Android " + Build.VERSION.RELEASE + "; " + Build.MODEL
                + ") AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/130.0.0.0 Mobile Safari/537.36";
    }

    /** android.webkit.WebViewFactoryProvider. */
    public static final class Factory {
        private final CookieManager mCookies = sCookies;
        private final WebStorage mStorage = new WebStorage();
        private final GeolocationPermissions mGeolocation = new GeolocationPermissions();
        private final WebViewDatabase mDatabase = new NeutralWebViewDatabase();
        private final WebViewFactoryProvider.Statics mStatics =
                Neutral.proxy(WebViewFactoryProvider.Statics.class, new Statics());

        public WebViewFactoryProvider.Statics getStatics() {
            return mStatics;
        }

        public WebViewProvider createWebView(WebView webView, WebView.PrivateAccess privateAccess) {
            return Neutral.proxy(WebViewProvider.class, sHost != null
                    ? new HostWebView(sHost, webView, privateAccess)
                    : new BlankWebView(webView, privateAccess));
        }

        public CookieManager getCookieManager() {
            return mCookies;
        }

        public WebStorage getWebStorage() {
            return mStorage;
        }

        public GeolocationPermissions getGeolocationPermissions() {
            return mGeolocation;
        }

        public WebViewDatabase getWebViewDatabase(Context context) {
            return mDatabase;
        }

        public ClassLoader getWebViewClassLoader() {
            return WebViewSupport.class.getClassLoader();
        }
    }

    /** android.webkit.WebViewFactoryProvider.Statics. */
    public static final class Statics {
        public String getDefaultUserAgent(Context context) {
            return defaultUserAgent();
        }
    }

    /** android.webkit.WebViewProvider for a WebView that exists and has a size, but no content. */
    public static final class BlankWebView {
        private final WebView mWebView;
        private final WebSettings mSettings = new A2IWebSettings();
        private final WebViewProvider.ViewDelegate mViewDelegate;
        private final WebViewProvider.ScrollDelegate mScrollDelegate =
                Neutral.proxy(WebViewProvider.ScrollDelegate.class, null);

        BlankWebView(WebView webView, WebView.PrivateAccess privateAccess) {
            mWebView = webView;
            mViewDelegate = Neutral.proxy(WebViewProvider.ViewDelegate.class, new ViewDelegate(privateAccess));
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

        public View findHierarchyView(String className, int hashCode) {
            return null;
        }
    }

    /** The part of a view's behaviour that WebView leaves to its provider. */
    public static class ViewDelegate {
        private final WebView.PrivateAccess mAccess;

        ViewDelegate(WebView.PrivateAccess access) {
            mAccess = access;
        }

        /** A view must report a size when measured; this one takes all it is offered. */
        public void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            mAccess.setMeasuredDimension(View.MeasureSpec.getSize(widthMeasureSpec),
                    View.MeasureSpec.getSize(heightMeasureSpec));
        }

        public boolean setFrame(int left, int top, int right, int bottom) {
            return mAccess.super_setFrame(left, top, right, bottom);
        }

        // What WebView routes through its provider and a provider normally passes back to
        // the View base class.
        public void setLayoutParams(android.view.ViewGroup.LayoutParams params) {
            mAccess.super_setLayoutParams(params);
        }

        public boolean performLongClick() {
            return mAccess.super_performLongClick();
        }

        public boolean dispatchKeyEvent(android.view.KeyEvent event) {
            return mAccess.super_dispatchKeyEvent(event);
        }

        public boolean requestFocus(int direction, android.graphics.Rect previouslyFocusedRect) {
            return mAccess.super_requestFocus(direction, previouslyFocusedRect);
        }

        public boolean onHoverEvent(android.view.MotionEvent event) {
            return mAccess.super_onHoverEvent(event);
        }

        public boolean onGenericMotionEvent(android.view.MotionEvent event) {
            return mAccess.super_onGenericMotionEvent(event);
        }

        public boolean performAccessibilityAction(int action, android.os.Bundle arguments) {
            return mAccess.super_performAccessibilityAction(action, arguments);
        }

        public void onDrawVerticalScrollBar(android.graphics.Canvas canvas,
                android.graphics.drawable.Drawable scrollBar, int l, int t, int r, int b) {
            mAccess.super_onDrawVerticalScrollBar(canvas, scrollBar, l, t, r, b);
        }

        public android.os.Handler getHandler(android.os.Handler originalHandler) {
            return originalHandler;
        }

        public View findFocus(View originalFocusedView) {
            return originalFocusedView;
        }
    }
}
