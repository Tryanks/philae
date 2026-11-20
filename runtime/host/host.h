// What the parts of the host app share: the view Android is shown in, and the way into
// the Android side.

#import <UIKit/UIKit.h>

#include "android_vm.h"

// The view that shows Android's screen. Views placed in it use its coordinates: Android's
// pixels divided by A2IScale().
UIView *A2IAndroidScreen(void);
CGFloat A2IScale(void);

// The calling thread's JNIEnv (attaching the thread to the VM if necessary), or NULL
// before the VM exists.
JNIEnv *A2IEnv(void);

// a2i.boot.IosHost, once the VM is up.
extern jclass gIosHostClass;

NSString *A2IString(JNIEnv *env, jstring string);  // nil for null
jstring A2INewString(JNIEnv *env, NSString *string);  // null for nil

// Web views (webview.m): the natives of a2i.boot.IosHost that it implements.
extern const JNINativeMethod kA2IWebViewNatives[];
extern const int kA2IWebViewNativeCount;
BOOL A2IWebViewsInit(JNIEnv *env);
