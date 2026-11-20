package a2i.boot;

import java.lang.reflect.Constructor;
import java.util.Enumeration;

/**
 * Java side of the runtime host. The native host calls into this class once ART is up;
 * it is on the boot class path, so it sees the platform's own classes.
 *
 * For now it only reports how far the embedded app gets: the runtime exists, the Android
 * framework (android.app.Activity and everything around it) does not.
 *
 * dalvik.system classes are reached reflectively because this file is compiled against
 * the JDK, which has no such package.
 */
public final class Boot {
    private Boot() {}

    /** One line identifying the runtime, e.g. "Dalvik 2.1.0, 8 processors, heap limit 256 MB". */
    public static String runtimeInfo() {
        return System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version")
                + ", " + Runtime.getRuntime().availableProcessors() + " processors, heap limit "
                + (Runtime.getRuntime().maxMemory() >> 20) + " MB";
    }

    /** Number of classes defined by the APK's dex files, or a description of the failure. */
    public static String countClasses(String apkPath) {
        try {
            Class<?> dexFileClass = Class.forName("dalvik.system.DexFile");
            Object dexFile = dexFileClass.getConstructor(String.class).newInstance(apkPath);
            Enumeration<?> names = (Enumeration<?>) dexFileClass.getMethod("entries").invoke(dexFile);
            int count = 0;
            while (names.hasMoreElements()) {
                names.nextElement();
                count++;
            }
            dexFileClass.getMethod("close").invoke(dexFile);
            return count + " classes";
        } catch (Throwable t) {
            return "unreadable: " + describe(t);
        }
    }

    /**
     * Opens the app's code with the class loader Android would use and tries to resolve its
     * launcher activity. Returns a one-line outcome for display; on failure it names the
     * class at the end of the dependency chain that could not be found.
     */
    public static String resolveLauncher(String apkPath, String activityName) {
        ClassLoader loader;
        try {
            Constructor<?> constructor = Class.forName("dalvik.system.PathClassLoader")
                    .getConstructor(String.class, ClassLoader.class);
            loader = (ClassLoader) constructor.newInstance(apkPath, Boot.class.getClassLoader());
        } catch (Throwable t) {
            return "cannot open the APK: " + describe(t);
        }

        // A class whose superclass or interface is missing first fails with a bare
        // ClassNotFoundException; asking again reports which dependency failed
        // ("Failed resolution of: Lsome/Class;"). Follow that chain to its end.
        String name = activityName;
        boolean retried = false;
        for (int step = 0; step < 100; step++) {
            try {
                Class<?> resolved = Class.forName(name, false, loader);
                if (name.equals(activityName)) {
                    return "resolved, extends " + resolved.getSuperclass().getName();
                }
                return "cannot be resolved: " + describe(new IllegalStateException("inconsistent failure at " + name));
            } catch (NoClassDefFoundError e) {
                String dependency = failedResolution(e.getMessage());
                if (dependency == null || dependency.equals(name)) {
                    return "cannot be resolved: " + describe(e);
                }
                name = dependency;
                retried = false;
            } catch (ClassNotFoundException e) {
                if (retried) {
                    return "cannot be resolved: it needs " + name + ", which does not exist";
                }
                retried = true;
            } catch (Throwable t) {
                return "cannot be resolved: " + describe(t);
            }
        }
        return "cannot be resolved: dependency chain too long at " + name;
    }

    /** "Failed resolution of: Lcom/example/Foo;" → "com.example.Foo". */
    private static String failedResolution(String message) {
        final String prefix = "Failed resolution of: L";
        if (message == null || !message.startsWith(prefix) || !message.endsWith(";")) {
            return null;
        }
        return message.substring(prefix.length(), message.length() - 1).replace('/', '.');
    }

    /** The innermost cause, which for linkage errors names the class that is missing. */
    private static String describe(Throwable t) {
        while (t.getCause() != null) t = t.getCause();
        String message = String.valueOf(t.getMessage());
        int newline = message.indexOf('\n');
        return t.getClass().getSimpleName() + ": " + (newline < 0 ? message : message.substring(0, newline));
    }
}
