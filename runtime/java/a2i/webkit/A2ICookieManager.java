package a2i.webkit;

import android.net.Uri;
import android.webkit.CookieManager;
import android.webkit.ValueCallback;
import android.webkit.WebView;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

/**
 * android.webkit.CookieManager: the cookie store that WebViews use and that apps read and
 * write directly, typically to share a login between a web page and their own HTTP
 * client. This is a mirror of the host web view's cookie store where there is one (what
 * is set here is passed on, and the host reports its cookies back), and otherwise just
 * lives in memory.
 */
public final class A2ICookieManager extends CookieManager {
    private static final class Cookie {
        String name;
        String value;
        String domain;       // lower case, without a leading dot
        boolean hostOnly;    // no Domain attribute: only sent to the exact host
        String path = "/";
        boolean secure;
        long expiresAt = Long.MAX_VALUE;  // milliseconds; MAX_VALUE for a session cookie
    }

    private final List<Cookie> mCookies = new ArrayList<>();
    private boolean mAccept = true;
    private boolean mAcceptFileScheme;

    @Override
    public synchronized void setAcceptCookie(boolean accept) {
        mAccept = accept;
    }

    @Override
    public synchronized boolean acceptCookie() {
        return mAccept;
    }

    @Override
    public void setAcceptThirdPartyCookies(WebView webview, boolean accept) {}

    @Override
    public boolean acceptThirdPartyCookies(WebView webview) {
        return true;
    }

    @Override
    public void setCookie(String url, String value) {
        setCookie(url, value, null);
    }

    @Override
    public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
        boolean stored = store(url, value);
        if (callback != null) {
            callback.onReceiveValue(stored);
        }
    }

    /** Parses a Set-Cookie value ("name=value; Domain=...; Path=...; Max-Age=...") received from {@code url}. */
    private synchronized boolean store(String url, String header) {
        Uri uri = url != null ? Uri.parse(url.contains("://") ? url : "http://" + url) : null;
        String host = uri != null && uri.getHost() != null ? uri.getHost().toLowerCase() : null;
        if (!mAccept || host == null || header == null) {
            return false;
        }
        String[] parts = header.split(";");
        int equals = parts[0].indexOf('=');
        if (equals <= 0) {
            return false;
        }
        Cookie cookie = new Cookie();
        cookie.name = parts[0].substring(0, equals).trim();
        cookie.value = parts[0].substring(equals + 1).trim();
        cookie.domain = host;
        cookie.hostOnly = true;
        for (int i = 1; i < parts.length; i++) {
            String attribute = parts[i].trim();
            int at = attribute.indexOf('=');
            String key = (at < 0 ? attribute : attribute.substring(0, at)).trim().toLowerCase();
            String attributeValue = at < 0 ? "" : attribute.substring(at + 1).trim();
            switch (key) {
                case "domain":
                    if (!attributeValue.isEmpty()) {
                        cookie.domain = attributeValue.toLowerCase().replaceFirst("^\\.", "");
                        cookie.hostOnly = false;
                    }
                    break;
                case "path":
                    if (attributeValue.startsWith("/")) {
                        cookie.path = attributeValue;
                    }
                    break;
                case "secure":
                    cookie.secure = true;
                    break;
                case "max-age":
                    try {
                        cookie.expiresAt = System.currentTimeMillis() + Long.parseLong(attributeValue) * 1000;
                    } catch (NumberFormatException e) {
                        // Ignored, as browsers do.
                    }
                    break;
                case "expires":
                    try {
                        if (cookie.expiresAt == Long.MAX_VALUE) {
                            cookie.expiresAt = java.util.Date.parse(attributeValue);
                        }
                    } catch (IllegalArgumentException e) {
                        // Ignored.
                    }
                    break;
                default:
                    break;
            }
        }
        for (Iterator<Cookie> i = mCookies.iterator(); i.hasNext();) {
            Cookie old = i.next();
            if (old.name.equals(cookie.name) && old.domain.equals(cookie.domain) && old.path.equals(cookie.path)) {
                i.remove();
            }
        }
        if (cookie.expiresAt > System.currentTimeMillis()) {
            mCookies.add(cookie);
        }
        WebViewSupport.CookieHost cookieHost = WebViewSupport.cookieHost();
        if (cookieHost != null) {
            cookieHost.setCookie((cookie.hostOnly ? "" : ".") + cookie.domain, cookie.path, cookie.name, cookie.value,
                    cookie.secure, cookie.expiresAt == Long.MAX_VALUE ? 0 : cookie.expiresAt);
        }
        return true;
    }

    /** Takes over the host's cookies; see {@link WebViewSupport#onHostCookies} for the format. */
    synchronized void mergeFromHost(String lines) {
        for (String line : lines.split("\n")) {
            String[] fields = line.split("\t", -1);
            if (fields.length < 7 || fields[2].isEmpty()) {
                continue;
            }
            Cookie cookie = new Cookie();
            cookie.domain = fields[0].toLowerCase().replaceFirst("^\\.", "");
            cookie.path = fields[1].isEmpty() ? "/" : fields[1];
            cookie.name = fields[2];
            cookie.value = fields[3];
            cookie.secure = fields[4].equals("1");
            long expires = Long.parseLong(fields[5]);
            cookie.expiresAt = expires == 0 ? Long.MAX_VALUE : expires;
            cookie.hostOnly = fields[6].equals("1");
            mCookies.removeIf(old -> old.name.equals(cookie.name) && old.domain.equals(cookie.domain)
                    && old.path.equals(cookie.path));
            mCookies.add(cookie);
        }
    }

    @Override
    public String getCookie(String url) {
        return getCookie(url, false);
    }

    @Override
    public synchronized String getCookie(String url, boolean privateBrowsing) {
        Uri uri = url != null ? Uri.parse(url.contains("://") ? url : "http://" + url) : null;
        String host = uri != null && uri.getHost() != null ? uri.getHost().toLowerCase() : null;
        if (host == null) {
            return null;
        }
        String path = uri.getPath() == null || uri.getPath().isEmpty() ? "/" : uri.getPath();
        boolean secure = "https".equalsIgnoreCase(uri.getScheme());
        long now = System.currentTimeMillis();
        StringBuilder result = new StringBuilder();
        for (Cookie cookie : mCookies) {
            boolean domainMatches = host.equals(cookie.domain)
                    || (!cookie.hostOnly && host.endsWith("." + cookie.domain));
            boolean pathMatches = path.equals(cookie.path) || path.startsWith(
                    cookie.path.endsWith("/") ? cookie.path : cookie.path + "/");
            if (domainMatches && pathMatches && (!cookie.secure || secure) && cookie.expiresAt > now) {
                if (result.length() > 0) {
                    result.append("; ");
                }
                result.append(cookie.name).append('=').append(cookie.value);
            }
        }
        return result.length() > 0 ? result.toString() : null;
    }

    @Override
    public void removeSessionCookie() {
        removeSessionCookies(null);
    }

    @Override
    public void removeSessionCookies(ValueCallback<Boolean> callback) {
        boolean removed;
        synchronized (this) {
            removed = mCookies.removeIf(cookie -> cookie.expiresAt == Long.MAX_VALUE);
        }
        if (callback != null) {
            callback.onReceiveValue(removed);
        }
    }

    @Override
    public void removeAllCookie() {
        removeAllCookies(null);
    }

    @Override
    public void removeAllCookies(ValueCallback<Boolean> callback) {
        boolean removed;
        synchronized (this) {
            removed = !mCookies.isEmpty();
            mCookies.clear();
        }
        WebViewSupport.CookieHost host = WebViewSupport.cookieHost();
        if (host != null) {
            host.clearCookies();
        }
        if (callback != null) {
            callback.onReceiveValue(removed);
        }
    }

    @Override
    public synchronized boolean hasCookies() {
        return !mCookies.isEmpty();
    }

    @Override
    public boolean hasCookies(boolean privateBrowsing) {
        return hasCookies();
    }

    @Override
    public synchronized void removeExpiredCookie() {
        long now = System.currentTimeMillis();
        mCookies.removeIf(cookie -> cookie.expiresAt <= now);
    }

    @Override
    public void flush() {}

    @Override
    protected synchronized boolean allowFileSchemeCookiesImpl() {
        return mAcceptFileScheme;
    }

    @Override
    protected synchronized void setAcceptFileSchemeCookiesImpl(boolean accept) {
        mAcceptFileScheme = accept;
    }
}
