package android.view;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.accessibility.AccessibilityEvent;

/**
 * The root of one window's view hierarchy, rendering in software into a bitmap.
 *
 * On a device this role belongs to ViewRootImpl, which is bound to the window manager
 * service and SurfaceFlinger: it negotiates a surface, receives input over a socket, and
 * draws through the render thread. None of those exist here. This class implements the
 * part of the contract that the view hierarchy itself depends on: it is the hierarchy's
 * parent (so requestLayout() and invalidate() reach it), it provides the AttachInfo that
 * makes views consider themselves attached to a window, and on each display frame it
 * measures, lays out and draws the hierarchy if anything asked for it. Where the window
 * goes on screen and how windows stack is {@link A2IWindowManager}'s business.
 *
 * It lives in android.view because attaching views is package-private there.
 */
final class A2IViewRoot implements ViewParent {
    final View mView;
    final WindowManager.LayoutParams mParams = new WindowManager.LayoutParams();
    /** Where the window is on screen; empty until the first layout. */
    final Rect mFrame = new Rect();
    /** The window's contents, or null before the first draw. */
    Bitmap mBitmap;

    private final A2IWindowManager mManager;
    private final Choreographer mChoreographer;
    private Canvas mCanvas;
    private boolean mLayoutRequested = true;
    private boolean mApplyInsetsRequested = true;
    private boolean mDrawRequested = true;
    private boolean mTraversalScheduled;
    private boolean mAttached;
    private boolean mVisible;
    private View.AttachInfo mAttachInfo;
    private ViewRootImpl mPlaceholder;

    private final Runnable mTraversal = this::performTraversal;

    /** Must be called on the main thread. */
    A2IViewRoot(A2IWindowManager manager, View view, WindowManager.LayoutParams params) {
        mManager = manager;
        mView = view;
        mParams.copyFrom(params);
        mChoreographer = Choreographer.getInstance();
    }

    /** Attaches the view hierarchy to this root, as adding a window does, and schedules the first frame. */
    void attach() {
        Context context = mView.getContext();
        Display display = context.getDisplayNoVerify();
        // AttachInfo wants a ViewRootImpl for the helpers views reach through it (a window's
        // decor view registers callbacks with it when attached). One that was constructed
        // but never given a view serves: it has no window or surface, and the view
        // hierarchy's parent is this object, not it.
        ViewRootImpl placeholder = new ViewRootImpl(context, display);
        // Views ask it for the display area available to windows (popups size themselves
        // by it); without a window manager service it would report an empty one.
        try {
            java.lang.reflect.Field frames = ViewRootImpl.class.getDeclaredField("mTmpFrames");
            frames.setAccessible(true);
            ((android.window.ClientWindowFrames) frames.get(placeholder)).displayFrame
                    .set(0, 0, mManager.screenWidth(), mManager.screenHeight());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
        IWindow window = (IWindow) a2i.boot.Services.createInterface(
                "window-client", IWindow.class, null, new android.os.Binder());
        // No effect player: click sounds and haptics are not routed to iOS yet, and the
        // placeholder cannot play them.
        View.AttachInfo info = new View.AttachInfo(WindowManagerGlobal.getWindowSession(), window, display,
                placeholder, new Handler(Looper.myLooper()), null /* effectPlayer */, context);
        mPlaceholder = placeholder;
        // The input method manager reads the window's attributes from the ViewRootImpl.
        placeholder.mWindowAttributes.copyFrom(mParams);
        mVisible = mView.getVisibility() == View.VISIBLE;
        info.mHasWindowFocus = false;
        info.mWindowVisibility = mView.getVisibility();
        info.mInTouchMode = true;
        info.mDisplayState = Display.STATE_ON;
        mAttachInfo = info;

        mView.assignParent(this);
        mView.dispatchAttachedToWindow(info, mView.getVisibility());
        mAttached = true;
        scheduleTraversal();
    }

    /** Detaches the hierarchy, as removing a window does. */
    void detach() {
        if (mAttached) {
            mAttached = false;
            mChoreographer.removeCallbacks(Choreographer.CALLBACK_TRAVERSAL, mTraversal, null);
            mTraversalScheduled = false;
            mPlaceholder.getImeFocusController().onWindowDismissed();
            mView.dispatchDetachedFromWindow();
            mView.assignParent(null);
        }
    }

    /** Whether a message is the placeholder ViewRootImpl's "key event from the input method". */
    boolean isKeyFromIme(android.os.Message message) {
        return mAttached && message.getTarget() == mPlaceholder.mHandler
                && message.what == MSG_DISPATCH_KEY_FROM_IME && message.obj instanceof KeyEvent;
    }

    private static final int MSG_DISPATCH_KEY_FROM_IME = 11;  // ViewRootImpl's, which is private

    void setLayoutParams(WindowManager.LayoutParams params) {
        mParams.copyFrom(params);
        mPlaceholder.mWindowAttributes.copyFrom(mParams);
        requestLayout();
    }

    /** Whether the window is on screen. A window is hidden by hiding its root view. */
    boolean isVisible() {
        return mAttached && mVisible;
    }

    /** Delivers a touch event, already translated into this window's coordinates. */
    boolean dispatchTouchEvent(MotionEvent event) {
        return mAttached && mView.dispatchPointerEvent(event);
    }

    /**
     * Delivers a key event. The back key first goes to the callback the window registered
     * for it, if the app has opted in to that mechanism, and otherwise travels through
     * the view hierarchy like any other key (ViewRootImpl's input stages do the same).
     */
    boolean dispatchKeyEvent(KeyEvent event) {
        if (!mAttached) {
            return false;
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            android.window.WindowOnBackInvokedDispatcher dispatcher = mPlaceholder.getOnBackInvokedDispatcher();
            if (dispatcher.isOnBackInvokedCallbackEnabled()) {
                android.window.OnBackInvokedCallback callback = dispatcher.getTopCallback();
                if (callback != null) {
                    if (event.getAction() == KeyEvent.ACTION_UP) {
                        callback.onBackInvoked();
                    }
                    return true;
                }
            }
        }
        return mView.dispatchKeyEventPreIme(event) || mView.dispatchKeyEvent(event);
    }

    /**
     * Gives the window input focus or takes it away, as ViewRootImpl does when the window
     * manager says so: besides the views, the input method machinery is told, which is
     * what makes the focused text field the one that typing goes to.
     */
    void setWindowFocus(boolean focused) {
        if (!mAttached) {
            return;
        }
        ImeFocusController ime = mPlaceholder.getImeFocusController();
        mAttachInfo.mHasWindowFocus = focused;
        ime.onPreWindowFocus(focused, mParams);
        mAttachInfo.mKeyDispatchState.reset();
        mView.dispatchWindowFocusChanged(focused);
        mAttachInfo.mTreeObserver.dispatchOnWindowFocusChange(focused);
        View focus = mView.findFocus();
        ime.onPostWindowFocus(focus != null ? focus : mView, focused, mParams);
        if (focused) {
            // "Forward navigation" only applies to the first time the window gets focus.
            mParams.softInputMode &= ~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION;
            mPlaceholder.mWindowAttributes.softInputMode = mParams.softInputMode;
        }
    }

    private void scheduleTraversal() {
        if (mAttached && !mTraversalScheduled) {
            mTraversalScheduled = true;
            mChoreographer.postCallback(Choreographer.CALLBACK_TRAVERSAL, mTraversal, null);
        }
    }

    private static int measureSpec(int requested, int available) {
        if (requested == ViewGroup.LayoutParams.MATCH_PARENT) {
            return View.MeasureSpec.makeMeasureSpec(available, View.MeasureSpec.EXACTLY);
        }
        if (requested == ViewGroup.LayoutParams.WRAP_CONTENT) {
            return View.MeasureSpec.makeMeasureSpec(available, View.MeasureSpec.AT_MOST);
        }
        return View.MeasureSpec.makeMeasureSpec(Math.min(requested, available), View.MeasureSpec.EXACTLY);
    }

    private void performTraversal() {
        mTraversalScheduled = false;
        if (!mAttached) {
            return;
        }
        boolean visible = mView.getVisibility() == View.VISIBLE;
        if (visible != mVisible) {
            mVisible = visible;
            mAttachInfo.mWindowVisibility = mView.getVisibility();
            mView.dispatchWindowVisibilityChanged(mView.getVisibility());
            mLayoutRequested |= visible;
            mManager.windowVisibilityChanged();
        }
        if (!visible) {
            return;  // nothing to show; requests stay pending until the window is visible again
        }
        if (mApplyInsetsRequested) {
            // Window insets tell the hierarchy how much of the window is covered by system
            // bars, the keyboard and display cutouts. Android's screen here is the area
            // iOS leaves free of those, so the answer is "nothing". It still has to be
            // given: apps lay themselves out in their insets listeners (a bar-sized scrim
            // stays full-screen until it hears how tall the bar is).
            mApplyInsetsRequested = false;
            // (Insets that were never set count as already consumed and would not be
            // passed down the hierarchy, hence the explicit zeroes.)
            mView.dispatchApplyWindowInsets(new WindowInsets.Builder()
                    .setInsets(WindowInsets.Type.systemBars(), android.graphics.Insets.NONE)
                    .setInsetsIgnoringVisibility(WindowInsets.Type.systemBars(), android.graphics.Insets.NONE)
                    .setVisible(WindowInsets.Type.systemBars(), true)
                    .setDisplayCutout(DisplayCutout.NO_CUTOUT)
                    .build());
        }
        if (mLayoutRequested) {
            mLayoutRequested = false;
            mView.measure(measureSpec(mParams.width, mManager.screenWidth()),
                    measureSpec(mParams.height, mManager.screenHeight()));
            int width = Math.max(mView.getMeasuredWidth(), 1);
            int height = Math.max(mView.getMeasuredHeight(), 1);
            mManager.placeWindow(mParams, width, height, mFrame);
            // Views compute their on-screen position from the window's.
            mAttachInfo.mWindowLeft = mFrame.left;
            mAttachInfo.mWindowTop = mFrame.top;
            if (mBitmap == null || mBitmap.getWidth() != width || mBitmap.getHeight() != height) {
                mBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                // A window's canvas has the display's density, so that bitmaps of another
                // density drawn onto it are scaled (as ViewRootImpl sets up its canvas).
                mBitmap.setDensity(mView.getResources().getDisplayMetrics().densityDpi);
                mCanvas = new Canvas(mBitmap);
            }
            mView.layout(0, 0, width, height);
            mDrawRequested = true;
        }
        if (mDrawRequested) {
            mDrawRequested = false;
            mView.getViewTreeObserver().dispatchOnPreDraw();
            mCanvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
            long drawStart = System.nanoTime();
            mView.draw(mCanvas);
            a2i.boot.FrameStats.add(a2i.boot.FrameStats.DRAW, System.nanoTime() - drawStart);
            mView.getViewTreeObserver().dispatchOnDraw();
            mManager.windowDrawn();
        }
    }

    // --- ViewParent: what the hierarchy asks of its root ------------------------------

    @Override
    public void requestLayout() {
        mLayoutRequested = true;
        scheduleTraversal();
    }

    @Override
    public boolean isLayoutRequested() {
        return mLayoutRequested;
    }

    @Override
    public void invalidateChild(View child, Rect dirty) {
        mDrawRequested = true;
        scheduleTraversal();
    }

    @Override
    public ViewParent invalidateChildInParent(int[] location, Rect dirty) {
        invalidateChild(null, dirty);
        return null;
    }

    @Override
    public void onDescendantInvalidated(View child, View descendant) {
        invalidateChild(child, null);
    }

    @Override
    public void childDrawableStateChanged(View child) {}

    @Override
    public void requestTransparentRegion(View child) {}

    @Override
    public ViewParent getParent() {
        return null;
    }

    @Override
    public void requestChildFocus(View child, View focused) {
        invalidateChild(child, null);
    }

    @Override
    public void clearChildFocus(View child) {
        invalidateChild(child, null);
    }

    @Override
    public void recomputeViewAttributes(View child) {}

    @Override
    public boolean getChildVisibleRect(View child, Rect r, Point offset) {
        return r.intersect(0, 0, mFrame.width(), mFrame.height());
    }

    @Override
    public View focusSearch(View v, int direction) {
        return null;
    }

    @Override
    public View keyboardNavigationClusterSearch(View currentCluster, int direction) {
        return null;
    }

    @Override
    public void bringChildToFront(View child) {}

    @Override
    public void focusableViewAvailable(View v) {}

    @Override
    public boolean showContextMenuForChild(View originalView) {
        return false;
    }

    @Override
    public boolean showContextMenuForChild(View originalView, float x, float y) {
        return false;
    }

    @Override
    public void createContextMenu(ContextMenu menu) {}

    @Override
    public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback) {
        return null;
    }

    @Override
    public ActionMode startActionModeForChild(View originalView, ActionMode.Callback callback, int type) {
        return null;
    }

    @Override
    public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {}

    @Override
    public boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate) {
        return false;
    }

    @Override
    public boolean requestSendAccessibilityEvent(View child, AccessibilityEvent event) {
        return false;
    }

    @Override
    public void childHasTransientStateChanged(View child, boolean hasTransientState) {}

    @Override
    public void requestFitSystemWindows() {
        mApplyInsetsRequested = true;
        scheduleTraversal();
    }

    @Override
    public ViewParent getParentForAccessibility() {
        return null;
    }

    @Override
    public void notifySubtreeAccessibilityStateChanged(View child, View source, int changeType) {}

    @Override
    public boolean canResolveLayoutDirection() {
        return true;
    }

    @Override
    public boolean isLayoutDirectionResolved() {
        return true;
    }

    @Override
    public int getLayoutDirection() {
        return View.LAYOUT_DIRECTION_RESOLVED_DEFAULT;
    }

    @Override
    public boolean canResolveTextDirection() {
        return true;
    }

    @Override
    public boolean isTextDirectionResolved() {
        return true;
    }

    @Override
    public int getTextDirection() {
        return View.TEXT_DIRECTION_RESOLVED_DEFAULT;
    }

    @Override
    public boolean canResolveTextAlignment() {
        return true;
    }

    @Override
    public boolean isTextAlignmentResolved() {
        return true;
    }

    @Override
    public int getTextAlignment() {
        return View.TEXT_ALIGNMENT_RESOLVED_DEFAULT;
    }

    @Override
    public boolean onStartNestedScroll(View child, View target, int nestedScrollAxes) {
        return false;
    }

    @Override
    public void onNestedScrollAccepted(View child, View target, int nestedScrollAxes) {}

    @Override
    public void onStopNestedScroll(View target) {}

    @Override
    public void onNestedScroll(View target, int dxConsumed, int dyConsumed, int dxUnconsumed, int dyUnconsumed) {}

    @Override
    public void onNestedPreScroll(View target, int dx, int dy, int[] consumed) {}

    @Override
    public boolean onNestedFling(View target, float velocityX, float velocityY, boolean consumed) {
        return false;
    }

    @Override
    public boolean onNestedPreFling(View target, float velocityX, float velocityY) {
        return false;
    }

    @Override
    public boolean onNestedPrePerformAccessibilityAction(View target, int action, Bundle arguments) {
        return false;
    }
}
