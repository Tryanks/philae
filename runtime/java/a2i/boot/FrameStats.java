package a2i.boot;

import android.util.Log;

/**
 * Where the time of a frame goes: logged every 30 frames as the average milliseconds each
 * stage took per frame, and the rate at which frames were produced. For judging
 * rendering performance on a device. All calls are on Android's main thread.
 */
public final class FrameStats {
    private FrameStats() {}

    public static final int DRAW = 0;         // the view hierarchy drawing into its window
    public static final int COMPOSE = 1;      // windows composed into the screen
    public static final int READ_PIXELS = 2;  // the screen read out of its bitmap
    public static final int PRESENT = 3;      // handed to the host
    private static final String[] NAMES = {"draw", "compose", "readPixels", "present"};
    private static final int WINDOW = 30;

    private static final long[] sNanos = new long[NAMES.length];
    private static int sFrames;
    private static long sWindowStart = System.nanoTime();

    public static void add(int stage, long nanos) {
        sNanos[stage] += nanos;
    }

    /** Marks the end of a frame. */
    public static void frame() {
        if (++sFrames < WINDOW) {
            return;
        }
        long now = System.nanoTime();
        StringBuilder line = new StringBuilder();
        long accounted = 0;
        for (int i = 0; i < NAMES.length; i++) {
            line.append(NAMES[i]).append('=').append(String.format("%.1f", sNanos[i] / 1e6 / sFrames)).append("ms ");
            accounted += sNanos[i];
            sNanos[i] = 0;
        }
        double seconds = (now - sWindowStart) / 1e9;
        Log.i("a2i.Frames", line + String.format("total=%.1fms  %d frames in %.1fs (%.1f fps)",
                accounted / 1e6 / sFrames, sFrames, seconds, sFrames / seconds));
        sFrames = 0;
        sWindowStart = now;
    }
}
