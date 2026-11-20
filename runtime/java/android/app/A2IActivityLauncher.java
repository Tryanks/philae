package android.app;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageParser;
import android.content.pm.ProviderInfo;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.hardware.display.DisplayManagerGlobal;
import android.os.LocaleList;
import android.os.Process;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.Surface;
import android.view.ThreadedRenderer;

import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * Sets up an app's process state without a system server: what installing the package
 * and binding the application do on a device.
 *
 * On a device these steps are the package manager's and ActivityThread.handleBindApplication's,
 * the latter driven by the activity manager service. This class performs the same sequence
 * directly. Activities are then started by {@link A2IActivityTaskManager}. It lives in
 * android.app because most of what it calls is package-private there.
 */
public final class A2IActivityLauncher {
    private A2IActivityLauncher() {}

    /** The APK parsed and loaded as the current process's application. */
    public static final class LoadedApp {
        public PackageParser.Package pkg;
        public ActivityThread thread;
        public LoadedApk loadedApk;
        public Application application;
    }

    /**
     * Parses the APK's manifest and completes its ApplicationInfo with what the package
     * manager fills in at install time on a device: where its code and data are.
     */
    public static PackageParser.Package parsePackage(String apkPath, String dataDir, String nativeLibraryDir)
            throws Exception {
        PackageParser.Package pkg = new PackageParser().parsePackage(new File(apkPath), 0);
        ApplicationInfo info = pkg.applicationInfo;
        File appData = new File(dataDir, "data/" + info.packageName);
        appData.mkdirs();
        info.sourceDir = info.publicSourceDir = apkPath;
        info.dataDir = info.credentialProtectedDataDir = info.deviceProtectedDataDir = appData.getPath();
        // The app's native libraries: converted to this platform's format when the app was
        // packaged, and installed with it rather than extracted from the APK.
        info.nativeLibraryDir = nativeLibraryDir != null ? nativeLibraryDir : new File(appData, "lib").getPath();
        info.uid = Process.myUid();
        info.processName = info.packageName;
        info.flags |= ApplicationInfo.FLAG_INSTALLED;
        for (PackageParser.Activity activity : pkg.activities) {
            activity.info.applicationInfo = info;
        }
        // Who signed the APK. Apps look at their own signature (to derive keys, or to
        // tell an official build from a repackaged one). The certificates are taken as
        // found: checking the APK against them is the installer's job, and the APK here
        // came packaged inside this app.
        try {
            PackageParser.collectCertificates(pkg, true /* skipVerify */);
        } catch (PackageParser.PackageParserException | RuntimeException e) {
            android.util.Log.w("a2i.Launcher", "cannot read the APK's signing certificates", e);
        }
        return pkg;
    }

    /**
     * Makes the parsed package this process's application: creates its class loader and
     * resources, instantiates its Application and calls onCreate().
     */
    public static LoadedApp loadApplication(PackageParser.Package pkg) throws Exception {
        LoadedApp app = new LoadedApp();
        app.pkg = pkg;
        ApplicationInfo info = pkg.applicationInfo;

        // The process-wide setup of ActivityThread.main(): how the framework's separately
        // updatable modules find their services, and the main thread's handler, which
        // code on other threads posts to (shared preference listeners, for one).
        ActivityThread.initializeMainlineModules();
        app.thread = new ActivityThread();
        setStatic(ActivityThread.class, "sCurrentActivityThread", app.thread);
        ActivityThread.sMainThreadHandler = app.thread.getHandler();
        Instrumentation instrumentation = new Instrumentation();
        instrumentation.basicInit(app.thread);
        app.thread.mInstrumentation = instrumentation;

        // What handleBindApplication does with the configuration the system sends it.
        Configuration config = deviceConfiguration();
        ResourcesManager.getInstance().applyConfigurationToResources(config,
                CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO);
        ConfigurationController configurationController = new ConfigurationController(app.thread);
        configurationController.setConfiguration(config);
        configurationController.setCompatConfiguration(config);
        setField(app.thread, "mConfigurationController", configurationController);
        // Windows are drawn in software (see A2IViewRoot); there is no render thread to warm up.
        ThreadedRenderer.sRendererEnabled = false;

        app.loadedApk = app.thread.getPackageInfoNoCheck(info, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO);
        ActivityThread.AppBindData bindData = new ActivityThread.AppBindData();
        bindData.appInfo = info;
        bindData.info = app.loadedApk;
        bindData.processName = info.processName;
        app.thread.mBoundApplication = bindData;
        app.thread.mProfiler = new ActivityThread.Profiler();

        // The app may start services and send broadcasts from here on (Application.onCreate
        // and content providers do).
        A2IComponentManager.getInstance().attach(app);

        // What handleBindApplication tells the rest of the process about the app it now is.
        Process.setArgV0(info.processName);
        dalvik.system.VMRuntime.setProcessPackageName(info.packageName);
        dalvik.system.VMRuntime.setProcessDataDirectory(info.dataDir);

        // As in handleBindApplication: the Application object, then the app's content
        // providers, and only then Application.onCreate().
        app.application = app.loadedApk.makeApplicationInner(false, null);
        app.thread.mInitialApplication = app.application;
        List<ProviderInfo> providers = new ArrayList<>();
        for (PackageParser.Provider provider : pkg.providers) {
            provider.info.applicationInfo = info;
            providers.add(provider.info);
        }
        if (!providers.isEmpty()) {
            Method install = ActivityThread.class.getDeclaredMethod("installContentProviders",
                    Context.class, List.class);
            install.setAccessible(true);
            install.invoke(app.thread, app.application, providers);
        }
        instrumentation.callApplicationOnCreate(app.application);
        return app;
    }

    /**
     * The device configuration (screen size and density, locale, input devices) that
     * resources are selected by. The window manager service supplies it on a device.
     */
    private static Configuration deviceConfiguration() throws ReflectiveOperationException {
        Display display = DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY);
        DisplayMetrics metrics = new DisplayMetrics();
        display.getRealMetrics(metrics);
        Rect screen = new Rect(0, 0, metrics.widthPixels, metrics.heightPixels);

        Configuration config = new Configuration();
        config.setToDefaults();
        // Screen size in dp, size class, orientation and density.
        Method apply = ResourcesManager.class.getDeclaredMethod("applyDisplayMetricsToConfiguration",
                DisplayMetrics.class, Configuration.class);
        apply.setAccessible(true);
        apply.invoke(null, metrics, config);
        config.setLocales(LocaleList.getDefault());
        config.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        config.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        config.keyboard = Configuration.KEYBOARD_NOKEYS;
        config.keyboardHidden = Configuration.KEYBOARDHIDDEN_NO;
        config.hardKeyboardHidden = Configuration.HARDKEYBOARDHIDDEN_YES;
        config.navigation = Configuration.NAVIGATION_NONAV;
        config.navigationHidden = Configuration.NAVIGATIONHIDDEN_YES;
        config.windowConfiguration.setBounds(screen);
        config.windowConfiguration.setAppBounds(screen);
        config.windowConfiguration.setMaxBounds(screen);
        config.windowConfiguration.setWindowingMode(WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
        config.windowConfiguration.setActivityType(WindowConfiguration.ACTIVITY_TYPE_STANDARD);
        config.windowConfiguration.setRotation(Surface.ROTATION_0);
        config.seq = 1;
        return config;
    }

    private static void setField(Object object, String name, Object value) throws ReflectiveOperationException {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static void setStatic(Class<?> owner, String name, Object value) throws ReflectiveOperationException {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        field.set(null, value);
    }
}
