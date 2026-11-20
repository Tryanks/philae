// The a2i runtime host.
//
// This is the process every converted app actually launches. It starts the Android
// runtime bundled next to it, hands it the embedded APK, shows the frames the Android
// side draws, sends it the touches the iOS view receives, and bridges the few things an
// Android app asks of the system that iOS has a direct counterpart for (the back gesture,
// going to the background, opening a link, the share sheet, the keyboard).
//
// Two threads matter. The iOS main thread owns UIKit. A second thread, started here, is
// Android's main thread: it creates the VM, launches the app's activity and then runs
// Android's message loop for the life of the process (a2i.boot.IosHost.run).

#import <UIKit/UIKit.h>
#include <pthread.h>

#include "host.h"

// MotionEvent.ACTION_* values.
enum { kActionDown = 0, kActionUp = 1, kActionMove = 2, kActionCancel = 3 };

static NSString *A2IBundlePath(NSString *relative) {
    return [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:relative];
}

#pragma mark - The view Android draws into

@interface A2IAndroidView : UIView
- (void)playScriptedTaps;
@end

static A2IAndroidView *gAndroidView;   // set once, on the main thread
static UILabel *gStatusLabel;          // shown until the first frame, or on failure
static JavaVM *gVM;
jclass gIosHostClass;                  // a2i.boot.IosHost
static jmethodID gTouchMethod;         // IosHost.touch(IFF)V
static jmethodID gBackMethod;          // IosHost.back()V
static jmethodID gForegroundMethod;    // IosHost.setForeground(Z)V
static jmethodID gKeyboardTextMethod;  // IosHost.keyboardText(Ljava/lang/String;Z)V
static jmethodID gKeyboardDeleteMethod;  // IosHost.keyboardDelete()V
static jmethodID gKeyboardReturnMethod;  // IosHost.keyboardReturn()V

// The calling thread's JNIEnv, attaching the thread to the VM if necessary.
JNIEnv *A2IEnv(void) {
    JNIEnv *env = NULL;
    if (gVM == NULL) return NULL;
    if ((*gVM)->GetEnv(gVM, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    return (*gVM)->AttachCurrentThread(gVM, &env, NULL) == JNI_OK ? env : NULL;
}

#pragma mark - The keyboard

// Android's soft keyboard is the iOS keyboard. A text field that is never seen is the
// keyboard's client, because UIKit's own field supports everything the keyboard can do
// (composition for Chinese and Japanese input, dictation) for free. It only ever holds
// the text being composed: finished text is passed to Android and removed again, so the
// Android field that has focus is the one and only copy of the text.
@interface A2IKeyboardField : UITextField <UITextFieldDelegate>
- (void)showForInputType:(int)inputType imeOptions:(int)imeOptions;
@end

@implementation A2IKeyboardField {
    BOOL _composing;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.delegate = self;
        self.alpha = 0;
        self.autocorrectionType = UITextAutocorrectionTypeNo;
        self.spellCheckingType = UITextSpellCheckingTypeNo;
        self.smartQuotesType = UITextSmartQuotesTypeNo;
        self.smartDashesType = UITextSmartDashesTypeNo;
        self.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
        [self addTarget:self action:@selector(textChanged) forControlEvents:UIControlEventEditingChanged];
    }
    return self;
}

static void A2ISendKeyboardText(NSString *text, BOOL composing) {
    JNIEnv *env = gKeyboardTextMethod ? A2IEnv() : NULL;
    if (env == NULL) return;
    jstring string = A2INewString(env, text);
    (*env)->CallStaticVoidMethod(env, gIosHostClass, gKeyboardTextMethod, string, (jboolean)composing);
    (*env)->DeleteLocalRef(env, string);
}

- (void)textChanged {
    NSString *text = self.text ?: @"";
    if (self.markedTextRange != nil) {
        _composing = YES;
        A2ISendKeyboardText(text, YES);
    } else if (text.length > 0) {
        _composing = NO;
        A2ISendKeyboardText(text, NO);
        self.text = @"";
    } else if (_composing) {
        _composing = NO;
        A2ISendKeyboardText(@"", YES);  // the composition was deleted
    }
}

// The delete key with nothing being composed deletes in the Android field.
- (void)deleteBackward {
    if (self.text.length > 0) {
        [super deleteBackward];
        return;
    }
    JNIEnv *env = gKeyboardDeleteMethod ? A2IEnv() : NULL;
    if (env != NULL) (*env)->CallStaticVoidMethod(env, gIosHostClass, gKeyboardDeleteMethod);
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    JNIEnv *env = gKeyboardReturnMethod ? A2IEnv() : NULL;
    if (env != NULL) (*env)->CallStaticVoidMethod(env, gIosHostClass, gKeyboardReturnMethod);
    return NO;
}

// Configures the keyboard from Android's description of the field (EditorInfo.inputType
// and imeOptions) and brings it up.
- (void)showForInputType:(int)inputType imeOptions:(int)imeOptions {
    int class = inputType & 0xf, variation = inputType & 0xff0, flags = inputType & 0xfff000;
    UIKeyboardType keyboard = UIKeyboardTypeDefault;
    BOOL secure = NO;
    if (class == 2) {  // TYPE_CLASS_NUMBER; flags: signed 0x1000, decimal 0x2000
        keyboard = (flags & 0x1000) ? UIKeyboardTypeNumbersAndPunctuation
                 : (flags & 0x2000) ? UIKeyboardTypeDecimalPad : UIKeyboardTypeNumberPad;
        secure = variation == 0x10;  // TYPE_NUMBER_VARIATION_PASSWORD
    } else if (class == 3) {  // TYPE_CLASS_PHONE
        keyboard = UIKeyboardTypePhonePad;
    } else if (class == 4) {  // TYPE_CLASS_DATETIME
        keyboard = UIKeyboardTypeNumbersAndPunctuation;
    } else if (class == 1) {  // TYPE_CLASS_TEXT
        if (variation == 0x10) keyboard = UIKeyboardTypeURL;                               // URI
        if (variation == 0x20 || variation == 0xd0) keyboard = UIKeyboardTypeEmailAddress;  // (web) e-mail
        secure = variation == 0x80 || variation == 0xe0;                                   // (web) password
        if (variation == 0x90) keyboard = UIKeyboardTypeASCIICapable;                      // visible password
    }
    UITextAutocapitalizationType caps = UITextAutocapitalizationTypeNone;
    if (class == 1) {
        if (flags & 0x1000) caps = UITextAutocapitalizationTypeAllCharacters;
        else if (flags & 0x2000) caps = UITextAutocapitalizationTypeWords;
        else if (flags & 0x4000) caps = UITextAutocapitalizationTypeSentences;
    }
    UIReturnKeyType returnKey = UIReturnKeyDefault;
    switch (imeOptions & 0xff) {  // EditorInfo.IME_MASK_ACTION
        case 2: returnKey = UIReturnKeyGo; break;
        case 3: returnKey = UIReturnKeySearch; break;
        case 4: returnKey = UIReturnKeySend; break;
        case 5: returnKey = UIReturnKeyNext; break;
        case 6: returnKey = UIReturnKeyDone; break;
    }
    if (imeOptions & 0x40000000) returnKey = UIReturnKeyDefault;  // IME_FLAG_NO_ENTER_ACTION
    BOOL changed = self.keyboardType != keyboard || self.secureTextEntry != secure
                || self.autocapitalizationType != caps || self.returnKeyType != returnKey;
    self.keyboardType = keyboard;
    self.secureTextEntry = secure;
    self.autocapitalizationType = caps;
    self.returnKeyType = returnKey;
    if (!self.isFirstResponder) {
        self.text = @"";
        _composing = NO;
        [self becomeFirstResponder];
    } else if (changed) {
        [self reloadInputViews];
    }
}

@end

static A2IKeyboardField *gKeyboardField;  // main thread only
static CGFloat gFieldBottom;              // of the Android field being edited, in view points
static CGFloat gKeyboardTop = CGFLOAT_MAX;  // in the Android view's untransformed coordinates

// Moves the Android screen up just enough that the field being edited is not under the
// keyboard (what Android calls panning the window).
static void A2IUpdateKeyboardPan(void) {
    CGFloat overlap = gKeyboardField.isFirstResponder ? gFieldBottom + 12 - gKeyboardTop : 0;
    CGAffineTransform transform = overlap > 0 ? CGAffineTransformMakeTranslation(0, -overlap)
                                              : CGAffineTransformIdentity;
    [UIView animateWithDuration:0.25 animations:^{ gAndroidView.transform = transform; }];
}

@implementation A2IAndroidView

- (void)backSwiped:(UIGestureRecognizer *)recognizer {
    if (recognizer.state == UIGestureRecognizerStateEnded) [self sendBack];
}

// Test automation: types text as the keyboard would.
- (void)typeScriptedText:(NSString *)text {
    if (!gKeyboardField.isFirstResponder) return;
    if ([text isEqualToString:@"<delete>"]) {
        [gKeyboardField deleteBackward];
    } else if ([text isEqualToString:@"<return>"]) {
        [gKeyboardField textFieldShouldReturn:gKeyboardField];
    } else {
        gKeyboardField.text = text;
        [gKeyboardField textChanged];
    }
}

- (void)sendBack {
    JNIEnv *env = gBackMethod ? A2IEnv() : NULL;
    if (env != NULL) (*env)->CallStaticVoidMethod(env, gIosHostClass, gBackMethod);
}

// Sends one touch to Android. `point` is in this view's coordinates; Android works in
// pixels, UIKit in points.
- (void)sendTouchAt:(CGPoint)point action:(int)action {
    JNIEnv *env = gTouchMethod ? A2IEnv() : NULL;
    if (env == NULL) return;
    CGFloat scale = self.contentScaleFactor;
    (*env)->CallStaticVoidMethod(env, gIosHostClass, gTouchMethod, (jint)action,
                                 (jfloat)(point.x * scale), (jfloat)(point.y * scale));
}

- (void)sendTouches:(NSSet<UITouch *> *)touches action:(int)action {
    [self sendTouchAt:[touches.anyObject locationInView:self] action:action];
}

// Test automation: A2I_TAPS="x,y;x,y;back;wait;type:text;..." plays those steps, half a
// second apart, once the app is on screen: a tap at a point in this view, the back
// gesture, nothing, text entered on the keyboard (also type:<delete>, type:<return>), or a
// drag (swipe:x1,y1,x2,y2). They take the same path as real input from here on; only UIKit's delivery to
// this view is not exercised.
- (void)playScriptedTaps {
    NSString *script = NSProcessInfo.processInfo.environment[@"A2I_TAPS"];
    NSTimeInterval delay = 1.0;
    for (NSString *tap in [script componentsSeparatedByString:@";"]) {
        if ([tap isEqualToString:@"back"]) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [self sendBack];
            });
        }
        if ([tap hasPrefix:@"swipe:"]) {
            NSArray<NSString *> *p = [[tap substringFromIndex:6] componentsSeparatedByString:@","];
            if (p.count == 4) {
                CGPoint from = CGPointMake(p[0].doubleValue, p[1].doubleValue);
                CGPoint to = CGPointMake(p[2].doubleValue, p[3].doubleValue);
                const int steps = 12;
                for (int i = 0; i <= steps + 1; i++) {
                    CGFloat f = MIN(i, steps) / (CGFloat)steps;
                    CGPoint point = CGPointMake(from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f);
                    int action = i == 0 ? kActionDown : i > steps ? kActionUp : kActionMove;
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((delay + 0.025 * i) * NSEC_PER_SEC)),
                                   dispatch_get_main_queue(), ^{ [self sendTouchAt:point action:action]; });
                }
            }
            delay += 0.5;
            continue;
        }
        if ([tap hasPrefix:@"type:"]) {
            NSString *text = [tap substringFromIndex:5];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                [self typeScriptedText:text];
            });
        }
        if ([tap isEqualToString:@"back"] || [tap isEqualToString:@"wait"] || [tap hasPrefix:@"type:"]) {
            delay += 0.5;
            continue;
        }
        NSArray<NSString *> *xy = [tap componentsSeparatedByString:@","];
        if (xy.count != 2) continue;
        CGPoint point = CGPointMake(xy[0].doubleValue, xy[1].doubleValue);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self sendTouchAt:point action:kActionDown];
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((delay + 0.08) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self sendTouchAt:point action:kActionUp];
        });
        delay += 0.5;
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self sendTouches:touches action:kActionDown];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self sendTouches:touches action:kActionMove];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self sendTouches:touches action:kActionUp];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self sendTouches:touches action:kActionCancel];
}

@end

#pragma mark - Android's main thread

static void A2IShowStatus(NSString *text) {
    printf("%s\n", text.UTF8String);
    fflush(stdout);
    dispatch_async(dispatch_get_main_queue(), ^{
        gStatusLabel.text = text;
        gStatusLabel.hidden = NO;
    });
}

static void A2IReleasePixels(void *info, const void *data, size_t size) {
    (void)info, (void)size;
    free((void *)data);
}

// a2i.boot.IosHost.presentFrame([III)V — called on Android's main thread for each frame.
// The pixels are ARGB ints, i.e. BGRA bytes; a window's frame is opaque, so the alpha
// byte is skipped rather than interpreted.
static void A2IPresentFrame(JNIEnv *env, jclass clazz, jintArray pixels, jint width, jint height) {
    (void)clazz;
    size_t size = (size_t)width * (size_t)height * 4;
    void *copy = malloc(size);
    (*env)->GetIntArrayRegion(env, pixels, 0, width * height, copy);

    static CGColorSpaceRef colorSpace;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB); });
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, copy, size, A2IReleasePixels);
    CGImageRef image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, colorSpace,
                                     kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
                                     NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    dispatch_async(dispatch_get_main_queue(), ^{
        gAndroidView.layer.contents = (__bridge id)image;
        gStatusLabel.hidden = YES;
        CGImageRelease(image);
        static dispatch_once_t firstFrame;
        dispatch_once(&firstFrame, ^{ [gAndroidView playScriptedTaps]; });
    });
}

// (Through UTF-16 both ways: JNI's "UTF" functions use a modified UTF-8 that garbles
// characters outside the basic plane.)
NSString *A2IString(JNIEnv *env, jstring string) {
    if (string == NULL) return nil;
    jsize length = (*env)->GetStringLength(env, string);
    const jchar *chars = (*env)->GetStringChars(env, string, NULL);
    NSString *result = [NSString stringWithCharacters:chars length:(NSUInteger)length];
    (*env)->ReleaseStringChars(env, string, chars);
    return result;
}

jstring A2INewString(JNIEnv *env, NSString *string) {
    if (string == nil) return NULL;
    NSUInteger length = string.length;
    unichar *chars = malloc(sizeof(unichar) * (length + 1));
    [string getCharacters:chars range:NSMakeRange(0, length)];
    jstring result = (*env)->NewString(env, chars, (jsize)length);
    free(chars);
    return result;
}

UIView *A2IAndroidScreen(void) {
    return gAndroidView;
}

CGFloat A2IScale(void) {
    return gAndroidView.contentScaleFactor;
}

// a2i.boot.IosHost.openUrl(String)V — an Android intent to view a link or address.
static void A2IOpenUrl(JNIEnv *env, jclass clazz, jstring url) {
    (void)clazz;
    NSURL *target = [NSURL URLWithString:A2IString(env, url)];
    if (target == nil) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [UIApplication.sharedApplication openURL:target options:@{} completionHandler:nil];
    });
}

// a2i.boot.IosHost.share(String,String,String)V — an Android intent to send text and/or
// a file somewhere.
static void A2IShare(JNIEnv *env, jclass clazz, jstring text, jstring filePath, jstring subject) {
    (void)clazz;
    NSString *content = A2IString(env, text);
    NSString *path = A2IString(env, filePath);
    NSString *title = A2IString(env, subject);
    NSMutableArray *items = [NSMutableArray array];
    if (path != nil) [items addObject:[NSURL fileURLWithPath:path]];
    if (content != nil) [items addObject:content];
    if (items.count == 0) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIActivityViewController *sheet =
            [[UIActivityViewController alloc] initWithActivityItems:items applicationActivities:nil];
        if (title != nil) [sheet setValue:title forKey:@"subject"];
        // On an iPad the sheet is a popover and needs an anchor.
        sheet.popoverPresentationController.sourceView = gAndroidView;
        sheet.popoverPresentationController.sourceRect =
            CGRectMake(CGRectGetMidX(gAndroidView.bounds), CGRectGetMidY(gAndroidView.bounds), 1, 1);
        [gAndroidView.window.rootViewController presentViewController:sheet animated:YES completion:nil];
    });
}

// a2i.boot.IosHost.showKeyboard(III)V and hideKeyboard()V.
static void A2IShowKeyboard(JNIEnv *env, jclass clazz, jint inputType, jint imeOptions, jint fieldBottom) {
    (void)env, (void)clazz;
    dispatch_async(dispatch_get_main_queue(), ^{
        gFieldBottom = fieldBottom / gAndroidView.contentScaleFactor;
        [gKeyboardField showForInputType:inputType imeOptions:imeOptions];
        A2IUpdateKeyboardPan();
    });
}

static void A2IHideKeyboard(JNIEnv *env, jclass clazz) {
    (void)env, (void)clazz;
    dispatch_async(dispatch_get_main_queue(), ^{
        [gKeyboardField resignFirstResponder];
        A2IUpdateKeyboardPan();
    });
}

// Describes and clears the pending Java exception.
static NSString *A2ITakeException(JNIEnv *env) {
    jthrowable exception = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionDescribe(env);  // full stack trace to the log
    (*env)->ExceptionClear(env);
    jclass object = (*env)->FindClass(env, "java/lang/Object");
    jmethodID toString = (*env)->GetMethodID(env, object, "toString", "()Ljava/lang/String;");
    jstring text = (*env)->CallObjectMethod(env, exception, toString);
    const char *chars = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    NSString *description = chars ? @(chars) : @"unknown exception";
    if (chars) (*env)->ReleaseStringUTFChars(env, text, chars);
    (*env)->ExceptionClear(env);
    return description;
}

typedef struct {
    int width, height, densityDpi;
} A2IDisplay;

static void *A2IAndroidMain(void *context) {
    A2IDisplay display = *(A2IDisplay *)context;
    free(context);
    pthread_setname_np("android-main");
    @autoreleasepool {
        NSString *dataDir = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)
                                .firstObject stringByAppendingPathComponent:@"android-data"];
        [NSFileManager.defaultManager createDirectoryAtPath:dataDir
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:NULL];
        NSString *androidRoot = A2IBundlePath(@"android-root");
        NSString *libraryDir = A2IBundlePath(@"Frameworks");
        NSString *apk = A2IBundlePath(@"android/base.apk");

        a2i_vm_config config = {0};
        config.android_root = androidRoot.fileSystemRepresentation;
        config.data_dir = dataDir.fileSystemRepresentation;
        JNIEnv *env = NULL;
        char error[512] = "";
        if (a2i_vm_create(&config, &gVM, &env, error, sizeof error) != JNI_OK) {
            A2IShowStatus([NSString stringWithFormat:@"The Android runtime failed to start:\n%s", error]);
            return NULL;
        }

        jclass host = (*env)->FindClass(env, "a2i/boot/IosHost");
        JNINativeMethod natives[] = {
            {"presentFrame", "([III)V", (void *)A2IPresentFrame},
            {"openUrl", "(Ljava/lang/String;)V", (void *)A2IOpenUrl},
            {"showKeyboard", "(III)V", (void *)A2IShowKeyboard},
            {"hideKeyboard", "()V", (void *)A2IHideKeyboard},
            {"share", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void *)A2IShare},
        };
        jmethodID run = NULL;
        if (host != NULL
            && (*env)->RegisterNatives(env, host, natives, sizeof natives / sizeof natives[0]) == JNI_OK
            && (*env)->RegisterNatives(env, host, kA2IWebViewNatives, kA2IWebViewNativeCount) == JNI_OK) {
            run = (*env)->GetStaticMethodID(
                env, host, "run",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;III)V");
            gIosHostClass = (*env)->NewGlobalRef(env, host);
            gTouchMethod = (*env)->GetStaticMethodID(env, host, "touch", "(IFF)V");
            gBackMethod = (*env)->GetStaticMethodID(env, host, "back", "()V");
            gForegroundMethod = (*env)->GetStaticMethodID(env, host, "setForeground", "(Z)V");
            gKeyboardTextMethod = (*env)->GetStaticMethodID(env, host, "keyboardText", "(Ljava/lang/String;Z)V");
            gKeyboardDeleteMethod = (*env)->GetStaticMethodID(env, host, "keyboardDelete", "()V");
            gKeyboardReturnMethod = (*env)->GetStaticMethodID(env, host, "keyboardReturn", "()V");
            if (!A2IWebViewsInit(env)) run = NULL;
        }
        if (run == NULL || gTouchMethod == NULL || gBackMethod == NULL || gForegroundMethod == NULL
            || gKeyboardTextMethod == NULL || gKeyboardDeleteMethod == NULL || gKeyboardReturnMethod == NULL) {
            A2IShowStatus([@"The Android runtime is incomplete:\n" stringByAppendingString:A2ITakeException(env)]);
            return NULL;
        }

        // Runs Android's message loop; returns only if the app fails to start.
        (*env)->CallStaticVoidMethod(env, host, run,
                                     (*env)->NewStringUTF(env, androidRoot.UTF8String),
                                     (*env)->NewStringUTF(env, libraryDir.UTF8String),
                                     (*env)->NewStringUTF(env, dataDir.UTF8String),
                                     (*env)->NewStringUTF(env, apk.UTF8String),
                                     (jint)display.width, (jint)display.height, (jint)display.densityDpi);
        if ((*env)->ExceptionCheck(env)) {
            A2IShowStatus([@"The Android app failed to start:\n" stringByAppendingString:A2ITakeException(env)]);
        }
    }
    // ART aborts if an attached thread exits, so stay parked.
    for (;;) pause();
}

static void A2IStartAndroid(A2IDisplay display) {
    A2IDisplay *context = malloc(sizeof *context);
    *context = display;
    // ART interprets on the native stack, and secondary threads only get 512 KiB by default.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    pthread_create(&thread, &attributes, A2IAndroidMain, context);
    pthread_attr_destroy(&attributes);
}

#pragma mark - UIKit plumbing

@interface A2IViewController : UIViewController
@end

@implementation A2IViewController {
    BOOL _started;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    // Android gets the safe area as its whole screen, so it never draws under the status
    // bar or the home indicator.
    gAndroidView = [[A2IAndroidView alloc] initWithFrame:CGRectZero];
    gAndroidView.translatesAutoresizingMaskIntoConstraints = NO;
    gAndroidView.layer.contentsGravity = kCAGravityResize;
    [self.view addSubview:gAndroidView];

    // Android's back key. iOS has no such key; its equivalent gesture is a swipe in from
    // the left edge of the screen. (On this view, so that it also works over web views.)
    UIScreenEdgePanGestureRecognizer *back =
        [[UIScreenEdgePanGestureRecognizer alloc] initWithTarget:gAndroidView action:@selector(backSwiped:)];
    back.edges = UIRectEdgeLeft;
    [self.view addGestureRecognizer:back];

    gKeyboardField = [[A2IKeyboardField alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
    [self.view addSubview:gKeyboardField];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(keyboardFrameChanged:)
                                               name:UIKeyboardWillChangeFrameNotification
                                             object:nil];

    gStatusLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    gStatusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    gStatusLabel.numberOfLines = 0;
    gStatusLabel.textColor = UIColor.whiteColor;
    gStatusLabel.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    gStatusLabel.text = @"Starting the Android runtime…";
    [self.view addSubview:gStatusLabel];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [gAndroidView.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [gAndroidView.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [gAndroidView.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [gAndroidView.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [gStatusLabel.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [gStatusLabel.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [gStatusLabel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
    ]];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGSize size = gAndroidView.bounds.size;
    if (_started || size.width <= 0 || size.height <= 0) return;
    _started = YES;
    // The Android screen is the view in pixels, at Android's density for this scale
    // (160 dpi per point: @2x is xhdpi, @3x is xxhdpi).
    // The screen's scale, not the view's: a plain view's contentScaleFactor stays 1 until
    // something draws into it, which would have Android render at a third of the
    // resolution. The view is given the same scale so that touches convert consistently.
    CGFloat scale = self.view.window.screen.scale;
    printf("display: view scale %.1f, screen scale %.1f\n", gAndroidView.contentScaleFactor, scale);
    gAndroidView.contentScaleFactor = scale;
    A2IDisplay display = {(int)lround(size.width * scale), (int)lround(size.height * scale), (int)lround(160 * scale)};
    printf("display: %d x %d px at %d dpi\n", display.width, display.height, display.densityDpi);
    fflush(stdout);
    A2IStartAndroid(display);
}

- (void)keyboardFrameChanged:(NSNotification *)notification {
    CGRect frame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect inView = [self.view convertRect:frame fromView:nil];
    // The Android view's own frame is unaffected by its transform's translation only in
    // `center`/`bounds` terms, so its top is taken from those.
    CGFloat androidTop = gAndroidView.center.y - gAndroidView.bounds.size.height / 2;
    gKeyboardTop = CGRectGetMinY(inView) >= CGRectGetMaxY(self.view.bounds) ? CGFLOAT_MAX
                                                                           : CGRectGetMinY(inView) - androidTop;
    A2IUpdateKeyboardPan();
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UIStatusBarStyleLightContent;
}

@end

// Named in the Info.plist scene manifest that the packer generates.
@interface A2ISceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation A2ISceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [A2IViewController new];
    [self.window makeKeyAndVisible];
}

// Leaving the screen is when Android apps save their state (onPause/onStop), and iOS may
// suspend the process soon after this returns, so the call waits for the app to finish.
- (void)sceneDidEnterBackground:(UIScene *)scene {
    [self setAndroidForeground:NO];
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
    [self setAndroidForeground:YES];
}

- (void)setAndroidForeground:(BOOL)foreground {
    JNIEnv *env = gForegroundMethod ? A2IEnv() : NULL;
    if (env != NULL) (*env)->CallStaticVoidMethod(env, gIosHostClass, gForegroundMethod, (jboolean)foreground);
}
@end

@interface A2IAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation A2IAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(A2IAppDelegate.class));
    }
}
