package a2i.boot;

import android.app.ActivityThread;
import android.app.INotificationManager;
import android.app.ITransientNotification;
import android.app.ITransientNotificationCallback;
import android.content.Context;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Log;
import android.view.ContextThemeWrapper;
import android.view.View;
import android.view.accessibility.IAccessibilityManager;
import android.widget.Toast;
import android.widget.ToastPresenter;

import java.util.ArrayDeque;
import java.util.Iterator;

/**
 * android.app.INotificationManager. So far this is the part that shows toasts.
 *
 * An app does not put a toast on screen itself: it hands it to the notification manager,
 * which shows one toast at a time for the requested duration. A plain text toast is
 * drawn by the system UI on the app's behalf; one with a custom view is drawn by the app
 * when the service tells it to. Both cases end up as a window added through the window
 * manager, using the framework's own ToastPresenter.
 */
public final class NotificationService {
    private static final String TAG = "a2i.Notifications";
    private static final long SHORT_DURATION_MS = 2000;
    private static final long LONG_DURATION_MS = 3500;

    private static final class ToastRecord {
        IBinder token;
        String packageName;
        int duration;
        // A text toast:
        CharSequence text;
        ITransientNotificationCallback textCallback;
        ToastPresenter presenter;
        // A custom toast, shown by the app:
        ITransientNotification custom;
    }

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final ArrayDeque<ToastRecord> mQueue = new ArrayDeque<>();
    private ToastRecord mShowing;
    private final Runnable mTimeout = () -> {
        hide(mShowing);
        showNext();
    };

    public boolean enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration,
            boolean isUiContext, int displayId, ITransientNotificationCallback callback) {
        ToastRecord record = new ToastRecord();
        record.packageName = pkg;
        record.token = token;
        record.text = text;
        record.duration = duration;
        record.textCallback = callback;
        enqueue(record);
        return true;
    }

    public boolean enqueueToast(String pkg, IBinder token, ITransientNotification callback, int duration,
            boolean isUiContext, int displayId) {
        ToastRecord record = new ToastRecord();
        record.packageName = pkg;
        record.token = token;
        record.custom = callback;
        record.duration = duration;
        enqueue(record);
        return true;
    }

    public void cancelToast(String pkg, IBinder token) {
        mHandler.post(() -> {
            for (Iterator<ToastRecord> i = mQueue.iterator(); i.hasNext();) {
                if (i.next().token == token) {
                    i.remove();
                }
            }
            if (mShowing != null && mShowing.token == token) {
                mHandler.removeCallbacks(mTimeout);
                hide(mShowing);
                showNext();
            }
        });
    }

    private void enqueue(ToastRecord record) {
        mHandler.post(() -> {
            mQueue.add(record);
            if (mShowing == null) {
                showNext();
            }
        });
    }

    private void showNext() {
        mShowing = mQueue.poll();
        ToastRecord record = mShowing;
        if (record == null) {
            return;
        }
        IBinder windowToken = new Binder();
        try {
            if (record.custom != null) {
                record.custom.show(windowToken);
            } else {
                // What the system UI does for a text toast, with the system's theme.
                Context context = new ContextThemeWrapper(ActivityThread.currentApplication(),
                        com.android.internal.R.style.Theme_DeviceDefault_DayNight);
                record.presenter = new ToastPresenter(context,
                        IAccessibilityManager.Stub.asInterface(ServiceManager.getService("accessibility")),
                        INotificationManager.Stub.asInterface(ServiceManager.getService("notification")),
                        record.packageName);
                View view = ToastPresenter.getTextToastView(context, record.text);
                record.presenter.show(view, record.token, windowToken, record.duration,
                        context.getResources().getInteger(com.android.internal.R.integer.config_toastDefaultGravity),
                        0 /* xOffset */,
                        context.getResources().getDimensionPixelSize(com.android.internal.R.dimen.toast_y_offset),
                        0f /* horizontalMargin */, 0f /* verticalMargin */, record.textCallback);
            }
        } catch (RemoteException | RuntimeException e) {
            Log.e(TAG, "cannot show toast", e);
            mShowing = null;
            showNext();
            return;
        }
        mHandler.postDelayed(mTimeout, record.duration == Toast.LENGTH_LONG ? LONG_DURATION_MS : SHORT_DURATION_MS);
    }

    private void hide(ToastRecord record) {
        if (record == null) {
            return;
        }
        try {
            if (record.custom != null) {
                record.custom.hide();
            } else if (record.presenter != null) {
                record.presenter.hide(record.textCallback);
            }
        } catch (RemoteException | RuntimeException e) {
            Log.e(TAG, "cannot hide toast", e);
        }
    }
}
