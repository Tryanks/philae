package a2i.webkit;

/** android.webkit.WebSettings with the defaults a WebView's settings start out with. */
final class A2IWebSettings extends NeutralWebSettings {
    A2IWebSettings() {
        mValues.put("UserAgentString", WebViewSupport.defaultUserAgent());
        mValues.put("TextZoom", 100);
        mValues.put("DefaultFontSize", 16);
        mValues.put("DefaultFixedFontSize", 13);
        mValues.put("MinimumFontSize", 8);
        mValues.put("MinimumLogicalFontSize", 8);
        mValues.put("DefaultTextEncodingName", "UTF-8");
        mValues.put("StandardFontFamily", "sans-serif");
        mValues.put("FixedFontFamily", "monospace");
        mValues.put("SansSerifFontFamily", "sans-serif");
        mValues.put("SerifFontFamily", "serif");
        mValues.put("CursiveFontFamily", "cursive");
        mValues.put("FantasyFontFamily", "fantasy");
        mValues.put("LoadsImagesAutomatically", true);
        mValues.put("AllowFileAccess", false);
        mValues.put("AllowContentAccess", true);
        mValues.put("SupportZoom", true);
        mValues.put("MediaPlaybackRequiresUserGesture", true);
        mValues.put("CacheMode", android.webkit.WebSettings.LOAD_DEFAULT);
    }
}
