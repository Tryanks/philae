// android.webkit.WebView on WKWebView.
//
// Android does not draw web pages here: each Android WebView has a WKWebView that is
// placed in front of the Android screen where the widget is (see a2i.webkit.HostWebView,
// which decides where and when). This file is the iOS half: it creates and positions the
// web views, carries out what the Android side asks of them, and reports navigation,
// progress, script results and cookies back.
//
// The natives are called on Android's main thread and do their work on the iOS main
// thread; events travel the other way through IosHost.webViewEvent.

#import <WebKit/WebKit.h>

#include "host.h"

// a2i.webkit.HostWebView.EVENT_* and COMMAND_*.
enum {
    kEventPageStarted = 1,
    kEventPageFinished = 2,
    kEventProgress = 3,
    kEventTitle = 4,
    kEventError = 5,
    kEventScriptResult = 6,
    kEventScriptCall = 7,
    kEventNavigate = 8,
    kEventHistory = 9,
    kEventPageVisible = 10,
};
enum { kCommandGoBack = 1, kCommandGoForward = 2, kCommandReload = 3, kCommandStop = 4 };

// android.webkit.WebViewClient.ERROR_* for the failures that have a counterpart.
static int A2IWebErrorCode(NSError *error) {
    if (![error.domain isEqualToString:NSURLErrorDomain]) return -1;  // ERROR_UNKNOWN
    switch (error.code) {
        case NSURLErrorCannotFindHost:
        case NSURLErrorDNSLookupFailed: return -2;        // ERROR_HOST_LOOKUP
        case NSURLErrorTimedOut: return -8;               // ERROR_TIMEOUT
        case NSURLErrorCannotConnectToHost:
        case NSURLErrorNetworkConnectionLost:
        case NSURLErrorNotConnectedToInternet: return -6;  // ERROR_CONNECT
        case NSURLErrorBadURL:
        case NSURLErrorUnsupportedURL: return -12;        // ERROR_BAD_URL
        case NSURLErrorHTTPTooManyRedirects: return -9;   // ERROR_REDIRECT_LOOP
        case NSURLErrorSecureConnectionFailed:
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasBadDate: return -11;  // ERROR_FAILED_SSL_HANDSHAKE
        default: return -1;
    }
}

static jmethodID gEventMethod;    // IosHost.webViewEvent(IILjava/lang/String;Ljava/lang/String;I)V
static jmethodID gCookiesMethod;  // IosHost.webViewCookies(Ljava/lang/String;)V

static void A2ISendEvent(int identifier, int event, NSString *a, NSString *b, int n) {
    JNIEnv *env = gEventMethod ? A2IEnv() : NULL;
    if (env == NULL) return;
    jstring ja = A2INewString(env, a), jb = A2INewString(env, b);
    (*env)->CallStaticVoidMethod(env, gIosHostClass, gEventMethod, (jint)identifier, (jint)event, ja, jb, (jint)n);
    if (ja) (*env)->DeleteLocalRef(env, ja);
    if (jb) (*env)->DeleteLocalRef(env, jb);
}

// Reports the web views' cookies to Android's CookieManager, then runs `then`.
static void A2ISendCookies(void (^then)(void)) {
    [WKWebsiteDataStore.defaultDataStore.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
        NSMutableString *lines = [NSMutableString string];
        for (NSHTTPCookie *cookie in cookies) {
            long long expires = cookie.expiresDate ? (long long)(cookie.expiresDate.timeIntervalSince1970 * 1000) : 0;
            BOOL hostOnly = ![cookie.domain hasPrefix:@"."];
            [lines appendFormat:@"%@\t%@\t%@\t%@\t%d\t%lld\t%d\n", cookie.domain, cookie.path, cookie.name,
                                cookie.value, cookie.isSecure, expires, hostOnly];
        }
        JNIEnv *env = gCookiesMethod ? A2IEnv() : NULL;
        if (env != NULL) {
            jstring blob = A2INewString(env, lines);
            (*env)->CallStaticVoidMethod(env, gIosHostClass, gCookiesMethod, blob);
            (*env)->DeleteLocalRef(env, blob);
        }
        if (then) then();
    }];
}

#pragma mark - One web view

@interface A2IWebView : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property(nonatomic, readonly) WKWebView *view;
- (instancetype)initWithIdentifier:(int)identifier;
- (void)loadURL:(NSString *)url headers:(NSArray<NSString *> *)headers;
- (void)answerNavigation:(int)token allow:(BOOL)allow;
- (void)invalidate;
@end

@implementation A2IWebView {
    int _identifier;
    NSString *_requestedURL;  // what loadUrl was given, until that navigation is under way
    NSMutableDictionary<NSNumber *, void (^)(WKNavigationActionPolicy)> *_decisions;
    int _nextToken;
}

- (instancetype)initWithIdentifier:(int)identifier {
    self = [super init];
    if (self) {
        _identifier = identifier;
        _decisions = [NSMutableDictionary dictionary];
        WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
        configuration.allowsInlineMediaPlayback = YES;
        [configuration.userContentController addScriptMessageHandler:self name:@"a2i"];
        _view = [[WKWebView alloc] initWithFrame:CGRectZero configuration:configuration];
        _view.navigationDelegate = self;
        _view.UIDelegate = self;
        _view.hidden = YES;
        _view.allowsBackForwardNavigationGestures = NO;  // the left edge is Android's back gesture
        [_view addObserver:self forKeyPath:@"estimatedProgress" options:0 context:NULL];
        [_view addObserver:self forKeyPath:@"title" options:0 context:NULL];
        [A2IAndroidScreen() addSubview:_view];
    }
    return self;
}

- (void)invalidate {
    for (void (^decision)(WKNavigationActionPolicy) in _decisions.allValues) decision(WKNavigationActionPolicyCancel);
    [_decisions removeAllObjects];
    [_view removeObserver:self forKeyPath:@"estimatedProgress"];
    [_view removeObserver:self forKeyPath:@"title"];
    [_view.configuration.userContentController removeScriptMessageHandlerForName:@"a2i"];
    _view.navigationDelegate = nil;
    _view.UIDelegate = nil;
    [_view stopLoading];
    [_view removeFromSuperview];
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    if ([keyPath isEqualToString:@"estimatedProgress"]) {
        A2ISendEvent(_identifier, kEventProgress, nil, nil, (int)lround(_view.estimatedProgress * 100));
    } else if ([keyPath isEqualToString:@"title"] && _view.title.length > 0) {
        A2ISendEvent(_identifier, kEventTitle, _view.title, nil, 0);
    }
}

- (void)sendHistory {
    A2ISendEvent(_identifier, kEventHistory, _view.URL.absoluteString, nil,
                 (_view.canGoBack ? 1 : 0) | (_view.canGoForward ? 2 : 0));
}

- (void)loadURL:(NSString *)url headers:(NSArray<NSString *> *)headers {
    NSURL *target = [NSURL URLWithString:url];
    if (target == nil) {
        A2ISendEvent(_identifier, kEventError, @"malformed URL", url, -12);
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:target];
    for (NSUInteger i = 0; i + 1 < headers.count; i += 2) {
        [request setValue:headers[i + 1] forHTTPHeaderField:headers[i]];
    }
    _requestedURL = target.absoluteString;
    if (target.isFileURL) {
        [_view loadFileURL:target allowingReadAccessToURL:target.URLByDeletingLastPathComponent];
    } else {
        [_view loadRequest:request];
    }
}

- (void)answerNavigation:(int)token allow:(BOOL)allow {
    void (^decision)(WKNavigationActionPolicy) = _decisions[@(token)];
    if (decision == nil) return;
    [_decisions removeObjectForKey:@(token)];
    decision(allow ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

#pragma mark Navigation

// Android asks the app (WebViewClient.shouldOverrideUrlLoading) about navigations in the
// main frame that the app did not start itself: links the user follows and redirects.
- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    NSString *url = action.request.URL.absoluteString;
    BOOL mainFrame = action.targetFrame == nil || action.targetFrame.isMainFrame;
    BOOL byApp = action.navigationType == WKNavigationTypeOther && [url isEqualToString:_requestedURL];
    BOOL historyOrReload = action.navigationType == WKNavigationTypeBackForward
                        || action.navigationType == WKNavigationTypeReload;
    NSString *scheme = action.request.URL.scheme.lowercaseString;
    BOOL internal = [scheme isEqualToString:@"about"] || [scheme isEqualToString:@"data"]
                 || [scheme isEqualToString:@"blob"];
    if (!mainFrame || byApp || historyOrReload || internal || url == nil) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    int token = ++_nextToken;
    _decisions[@(token)] = [decisionHandler copy];
    NSString *kind = action.navigationType == WKNavigationTypeOther ? @"redirect" : @"link";
    A2ISendEvent(_identifier, kEventNavigate, url, kind, token);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    A2ISendEvent(_identifier, kEventPageStarted, webView.URL.absoluteString, nil, 0);
}

- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation {
    _requestedURL = nil;
    [self sendHistory];
    A2ISendEvent(_identifier, kEventPageVisible, webView.URL.absoluteString, nil, 0);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    // Apps look at the cookies as soon as a page has loaded (to see whether a login or a
    // bot check went through), so those are reported first.
    NSString *url = webView.URL.absoluteString;
    A2ISendCookies(^{
        [self sendHistory];
        A2ISendEvent(self->_identifier, kEventPageFinished, url, nil, 0);
    });
}

- (void)failed:(NSError *)error {
    _requestedURL = nil;
    if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) return;
    // (WebKit reports a navigation the app's policy cancelled as error 102.)
    if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102) return;
    NSString *url = error.userInfo[NSURLErrorFailingURLStringErrorKey] ?: _view.URL.absoluteString;
    A2ISendEvent(_identifier, kEventError, error.localizedDescription, url, A2IWebErrorCode(error));
    A2ISendEvent(_identifier, kEventPageFinished, url, nil, 0);
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    [self failed:error];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    [self failed:error];
}

#pragma mark UI delegate

// A link that wants a new window opens in this one: there is only the one.
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)action
                    windowFeatures:(WKWindowFeatures *)features {
    if (action.targetFrame == nil) [webView loadRequest:action.request];
    return nil;
}

// JavaScript dialogs are answered at once, as if dismissed (they are not shown yet).
- (void)webView:(WKWebView *)webView
    runJavaScriptAlertPanelWithMessage:(NSString *)message
                      initiatedByFrame:(WKFrameInfo *)frame
                     completionHandler:(void (^)(void))completionHandler {
    completionHandler();
}

- (void)webView:(WKWebView *)webView
    runJavaScriptConfirmPanelWithMessage:(NSString *)message
                        initiatedByFrame:(WKFrameInfo *)frame
                       completionHandler:(void (^)(BOOL))completionHandler {
    completionHandler(NO);
}

#pragma mark JavaScript interfaces

// A page calling window.<name>.<method>(...) of an object the app exposed
// (WebView.addJavascriptInterface); see A2IAddJavascriptObject for the page's side.
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    NSDictionary *body = [message.body isKindOfClass:NSDictionary.class] ? message.body : nil;
    NSString *object = body[@"object"];
    if (![object isKindOfClass:NSString.class]) return;
    NSDictionary *call = @{@"method": body[@"method"] ?: @"", @"args": body[@"args"] ?: @[]};
    NSData *json = [NSJSONSerialization dataWithJSONObject:call options:0 error:NULL];
    if (json == nil) return;
    A2ISendEvent(_identifier, kEventScriptCall, object,
                 [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding], 0);
}

@end

#pragma mark - Natives

static NSMutableDictionary<NSNumber *, A2IWebView *> *gWebViews;  // main thread only

static void A2IOnMain(void (^block)(void)) {
    dispatch_async(dispatch_get_main_queue(), block);
}

static void WebViewCreate(JNIEnv *env, jclass clazz, jint identifier) {
    (void)env, (void)clazz;
    A2IOnMain(^{
        if (gWebViews == nil) gWebViews = [NSMutableDictionary dictionary];
        gWebViews[@(identifier)] = [[A2IWebView alloc] initWithIdentifier:identifier];
    });
}

static void WebViewDestroy(JNIEnv *env, jclass clazz, jint identifier) {
    (void)env, (void)clazz;
    A2IOnMain(^{
        [gWebViews[@(identifier)] invalidate];
        [gWebViews removeObjectForKey:@(identifier)];
    });
}

static void WebViewSetFrame(JNIEnv *env, jclass clazz, jint identifier, jint x, jint y, jint width, jint height,
                            jboolean visible) {
    (void)env, (void)clazz;
    A2IOnMain(^{
        WKWebView *view = gWebViews[@(identifier)].view;
        CGFloat scale = A2IScale();
        if (visible) view.frame = CGRectMake(x / scale, y / scale, width / scale, height / scale);
        view.hidden = !visible;
    });
}

static void WebViewConfigure(JNIEnv *env, jclass clazz, jint identifier, jstring userAgent, jboolean javaScript) {
    (void)clazz;
    NSString *agent = A2IString(env, userAgent);
    A2IOnMain(^{
        WKWebView *view = gWebViews[@(identifier)].view;
        view.customUserAgent = agent;
        view.configuration.defaultWebpagePreferences.allowsContentJavaScript = javaScript;
    });
}

static void WebViewLoadUrl(JNIEnv *env, jclass clazz, jint identifier, jstring url, jobjectArray headers) {
    (void)clazz;
    NSString *target = A2IString(env, url);
    NSMutableArray<NSString *> *fields = [NSMutableArray array];
    jsize count = headers ? (*env)->GetArrayLength(env, headers) : 0;
    for (jsize i = 0; i < count; i++) {
        jstring field = (*env)->GetObjectArrayElement(env, headers, i);
        [fields addObject:A2IString(env, field) ?: @""];
        (*env)->DeleteLocalRef(env, field);
    }
    A2IOnMain(^{ [gWebViews[@(identifier)] loadURL:target headers:fields]; });
}

static void WebViewLoadData(JNIEnv *env, jclass clazz, jint identifier, jstring data, jstring mimeType,
                            jstring encoding, jstring baseUrl) {
    (void)clazz;
    NSString *content = A2IString(env, data), *mime = A2IString(env, mimeType);
    NSString *charset = A2IString(env, encoding), *base = A2IString(env, baseUrl);
    A2IOnMain(^{
        WKWebView *view = gWebViews[@(identifier)].view;
        NSURL *baseURL = base ? [NSURL URLWithString:base] : nil;
        if ([charset caseInsensitiveCompare:@"base64"] == NSOrderedSame) {
            NSData *decoded = [[NSData alloc] initWithBase64EncodedString:content
                                                                  options:NSDataBase64DecodingIgnoreUnknownCharacters];
            [view loadData:decoded ?: NSData.data MIMEType:mime characterEncodingName:@"utf-8"
                   baseURL:baseURL ?: [NSURL URLWithString:@"about:blank"]];
        } else if ([mime hasPrefix:@"text/html"]) {
            [view loadHTMLString:content baseURL:baseURL];
        } else {
            [view loadData:[content dataUsingEncoding:NSUTF8StringEncoding] MIMEType:mime
                characterEncodingName:@"utf-8" baseURL:baseURL ?: [NSURL URLWithString:@"about:blank"]];
        }
    });
}

// Android hands the script's value back as JSON text ("null" for undefined).
static void WebViewEvaluate(JNIEnv *env, jclass clazz, jint identifier, jstring script, jint callback) {
    (void)clazz;
    NSString *source = A2IString(env, script);
    A2IOnMain(^{
        [gWebViews[@(identifier)].view evaluateJavaScript:source completionHandler:^(id result, NSError *error) {
            if (callback == 0) return;
            NSString *json = @"null";
            if (result != nil && error == nil) {
                NSData *data = [NSJSONSerialization dataWithJSONObject:@[result] options:0 error:NULL];
                NSString *wrapped = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
                if (wrapped.length >= 2) json = [wrapped substringWithRange:NSMakeRange(1, wrapped.length - 2)];
            }
            A2ISendEvent(identifier, kEventScriptResult, json, nil, callback);
        }];
    });
}

static void WebViewCommand(JNIEnv *env, jclass clazz, jint identifier, jint command) {
    (void)env, (void)clazz;
    A2IOnMain(^{
        WKWebView *view = gWebViews[@(identifier)].view;
        switch (command) {
            case kCommandGoBack: [view goBack]; break;
            case kCommandGoForward: [view goForward]; break;
            case kCommandReload: [view reload]; break;
            case kCommandStop: [view stopLoading]; break;
        }
    });
}

// Gives pages a window.<name> whose every method posts the call to this app. (Android
// runs such calls synchronously and returns their value; WebKit offers no synchronous
// channel, so the methods return nothing.)
static void WebViewAddJavascriptObject(JNIEnv *env, jclass clazz, jint identifier, jstring name) {
    (void)clazz;
    NSString *object = A2IString(env, name);
    A2IOnMain(^{
        NSData *quoted = [NSJSONSerialization dataWithJSONObject:@[object] options:0 error:NULL];
        NSString *literal = [[NSString alloc] initWithData:quoted encoding:NSUTF8StringEncoding];
        NSString *source = [NSString stringWithFormat:
            @"(function(n){window[n]=new Proxy({},{get:function(t,m){return function(){"
             "window.webkit.messageHandlers.a2i.postMessage({object:n,method:String(m),"
             "args:Array.prototype.slice.call(arguments)});};}});})(%@[0]);", literal];
        WKUserScript *script = [[WKUserScript alloc] initWithSource:source
                                                      injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                   forMainFrameOnly:NO];
        [gWebViews[@(identifier)].view.configuration.userContentController addUserScript:script];
    });
}

static void WebViewNavigationDecision(JNIEnv *env, jclass clazz, jint identifier, jint token, jboolean allow) {
    (void)env, (void)clazz;
    A2IOnMain(^{ [gWebViews[@(identifier)] answerNavigation:token allow:allow]; });
}

static void WebViewSetCookie(JNIEnv *env, jclass clazz, jstring domain, jstring path, jstring name, jstring value,
                             jboolean secure, jlong expiresAt) {
    (void)clazz;
    NSMutableDictionary *properties = [NSMutableDictionary dictionary];
    properties[NSHTTPCookieDomain] = A2IString(env, domain);
    properties[NSHTTPCookiePath] = A2IString(env, path) ?: @"/";
    properties[NSHTTPCookieName] = A2IString(env, name);
    properties[NSHTTPCookieValue] = A2IString(env, value) ?: @"";
    if (secure) properties[NSHTTPCookieSecure] = @"TRUE";
    if (expiresAt > 0) properties[NSHTTPCookieExpires] = [NSDate dateWithTimeIntervalSince1970:expiresAt / 1000.0];
    A2IOnMain(^{
        NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:properties];
        if (cookie) [WKWebsiteDataStore.defaultDataStore.httpCookieStore setCookie:cookie completionHandler:nil];
    });
}

static void WebViewClearCookies(JNIEnv *env, jclass clazz) {
    (void)env, (void)clazz;
    A2IOnMain(^{
        [WKWebsiteDataStore.defaultDataStore removeDataOfTypes:[NSSet setWithObject:WKWebsiteDataTypeCookies]
                                                 modifiedSince:NSDate.distantPast
                                             completionHandler:^{}];
    });
}

static void WebViewRequestCookies(JNIEnv *env, jclass clazz) {
    (void)env, (void)clazz;
    A2IOnMain(^{ A2ISendCookies(nil); });
}

const JNINativeMethod kA2IWebViewNatives[] = {
    {"webViewCreate", "(I)V", (void *)WebViewCreate},
    {"webViewDestroy", "(I)V", (void *)WebViewDestroy},
    {"webViewSetFrame", "(IIIIIZ)V", (void *)WebViewSetFrame},
    {"webViewConfigure", "(ILjava/lang/String;Z)V", (void *)WebViewConfigure},
    {"webViewLoadUrl", "(ILjava/lang/String;[Ljava/lang/String;)V", (void *)WebViewLoadUrl},
    {"webViewLoadData", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
     (void *)WebViewLoadData},
    {"webViewEvaluate", "(ILjava/lang/String;I)V", (void *)WebViewEvaluate},
    {"webViewCommand", "(II)V", (void *)WebViewCommand},
    {"webViewAddJavascriptObject", "(ILjava/lang/String;)V", (void *)WebViewAddJavascriptObject},
    {"webViewNavigationDecision", "(IIZ)V", (void *)WebViewNavigationDecision},
    {"webViewSetCookie",
     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZJ)V", (void *)WebViewSetCookie},
    {"webViewClearCookies", "()V", (void *)WebViewClearCookies},
    {"webViewRequestCookies", "()V", (void *)WebViewRequestCookies},
};
const int kA2IWebViewNativeCount = sizeof kA2IWebViewNatives / sizeof kA2IWebViewNatives[0];

BOOL A2IWebViewsInit(JNIEnv *env) {
    gEventMethod = (*env)->GetStaticMethodID(env, gIosHostClass, "webViewEvent",
                                             "(IILjava/lang/String;Ljava/lang/String;I)V");
    gCookiesMethod = (*env)->GetStaticMethodID(env, gIosHostClass, "webViewCookies", "(Ljava/lang/String;)V");
    return gEventMethod != NULL && gCookiesMethod != NULL;
}
