package android.view;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;

import java.util.ArrayList;

/**
 * The window manager and compositor of this single-app system.
 *
 * Windows (an activity's main window, popup menus, dialogs) are added through the normal
 * android.view.WindowManager API, which ends in {@link WindowManagerGlobal}; here that
 * class hands them to this one. Each window draws itself into its own bitmap
 * ({@link A2IViewRoot}); this class decides where each window sits on the screen, stacks
 * them in the order they were added (system windows above the app's), composes the screen from them whenever one redraws,
 * and routes touches to the right window.
 *
 * All methods must be called on Android's main thread.
 */
public final class A2IWindowManager {
    /** Receives the composed screen each time it changes. */
    public interface ScreenListener {
        void onScreenDrawn(Bitmap screen);
    }

    private static A2IWindowManager sInstance;

    private final int mWidth;
    private final int mHeight;
    private final ScreenListener mListener;
    private final Bitmap mScreen;
    private final Canvas mCanvas;
    private final Paint mDimPaint = new Paint();
    /** Bottom to top. */
    private final ArrayList<A2IViewRoot> mWindows = new ArrayList<>();
    private A2IViewRoot mFocused;
    private A2IViewRoot mTouchTarget;

    private A2IWindowManager(int width, int height, ScreenListener listener) {
        mWidth = width;
        mHeight = height;
        mListener = listener;
        mScreen = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        // The screen is composed pixel for pixel, whatever density the windows declare.
        mScreen.setDensity(Bitmap.DENSITY_NONE);
        mCanvas = new Canvas(mScreen);
        // The framework sends key events that originate in text input (the editor action
        // of a field without one, an input connection's sendKeyEvent) to the window's
        // ViewRootImpl by message. A window's ViewRootImpl here is a placeholder that
        // drops them, so they are picked up as they pass through the message loop and
        // delivered to the window's views.
        android.os.Looper.setObserver(new android.os.Looper.Observer() {
            @Override
            public Object messageDispatchStarting() {
                return null;
            }

            @Override
            public void messageDispatched(Object token, android.os.Message message) {
                if (message.what != 11 || !(message.obj instanceof KeyEvent)) {
                    return;
                }
                for (A2IViewRoot window : mWindows) {
                    if (window.isKeyFromIme(message)) {
                        window.dispatchKeyEvent((KeyEvent) message.obj);
                        return;
                    }
                }
            }

            @Override
            public void dispatchingThrewException(Object token, android.os.Message message, Exception exception) {}
        });
    }

    /** Sets up the screen. Must happen before any window is added. */
    public static void initialize(int width, int height, ScreenListener listener) {
        sInstance = new A2IWindowManager(width, height, listener);
    }

    public static A2IWindowManager getInstance() {
        if (sInstance == null) {
            throw new IllegalStateException("the window manager has not been initialized");
        }
        return sInstance;
    }

    int screenWidth() {
        return mWidth;
    }

    int screenHeight() {
        return mHeight;
    }

    // --- Windows --------------------------------------------------------------------------

    void addWindow(View view, WindowManager.LayoutParams params) {
        if (find(view) != null) {
            throw new IllegalStateException("View " + view + " has already been added to the window manager.");
        }
        A2IViewRoot window = new A2IViewRoot(this, view, params);
        // Newer windows go on top, but an app's windows never cover system windows such
        // as toasts.
        int index = mWindows.size();
        if (!isSystemWindow(params)) {
            while (index > 0 && isSystemWindow(mWindows.get(index - 1).mParams)) {
                index--;
            }
        }
        mWindows.add(index, window);
        window.attach();
        updateFocus();
    }

    private static boolean isSystemWindow(WindowManager.LayoutParams params) {
        return params.type >= WindowManager.LayoutParams.FIRST_SYSTEM_WINDOW;
    }

    void updateWindow(View view, WindowManager.LayoutParams params) {
        A2IViewRoot window = find(view);
        if (window == null) {
            throw new IllegalArgumentException("View " + view + " not attached to window manager");
        }
        window.setLayoutParams(params);
        updateFocus();
    }

    void removeWindow(View view) {
        A2IViewRoot window = find(view);
        if (window == null) {
            throw new IllegalArgumentException("View " + view + " not attached to window manager");
        }
        mWindows.remove(window);
        if (mTouchTarget == window) {
            mTouchTarget = null;
        }
        if (mFocused == window) {
            mFocused = null;
        }
        window.detach();
        updateFocus();
        compose();
    }

    ArrayList<View> windowViews() {
        ArrayList<View> views = new ArrayList<>();
        for (A2IViewRoot window : mWindows) {
            views.add(window.mView);
        }
        return views;
    }

    private A2IViewRoot find(View view) {
        for (A2IViewRoot window : mWindows) {
            if (window.mView == view) {
                return window;
            }
        }
        return null;
    }

    /** Computes a window's frame on screen from its layout parameters and measured size. */
    void placeWindow(WindowManager.LayoutParams params, int width, int height, Rect outFrame) {
        Rect screen = new Rect(0, 0, mWidth, mHeight);
        Gravity.apply(params.gravity, width, height, screen,
                (int) (params.x + params.horizontalMargin * mWidth),
                (int) (params.y + params.verticalMargin * mHeight), outFrame);
        // As on a device, a window is kept on screen unless it asks not to be.
        if ((params.flags & WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS) == 0) {
            Gravity.applyDisplay(params.gravity, screen, outFrame);
        }
    }

    /** The topmost window that takes input focus has it. */
    private void updateFocus() {
        A2IViewRoot focused = null;
        for (int i = mWindows.size() - 1; i >= 0 && focused == null; i--) {
            A2IViewRoot window = mWindows.get(i);
            if (window.isVisible() && (window.mParams.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0) {
                focused = window;
            }
        }
        if (focused != mFocused) {
            if (mFocused != null) {
                mFocused.setWindowFocus(false);
            }
            mFocused = focused;
            if (focused != null) {
                focused.setWindowFocus(true);
            }
        }
    }

    // --- Composition ----------------------------------------------------------------------

    /** Called by a window after it redrew its bitmap. */
    void windowDrawn() {
        compose();
    }

    /** Called by a window after it was hidden or shown. */
    void windowVisibilityChanged() {
        updateFocus();
        compose();
    }

    private void compose() {
        long composeStart = System.nanoTime();
        if (sOnWindowsChanged != null) {
            sOnWindowsChanged.run();
        }
        mCanvas.drawColor(Color.BLACK, PorterDuff.Mode.SRC);
        for (A2IViewRoot window : mWindows) {
            if (window.mBitmap == null || !window.isVisible()) {
                continue;
            }
            if ((window.mParams.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0) {
                mDimPaint.setColor(Color.argb(Math.round(window.mParams.dimAmount * 255), 0, 0, 0));
                mCanvas.drawRect(0, 0, mWidth, mHeight, mDimPaint);
            }
            mCanvas.drawBitmap(window.mBitmap, window.mFrame.left, window.mFrame.top, null);
        }
        a2i.boot.FrameStats.add(a2i.boot.FrameStats.COMPOSE, System.nanoTime() - composeStart);
        mListener.onScreenDrawn(mScreen);
    }

    // --- Input ----------------------------------------------------------------------------

    /**
     * Delivers a touch, given in screen coordinates. A gesture goes to the window chosen
     * when it starts: the topmost one under the finger, except that a window which has not
     * declared itself non-modal takes every touch while it is on top (that is how tapping
     * outside a menu dismisses it).
     */
    public void dispatchTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            mTouchTarget = null;
            int x = (int) event.getX(), y = (int) event.getY();
            for (int i = mWindows.size() - 1; i >= 0; i--) {
                A2IViewRoot window = mWindows.get(i);
                int flags = window.mParams.flags;
                if ((flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0 || window.mBitmap == null
                        || !window.isVisible()) {
                    continue;
                }
                boolean modal = (flags & (WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)) == 0;
                if (modal || window.mFrame.contains(x, y)) {
                    mTouchTarget = window;
                    break;
                }
            }
        }
        A2IViewRoot target = mTouchTarget;
        if (target == null) {
            return;
        }
        event.offsetLocation(-target.mFrame.left, -target.mFrame.top);
        target.dispatchTouchEvent(event);
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            mTouchTarget = null;
        }
    }

    /**
     * The bounds on screen of the view that has input focus in the focused window.
     * Returns false if there is none.
     */
    public boolean getFocusedViewBounds(Rect outBounds) {
        View focus = mFocused != null ? mFocused.mView.findFocus() : null;
        if (focus == null) {
            return false;
        }
        int[] location = new int[2];
        focus.getLocationOnScreen(location);
        outBounds.set(location[0], location[1], location[0] + focus.getWidth(), location[1] + focus.getHeight());
        return true;
    }

    /**
     * Describes every window and its view hierarchy (class, id, position, visibility,
     * opacity), for finding out why something is not on screen.
     */
    public String dump() {
        StringBuilder out = new StringBuilder();
        for (A2IViewRoot window : mWindows) {
            out.append("window type=").append(window.mParams.type)
                    .append(" frame=").append(window.mFrame.toShortString())
                    .append(window.isVisible() ? "" : " hidden")
                    .append(window == mFocused ? " focused" : "")
                    .append(" title=").append(window.mParams.getTitle()).append('\n');
            dump(window.mView, 1, out);
        }
        return out.toString();
    }

    private static void dump(View view, int depth, StringBuilder out) {
        for (int i = 0; i < depth; i++) {
            out.append("  ");
        }
        String id = "";
        if (view.getId() != View.NO_ID) {
            try {
                id = " #" + view.getResources().getResourceEntryName(view.getId());
            } catch (android.content.res.Resources.NotFoundException e) {
                id = " #" + Integer.toHexString(view.getId());
            }
        }
        out.append(view.getClass().getSimpleName()).append(id)
                .append(' ').append(view.getLeft()).append(',').append(view.getTop())
                .append(' ').append(view.getWidth()).append('x').append(view.getHeight());
        if (view.getVisibility() != View.VISIBLE) {
            out.append(view.getVisibility() == View.GONE ? " GONE" : " INVISIBLE");
        }
        if (view.getAlpha() != 1f) {
            out.append(" alpha=").append(view.getAlpha());
        }
        if (view.getTranslationX() != 0 || view.getTranslationY() != 0) {
            out.append(" translation=").append(view.getTranslationX()).append(',').append(view.getTranslationY());
        }
        if (view instanceof android.widget.TextView) {
            CharSequence text = ((android.widget.TextView) view).getText();
            if (text != null && text.length() > 0) {
                out.append(" \"").append(text.length() > 30 ? text.subSequence(0, 30) + "…" : text).append('"');
            }
        }
        out.append('\n');
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                dump(group.getChildAt(i), depth + 1, out);
            }
        }
    }

    private static Runnable sOnWindowsChanged;

    /** Sets something to run whenever a window is added, removed, shown, hidden or redrawn. */
    public static void setOnWindowsChanged(Runnable listener) {
        sOnWindowsChanged = listener;
    }

    /**
     * Whether nothing is in front of the window that {@code view} is in: no other visible
     * window above it, apart from those that take no touches (toasts).
     */
    public boolean isInTopWindow(View view) {
        View root = view.getRootView();
        for (int i = mWindows.size() - 1; i >= 0; i--) {
            A2IViewRoot window = mWindows.get(i);
            if (window.mView == root) {
                return window.isVisible();
            }
            if (window.isVisible()
                    && (window.mParams.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0) {
                return false;
            }
        }
        return false;
    }

    /** The view that has input focus in the focused window, or null. */
    public View getFocusedView() {
        return mFocused != null ? mFocused.mView.findFocus() : null;
    }

    /** Delivers a key event to the window that has input focus. */
    public void dispatchKeyEvent(KeyEvent event) {
        if (mFocused != null) {
            mFocused.dispatchKeyEvent(event);
        }
    }
}
