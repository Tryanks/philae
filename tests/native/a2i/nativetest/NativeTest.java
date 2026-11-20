package a2i.nativetest;

/**
 * Exercises JNI between the runtime and a native library built for Android
 * (tests/native/android_jni_test.c). Run by tests/native/run.sh.
 */
public final class NativeTest {
    static native int add(int a, int b);
    static native String describe(String text, int n, double d, long l);
    static native double manyArguments(int a, long b, float c, double d, int e, long f, float g, double h, int i,
            long j, float k, double l, int m, byte n, short o, float p, double q, float r, int s);
    native int twice(int value);
    static native int callBack(Object target);
    static native void throwSomething();
    static native int runThread();

    static int mix(int a, long b, float c, double d, String e, boolean f, byte g, char h, short i) {
        boolean ok = a == 1 && b == 1L << 40 && c == 2.5f && d == 3.25 && e.equals("abc") && f && g == -3 && h == 'x'
                && i == -7;
        return ok ? 77 : -1;
    }

    static double many(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, double k, double l,
            double m, double n, double o, double p, double q, double r, double s, double t) {
        return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + s + t;
    }

    public String join(String text, int n) {
        return text + n;
    }

    static int fromThread(int value, String name) {
        return name.equals("thread") && !Thread.currentThread().getName().equals("main") ? value * 2 : -1;
    }

    private static int sFailures;

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok) {
            sFailures++;
        }
    }

    public static void main(String[] args) {
        System.load(args[0]);
        check(true, "library loaded and JNI_OnLoad succeeded");
        check(add(40, 2) == 42, "static native method");
        check("héllo/7/2.50/1099511627776".equals(describe("héllo", 7, 2.5, 1L << 40)), "strings in and out");
        check(manyArguments(1, 2, 3f, 4, 5, 6, 7f, 8, 9, 10, 11f, 12, 13, (byte) 14, (short) 15, 16f, 17, 18f, 19)
                == 190.0, "nineteen arguments, some on the stack");
        check(new NativeTest().twice(21) == 42, "method registered with RegisterNatives");
        int callbacks = callBack(new NativeTest());
        check(callbacks == 0, "native code calling Java: variadic, va_list and array forms"
                + (callbacks == 0 ? "" : " (check " + callbacks + ")"));
        boolean thrown = false;
        try {
            throwSomething();
        } catch (IllegalStateException e) {
            thrown = "thrown from native code".equals(e.getMessage());
        }
        check(thrown, "exception thrown from native code");
        check(runThread() == 42, "native thread attaching and calling Java");
        System.out.println(sFailures == 0 ? "ALL OK" : sFailures + " FAILED");
        System.exit(sFailures == 0 ? 0 : 1);
    }
}
