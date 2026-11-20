package a2i.boot;

import android.app.A2IActivityLauncher;
import android.app.A2IActivityTaskManager;
import android.content.pm.PackageParser;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.view.A2IWindowManager;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Runs one Android app in this process: brings the platform up, launches the APK's
 * launcher activity, and then serves as its main thread until the process ends.
 */
public final class AppRuntime {
    private AppRuntime() {}

    /** Where to find everything, and the screen the app is shown on. */
    public static final class Config {
        public String androidRoot;        // the bundled system directory (Android's /system)
        public String nativeLibraryDir;   // the runtime's native libraries
        public String dataDir;            // writable directory playing the role of /data
        public String apkPath;
        public String appLibraryDir;      // the app's own native libraries, converted; may be null
        public int width;                 // in pixels
        public int height;
        public int densityDpi;
    }

    /** Receives the screen's contents each time they change. Called on Android's main thread. */
    public interface FrameSink {
        void present(Bitmap frame);
    }

    private static Handler sMainHandler;
    private static A2IActivityTaskManager.ExternalStarter sExternalStarter;
    private static InputMethodService.Keyboard sKeyboard;
    private static long sTouchDownTime;

    /**
     * Starts the app on the calling thread, which becomes Android's main thread, and runs
     * its message loop. Returns only if the loop is quit. {@code onStarted}, if given,
     * runs on this thread once the activity is resumed and its window is attached.
     */
    public static void run(Config config, FrameSink sink, Runnable onStarted) throws Exception {
        Platform.init(config.androidRoot, config.nativeLibraryDir);
        // The display's density is a system property on Android, set by the display driver.
        SystemProperties.set("ro.sf.lcd_density", Integer.toString(config.densityDpi));
        Looper.prepareMainLooper();
        Services.install();
        SystemServices.registerAll(
                new SystemServices.DisplayConfig(config.width, config.height, config.densityDpi));
        PackageParser.Package pkg = A2IActivityLauncher.parsePackage(config.apkPath, config.dataDir,
                config.appLibraryDir);
        SystemServices.registerPackage(pkg);

        A2IWindowManager.initialize(config.width, config.height, sink::present);
        sMainHandler = new Handler(Looper.getMainLooper());

        a2i.webkit.WebViewSupport.install();
        A2IActivityTaskManager.getInstance().setExternalStarter(new SystemActivities(sExternalStarter));
        SystemServices.inputMethod().setKeyboard(sKeyboard);
        A2IActivityLauncher.LoadedApp app = A2IActivityLauncher.loadApplication(pkg);
        A2IActivityTaskManager.getInstance().startLauncher(app, config.width, config.height);
        if (onStarted != null) {
            onStarted.run();
        }
        Looper.loop();
    }

    /**
     * Delivers a touch to the app's window. May be called from any thread; the event is
     * dispatched on Android's main thread. Coordinates are in window pixels and
     * {@code action} is a MotionEvent.ACTION_* constant.
     */
    public static void injectTouch(int action, float x, float y) {
        Handler handler = sMainHandler;
        if (handler == null) {
            return;  // not started yet
        }
        final long now = SystemClock.uptimeMillis();
        handler.post(() -> {
            if (action == MotionEvent.ACTION_DOWN) {
                sTouchDownTime = now;
            }
            MotionEvent event = MotionEvent.obtain(sTouchDownTime, now, action, x, y, 0);
            event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
            A2IWindowManager.getInstance().dispatchTouchEvent(event);
            event.recycle();
        });
    }

    /**
     * Presses and releases a key, such as KeyEvent.KEYCODE_BACK, in the focused window.
     * May be called from any thread.
     */
    public static void injectKey(int keyCode) {
        Handler handler = sMainHandler;
        if (handler == null) {
            return;
        }
        handler.post(() -> {
            long now = SystemClock.uptimeMillis();
            for (int action : new int[] {KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP}) {
                KeyEvent event = new KeyEvent(now, now, action, keyCode, 0 /* repeat */, 0 /* metaState */,
                        KeyCharacterMap.VIRTUAL_KEYBOARD, 0 /* scancode */,
                        KeyEvent.FLAG_FROM_SYSTEM | KeyEvent.FLAG_VIRTUAL_HARD_KEY, InputDevice.SOURCE_KEYBOARD);
                A2IWindowManager.getInstance().dispatchKeyEvent(event);
            }
        });
    }

    /**
     * Tells the app that it left the screen or returned to it. May be called from any
     * thread, and returns once the app has finished reacting (stopping its activities and
     * saving its state, or resuming), or after {@code timeoutMs}.
     */
    public static void setForeground(boolean foreground, long timeoutMs) {
        Handler handler = sMainHandler;
        if (handler == null) {
            return;
        }
        CountDownLatch done = new CountDownLatch(1);
        handler.post(() -> {
            try {
                A2IActivityTaskManager.getInstance().setForeground(foreground);
            } finally {
                done.countDown();
            }
        });
        try {
            done.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** Sets what handles requests to start activities outside the app. Call before {@link #run}. */
    public static void setExternalStarter(A2IActivityTaskManager.ExternalStarter starter) {
        sExternalStarter = starter;  // applied in run(): framework classes are not usable before
    }

    /** Connects the host's soft keyboard. Call before {@link #run}. */
    public static void setKeyboard(InputMethodService.Keyboard keyboard) {
        sKeyboard = keyboard;  // applied in run(), like the external starter
    }

    /**
     * Runs something that was typed on the host's keyboard against the input method
     * service, on Android's main thread. May be called from any thread.
     */
    public static void withInputMethod(java.util.function.Consumer<InputMethodService> action) {
        Handler handler = sMainHandler;
        if (handler != null) {
            handler.post(() -> action.accept(SystemServices.inputMethod()));
        }
    }

    /** Android's main-thread handler, once the app has started. */
    public static Handler mainHandler() {
        return sMainHandler;
    }
}
