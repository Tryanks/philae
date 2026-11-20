package a2i.smoke;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import java.util.stream.IntStream;

/**
 * Runtime smoke test: plain Java exercising the parts of the runtime a real app leans on
 * before it ever touches the Android framework. Each check prints one line; a failed
 * check throws, so reaching "ALL OK" means every line above it held.
 */
public final class Main {
    interface Shape { double area(); }

    static final class Circle implements Shape {
        final double r;
        Circle(double r) { this.r = r; }
        public double area() { return Math.PI * r * r; }
    }

    static final class Square implements Shape {
        final double side;
        Square(double side) { this.side = side; }
        public double area() { return side * side; }
    }

    @Retention(RetentionPolicy.RUNTIME)
    @interface Tag {
        String value();
        int weight() default 7;
    }

    @Tag("smoke")
    static final class Tagged {}

    private static void check(String what, boolean ok) {
        if (!ok) throw new AssertionError(what);
        System.out.println("ok  " + what);
    }

    public static void main(String[] args) throws Exception {
        System.out.println("args: " + String.join(",", args));

        // Arithmetic, strings, boxing.
        long sum = 0;
        for (int i = 1; i <= 100_000; i++) sum += (long) i * i % 7;
        check("integer loop", sum == 200_003);
        check("string concat and switch", describe(3).equals("three:3.5:true"));
        check("string format", String.format("%05d|%-4s|%.3f|%x", 42, "ab", Math.E, 255).equals("00042|ab  |2.718|ff"));
        check("utf-8 round trip", new String("日本語 émoji 😀".getBytes(StandardCharsets.UTF_8), StandardCharsets.UTF_8).length() == 12);

        // Collections, lambdas, streams.
        Map<String, Integer> counts = new HashMap<>();
        for (String word : "the quick brown fox jumps over the lazy dog the end".split(" ")) {
            counts.merge(word, 1, Integer::sum);
        }
        check("hash map merge", counts.get("the") == 3 && counts.size() == 9);
        List<Integer> squares = IntStream.rangeClosed(1, 10).map(x -> x * x).boxed().collect(Collectors.toList());
        check("streams", squares.stream().mapToInt(Integer::intValue).sum() == 385);

        // Virtual and interface dispatch.
        List<Shape> shapes = new ArrayList<>();
        shapes.add(new Circle(1));
        shapes.add(new Square(2));
        double area = 0;
        for (Shape s : shapes) area += s.area();
        check("interface dispatch", Math.abs(area - (Math.PI + 4)) < 1e-12);

        // Exceptions with stack traces.
        String caught;
        try {
            Object o = "text";
            Integer bad = (Integer) o;
            caught = "none " + bad;
        } catch (ClassCastException e) {
            caught = e.getStackTrace()[0].getMethodName();
        }
        check("exception and stack trace", caught.equals("main"));
        try {
            recurse(0);
            check("stack overflow", false);
        } catch (StackOverflowError e) {
            check("stack overflow is catchable", true);
        }

        // Reflection.
        Method m = Main.class.getDeclaredMethod("describe", int.class);
        check("reflection", m.invoke(null, 1).equals("one:1.5:false"));

        // Instance methods through reflection, dynamic proxies, runtime annotations.
        Method areaMethod = Shape.class.getMethod("area");
        check("reflective instance call", ((Double) areaMethod.invoke(new Square(3))) == 9.0);
        final StringBuilder calls = new StringBuilder();
        Shape proxy = (Shape) Proxy.newProxyInstance(Main.class.getClassLoader(), new Class<?>[] {Shape.class},
                (self, method, methodArgs) -> {
                    calls.append(method.getName()).append(' ');
                    return method.getName().equals("area") ? (Object) 42.0 : "a proxied shape";
                });
        check("dynamic proxy", proxy.area() == 42.0 && proxy.toString().equals("a proxied shape")
                && calls.toString().equals("area toString ") && Proxy.isProxyClass(proxy.getClass()));
        Tag tag = Tagged.class.getAnnotation(Tag.class);
        check("runtime annotation", tag != null && tag.value().equals("smoke") && tag.weight() == 7);

        // Regular expressions (backed by ICU through libicu_jni).
        Matcher matcher = Pattern.compile("(\\d{4})-(\\d{2})-(\\d{2})").matcher("released 2026-11-19, built 2026-11-18");
        int dates = 0;
        while (matcher.find()) dates += Integer.parseInt(matcher.group(3));
        check("regex", dates == 37);

        // Threads, monitors, concurrent collections.
        final int threads = 8, perThread = 20_000;
        AtomicLong atomic = new AtomicLong();
        long[] guarded = new long[1];
        ConcurrentHashMap<Integer, Integer> shared = new ConcurrentHashMap<>();
        ExecutorService pool = Executors.newFixedThreadPool(threads);
        CountDownLatch done = new CountDownLatch(threads);
        for (int t = 0; t < threads; t++) {
            final int id = t;
            pool.execute(() -> {
                for (int i = 0; i < perThread; i++) {
                    atomic.incrementAndGet();
                    synchronized (guarded) { guarded[0]++; }
                    shared.merge(i % 100, 1, Integer::sum);
                }
                shared.put(1000 + id, id);
                done.countDown();
            });
        }
        done.await();
        pool.shutdown();
        check("threads and monitors", atomic.get() == threads * perThread && guarded[0] == threads * perThread
                && shared.size() == 100 + threads && shared.get(7) == threads * perThread / 100);

        // Allocation pressure: forces several garbage collections.
        long checksum = 0;
        for (int round = 0; round < 200; round++) {
            List<byte[]> garbage = new ArrayList<>();
            for (int i = 0; i < 200; i++) {
                byte[] block = new byte[4096];
                block[i] = (byte) round;
                garbage.add(block);
            }
            checksum += garbage.get(round % 200)[round % 200];
        }
        check("garbage collection", checksum == 1468);

        // File I/O in the data directory.
        File file = new File(System.getProperty("a2i.tmp", "/tmp"), "a2i-smoke.txt");
        byte[] payload = "written by Android code on iOS\n".getBytes(StandardCharsets.UTF_8);
        try (FileOutputStream out = new FileOutputStream(file)) { out.write(payload); }
        byte[] back = new byte[payload.length];
        try (FileInputStream in = new FileInputStream(file)) { check("file read", in.read(back) == payload.length); }
        check("file round trip", new String(back, StandardCharsets.UTF_8).startsWith("written by Android") && file.delete());

        System.out.println("ALL OK on " + System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version"));
    }

    static String describe(int n) {
        String name;
        switch (n) {
            case 1: name = "one"; break;
            case 3: name = "three"; break;
            default: name = "many";
        }
        return name + ":" + (n + 0.5) + ":" + (n > 2);
    }

    static int recurse(int depth) {
        return recurse(depth + 1) + 1;
    }
}
