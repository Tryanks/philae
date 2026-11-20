package a2i.boot;

import android.graphics.Typeface;
import android.os.SystemProperties;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

/**
 * Brings the Android platform up inside the process: everything that on a device is done
 * by init, zygote and the native half of the framework before an app's first class loads.
 */
public final class Platform {
    private Platform() {}

    /**
     * Framework classes whose native methods are implemented by libandroid_runtime, AOSP's
     * host build of the framework's native code. The library registers exactly the classes
     * it is asked for (see HostRuntime.cpp in frameworks/base), so these lists must name
     * only classes it knows.
     */
    private static final String[] CORE_NATIVE_CLASSES = {
        "android.animation.PropertyValuesHolder",
        "android.database.CursorWindow",
        "android.database.sqlite.SQLiteConnection",
        "android.database.sqlite.SQLiteDebug",
        "android.database.sqlite.SQLiteGlobal",
        "android.database.sqlite.SQLiteRawStatement",
        "android.media.ImageReader",
        "android.media.PublicFormatUtils",
        "android.text.AndroidCharacter",
        "android.text.Hyphenator",
        "android.util.EventLog",
        "android.util.jar.StrictJarFile",
        "android.view.InputDevice",
        "android.view.KeyCharacterMap",
        "android.view.KeyEvent",
        "android.view.MotionEvent",
        "android.view.Surface",
        "android.view.VelocityTracker",
        "com.android.internal.util.VirtualRefBasePtr",
        "android.os.Trace",
        "android.util.Log"
    };

    // Registration order matters: Graphics caches classes and fields of the others (it
    // initialises android.graphics.ColorSpace, whose static initialiser already calls
    // natives), so it comes after them.
    private static final String[] GRAPHICS_NATIVE_CLASSES = {
        "android.graphics.animation.NativeInterpolatorFactory",
        "android.graphics.animation.RenderNodeAnimator",
        "android.graphics.Bitmap",
        "android.graphics.BitmapFactory",
        "android.graphics.BitmapRegionDecoder",
        "android.graphics.ByteBufferStreamAdaptor",
        "android.graphics.Camera",
        "android.graphics.Canvas",
        "android.graphics.CanvasProperty",
        "android.graphics.Color",
        "android.graphics.ColorFilter",
        "android.graphics.ColorSpace",
        "android.graphics.CreateJavaOutputStreamAdaptor",
        "android.graphics.drawable.AnimatedImageDrawable",
        "android.graphics.drawable.AnimatedVectorDrawable",
        "android.graphics.drawable.VectorDrawable",
        "android.graphics.DrawFilter",
        "android.graphics.FontFamily",
        "android.graphics.fonts.Font",
        "android.graphics.fonts.FontFamily",
        "android.graphics.Gainmap",
        "android.graphics.HardwareRenderer",
        "android.graphics.HardwareRendererObserver",
        "android.graphics.ImageDecoder",
        "android.graphics.Interpolator",
        "android.graphics.MaskFilter",
        "android.graphics.Matrix",
        "android.graphics.Mesh",
        "android.graphics.MeshSpecification",
        "android.graphics.NinePatch",
        "android.graphics.Paint",
        "android.graphics.Path",
        "android.graphics.PathEffect",
        "android.graphics.PathIterator",
        "android.graphics.PathMeasure",
        "android.graphics.Picture",
        "android.graphics.RecordingCanvas",
        "android.graphics.Region",
        "android.graphics.RenderEffect",
        "android.graphics.RenderNode",
        "android.graphics.Shader",
        "android.graphics.text.GraphemeBreak",
        "android.graphics.text.LineBreaker",
        "android.graphics.text.MeasuredText",
        "android.graphics.text.TextRunShaper",
        "android.graphics.Typeface",
        "android.graphics.YuvImage",
        "android.util.PathParser",
        "android.graphics.Graphics",
        "android.graphics.HardwareBufferRenderer"
    };

    private static boolean sInitialized;

    /**
     * @param androidRoot the bundled system directory (Android's /system)
     * @param nativeLibraryDir directory holding the runtime's native libraries
     */
    public static synchronized void init(String androidRoot, String nativeLibraryDir) throws IOException {
        if (sInitialized) {
            return;
        }
        // The host runtime takes its configuration from Java system properties.
        System.setProperty("core_native_classes", String.join(",", CORE_NATIVE_CLASSES));
        System.setProperty("graphics_native_classes", String.join(",", GRAPHICS_NATIVE_CLASSES));
        System.setProperty("icu.data.path", androidRoot + "/apex/com.android.i18n/etc/icu/icudt75l.dat");
        System.load(nativeLibraryDir + "/libandroid_runtime.dylib");
        // The framework natives built by this project: the resource system.
        System.load(nativeLibraryDir + "/libandroid_framework.dylib");

        // android.os.Build and much else read these in their static initialisers, so they
        // must be defined before any other framework code runs.
        loadSystemProperties(new File(androidRoot, "build.prop"));

        // What zygote does before forking apps: read fonts.xml and build the system font
        // families. Text cannot be measured or drawn until the default typeface exists.
        Typeface.loadPreinstalledSystemFontMap();
        sInitialized = true;
    }

    private static void loadSystemProperties(File file) throws IOException {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            for (String line = reader.readLine(); line != null; line = reader.readLine()) {
                line = line.trim();
                int equals = line.indexOf('=');
                if (line.startsWith("#") || equals <= 0) {
                    continue;
                }
                String key = line.substring(0, equals);
                String value = line.substring(equals + 1);
                SystemProperties.set(key, value);
            }
        }
    }
}
