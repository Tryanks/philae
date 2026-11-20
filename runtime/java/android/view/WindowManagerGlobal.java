package android.view;

import android.content.res.Configuration;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;

import java.io.FileDescriptor;
import java.util.ArrayList;

/**
 * Replacement for the framework's android.view.WindowManagerGlobal.
 *
 * This is the process-wide object behind every WindowManager: adding, updating and
 * removing a window all arrive here. The framework's version creates a ViewRootImpl for
 * each window and connects it to the window manager service; this version hands the
 * window to {@link A2IWindowManager}, which draws and composes windows in this process.
 * (It takes the place of the framework class because it comes first on the boot class
 * path.) The rest of the class's API is kept so that callers link; operations that only
 * make sense with a window manager service do nothing.
 */
public final class WindowManagerGlobal {
    public static final int ADD_OKAY = 0;

    private static WindowManagerGlobal sDefaultWindowManager;
    private static IWindowManager sWindowManagerService;
    private static IWindowSession sWindowSession;

    private WindowManagerGlobal() {}

    public static void initialize() {
        getWindowManagerService();
    }

    public static synchronized WindowManagerGlobal getInstance() {
        if (sDefaultWindowManager == null) {
            sDefaultWindowManager = new WindowManagerGlobal();
        }
        return sDefaultWindowManager;
    }

    public static synchronized IWindowManager getWindowManagerService() {
        if (sWindowManagerService == null) {
            sWindowManagerService = IWindowManager.Stub.asInterface(ServiceManager.getService("window"));
        }
        return sWindowManagerService;
    }

    public static synchronized IWindowSession getWindowSession() {
        if (sWindowSession == null) {
            try {
                sWindowSession = getWindowManagerService().openSession(null);
            } catch (RemoteException e) {
                throw e.rethrowFromSystemServer();
            }
        }
        return sWindowSession;
    }

    public static synchronized IWindowSession peekWindowSession() {
        return sWindowSession;
    }

    public void addView(View view, ViewGroup.LayoutParams params, Display display, Window parentWindow,
            int userId) {
        if (view == null) {
            throw new IllegalArgumentException("view must not be null");
        }
        if (!(params instanceof WindowManager.LayoutParams)) {
            throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        }
        WindowManager.LayoutParams windowParams = (WindowManager.LayoutParams) params;
        if (parentWindow != null) {
            parentWindow.adjustLayoutParamsForSubWindow(windowParams);
        }
        view.setLayoutParams(windowParams);
        A2IWindowManager.getInstance().addWindow(view, windowParams);
    }

    public void updateViewLayout(View view, ViewGroup.LayoutParams params) {
        if (view == null) {
            throw new IllegalArgumentException("view must not be null");
        }
        if (!(params instanceof WindowManager.LayoutParams)) {
            throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        }
        view.setLayoutParams(params);
        A2IWindowManager.getInstance().updateWindow(view, (WindowManager.LayoutParams) params);
    }

    public void removeView(View view, boolean immediate) {
        if (view == null) {
            throw new IllegalArgumentException("view must not be null");
        }
        A2IWindowManager.getInstance().removeWindow(view);
    }

    public ArrayList<View> getWindowViews() {
        return A2IWindowManager.getInstance().windowViews();
    }

    // --- Parts of the API that have no work to do here --------------------------------

    public String[] getViewRootNames() {
        return new String[0];
    }

    public ArrayList<ViewRootImpl> getRootViews(IBinder token) {
        return new ArrayList<>();
    }

    public View getWindowView(IBinder windowToken) {
        return null;
    }

    public View getRootView(String name) {
        return null;
    }

    public void closeAll(IBinder token, String who, String what) {}

    public void closeAllExceptView(IBinder token, View view, String who, String what) {}

    void doRemoveView(ViewRootImpl root) {}

    public void trimMemory(int level) {}

    public void trimCaches(int level) {}

    public void dumpGfxInfo(FileDescriptor fd, String[] args) {}

    public void setStoppedState(IBinder token, boolean stopped) {}

    public void reportNewConfiguration(Configuration config) {}

    public void changeCanvasOpacity(IBinder token, boolean opaque) {}

    public void addWindowlessRoot(ViewRootImpl impl) {}

    public void removeWindowlessRoot(ViewRootImpl impl) {}

    public void setRecentsAppBehindSystemBars(boolean behindSystemBars) {}
}
