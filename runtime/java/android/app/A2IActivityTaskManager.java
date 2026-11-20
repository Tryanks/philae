package android.app;

import android.app.servertransaction.ActivityLifecycleItem;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ClientTransactionItem;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.NewIntentItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StartActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageParser;
import android.content.res.Configuration;
import android.content.res.TypedArray;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.os.ShellCallback;
import android.util.Log;
import android.window.ActivityWindowInfo;

import com.android.internal.content.ReferrerIntent;

import java.io.FileDescriptor;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The activity task manager of this single-app system: it owns the stack of activities
 * and decides which one is created, resumed, stopped or destroyed, and when.
 *
 * On a device this is ActivityTaskManagerService in system_server. It drives the app by
 * sending it transactions (launch this activity, pause that one), which the app's
 * ActivityThread executes. This class sends the same transactions to the same
 * ActivityThread, from inside the process, so everything on the app's side of that
 * boundary is the framework's own code: instantiating the activity, the lifecycle
 * callbacks and their ordering, saved state, Activity.recreate(), results.
 *
 * What it keeps of the server side is one task: a stack with the launcher activity at the
 * bottom. Requests arrive from the app through the two AIDL interfaces at the bottom of
 * this file. They are answered immediately and carried out afterwards from the main
 * thread's message queue, as on a device, because the app typically makes them in the
 * middle of a lifecycle callback.
 *
 * It lives in android.app because much of what it uses is package-private there.
 */
public final class A2IActivityTaskManager {
    private static final String TAG = "a2i.Activities";

    /** Starts activities that are not part of the app: opening a link, sharing text. */
    public interface ExternalStarter {
        /** Whether {@link #startActivity} would do something with the intent. */
        boolean canStartActivity(Intent intent);

        /**
         * Called on Android's main thread, for an intent that {@link #canStartActivity}
         * accepted. Returns the activity's result if it is known at once, and otherwise
         * null (nothing is reported back to an app that asked for a result).
         */
        Instrumentation.ActivityResult startActivity(Intent intent);
    }

    /**
     * Identifies an activity to the app, as the handle to the server's ActivityRecord does
     * on a device. It is deliberately not a local Binder object: the framework takes an
     * activity whose token is one to belong to the system server's own process and, for
     * instance, refuses to recreate it.
     */
    private static final class Token implements IBinder {
        @Override
        public String getInterfaceDescriptor() {
            return "android.app.IApplicationToken";
        }

        @Override
        public boolean pingBinder() {
            return true;
        }

        @Override
        public boolean isBinderAlive() {
            return true;
        }

        @Override
        public IInterface queryLocalInterface(String descriptor) {
            return null;
        }

        @Override
        public void dump(FileDescriptor fd, String[] args) {}

        @Override
        public void dumpAsync(FileDescriptor fd, String[] args) {}

        @Override
        public void shellCommand(FileDescriptor in, FileDescriptor out, FileDescriptor err, String[] args,
                ShellCallback shellCallback, ResultReceiver resultReceiver) {}

        @Override
        public boolean transact(int code, Parcel data, Parcel reply, int flags) {
            return false;
        }

        @Override
        public void linkToDeath(DeathRecipient recipient, int flags) {}

        @Override
        public boolean unlinkToDeath(DeathRecipient recipient, int flags) {
            return true;
        }
    }

    private static final class Record {
        final IBinder token = new Token();
        final ActivityInfo info;
        final Intent intent;
        /** The activity that started this one with startActivityForResult, if any. */
        Record resultTo;
        String resultWho;
        int requestCode = -1;
        int resultCode = Activity.RESULT_CANCELED;
        Intent resultData;
        boolean finishing;

        Record(ActivityInfo info, Intent intent) {
            this.info = info;
            this.intent = intent;
        }
    }

    private static final A2IActivityTaskManager sInstance = new A2IActivityTaskManager();

    /** Bottom to top. Only touched on the main thread. */
    private final ArrayList<Record> mStack = new ArrayList<>();
    private final IActivityClientController mController = (IActivityClientController) a2i.boot.Services
            .createInterface("activity_client", IActivityClientController.class, new ClientController(),
                    new Binder());
    private A2IActivityLauncher.LoadedApp mApp;
    private Handler mHandler;
    private ExternalStarter mExternalStarter;
    private Rect mScreen = new Rect();
    private boolean mForeground = true;

    private A2IActivityTaskManager() {}

    public static A2IActivityTaskManager getInstance() {
        return sInstance;
    }

    /** The object implementing android.app.IActivityTaskManager, for the service registry. */
    public Object service() {
        return new TaskManagerService();
    }

    public void setExternalStarter(ExternalStarter starter) {
        mExternalStarter = starter;
    }

    /**
     * Starts the app's launcher activity. Must be called on the main thread, once, after
     * the application has been loaded. Returns when the activity is resumed.
     */
    public void startLauncher(A2IActivityLauncher.LoadedApp app, int screenWidth, int screenHeight) {
        mApp = app;
        mHandler = new Handler(Looper.getMainLooper());
        mScreen = new Rect(0, 0, screenWidth, screenHeight);
        startLauncher();
    }

    private void startLauncher() {
        ActivityInfo info = null;
        for (PackageParser.Activity activity : mApp.pkg.activities) {
            for (PackageParser.ActivityIntentInfo filter : activity.intents) {
                if (info == null && filter.hasAction(Intent.ACTION_MAIN)
                        && filter.hasCategory(Intent.CATEGORY_LAUNCHER)) {
                    info = activity.info;
                }
            }
        }
        if (info == null) {
            throw new IllegalStateException("the APK declares no launcher activity");
        }
        Intent intent = new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .setComponent(new ComponentName(info.packageName, info.name));
        push(new Record(info, intent));
    }

    /**
     * Tells the app whether it is on screen. Leaving the screen stops its activities (the
     * top one is paused first), which is when apps save their state; coming back restarts
     * and resumes them. Must be called on the main thread.
     */
    public void setForeground(boolean foreground) {
        if (mApp == null || foreground == mForeground) {
            return;
        }
        mForeground = foreground;
        Record top = top();
        if (!foreground) {
            for (int i = mStack.size() - 1; i >= 0; i--) {
                Record record = mStack.get(i);
                ActivityThread.ActivityClientRecord client = mApp.thread.getActivityClient(record.token);
                if (client == null || client.getLifecycleState() >= ActivityLifecycleItem.ON_STOP) {
                    continue;
                }
                if (isResumed(record)) {
                    execute(PauseActivityItem.obtain(record.token, false /* finished */, true /* userLeaving */,
                            false /* dontReport */, false /* autoEnteringPip */));
                }
                execute(StopActivityItem.obtain(record.token));
            }
        } else if (top != null) {
            // Activities showing through a dialog-like one on top become visible again too.
            for (int i = mStack.size() - 1; i > 0 && !coversScreen(mStack.get(i)); i--) {
                execute(StartActivityItem.obtain(mStack.get(i - 1).token, null /* sceneTransitionInfo */));
            }
            execute(ResumeActivityItem.obtain(top.token, false /* isForward */,
                    false /* shouldSendCompatFakeFocus */));
        }
    }

    // --- Requests from the app ----------------------------------------------------------

    private int requestStart(Intent intent, String resolvedType, IBinder resultTo, String resultWho,
            int requestCode) {
        boolean chooser = Intent.ACTION_CHOOSER.equals(intent.getAction())
                && intent.getParcelableExtra(Intent.EXTRA_INTENT) instanceof Intent;
        if (chooser) {
            // The app asks the user to pick a target for the wrapped intent. That is a
            // request to hand it to someone else, even if the app itself could take it
            // (the chooser never lists the app that is asking), and outside the app there
            // is exactly one taker.
            intent = (Intent) intent.getParcelableExtra(Intent.EXTRA_INTENT);
            resolvedType = intent.getType();
        }
        final ActivityInfo info = chooser && startsExternally(intent) ? null : resolve(intent, resolvedType);
        if (info == null) {
            if (startsExternally(intent)) {
                final Instrumentation.ActivityResult result = mExternalStarter.startActivity(new Intent(intent));
                if (result != null && requestCode >= 0 && resultTo != null) {
                    final IBinder caller = resultTo;
                    final String who = resultWho;
                    final int code = requestCode;
                    // Delivered after this call returns, as a result from another activity is.
                    mHandler.post(() -> {
                        if (find(caller) != null) {
                            execute(ActivityResultItem.obtain(caller, Collections.singletonList(
                                    new ResultInfo(who, code, result.getResultCode(), result.getResultData()))));
                        }
                    });
                }
                return ActivityManager.START_SUCCESS;
            }
            Log.w(TAG, "no activity for " + intent);
            return intent.getComponent() != null
                    ? ActivityManager.START_CLASS_NOT_FOUND
                    : ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        final Record record = new Record(info, new Intent(intent));
        final IBinder caller = resultTo;
        if (requestCode >= 0) {
            record.resultWho = resultWho;
            record.requestCode = requestCode;
        }
        mHandler.post(() -> {
            if (record.requestCode >= 0) {
                record.resultTo = find(caller);
            }
            start(record);
        });
        return ActivityManager.START_SUCCESS;
    }

    private boolean requestFinish(IBinder token, int resultCode, Intent resultData) {
        mHandler.post(() -> {
            Record record = find(token);
            if (record != null) {
                record.resultCode = resultCode;
                record.resultData = resultData;
                finish(record);
            }
        });
        return true;
    }

    /** Whether the intent is one for something outside the app that the host can provide. */
    public boolean startsExternally(Intent intent) {
        return intent.getComponent() == null && mExternalStarter != null
                && mExternalStarter.canStartActivity(intent);
    }

    /** The activity in this app that should handle the intent, or null if none does. */
    public ActivityInfo resolve(Intent intent, String resolvedType) {
        String packageName = mApp.pkg.packageName;
        ComponentName component = intent.getComponent();
        if (component == null && intent.getSelector() != null) {
            intent = intent.getSelector();
            component = intent.getComponent();
        }
        if (component != null) {
            for (PackageParser.Activity activity : mApp.pkg.activities) {
                if (component.getPackageName().equals(packageName)
                        && component.getClassName().equals(activity.info.name)) {
                    return activity.info;
                }
            }
            return null;
        }
        if (intent.getPackage() != null && !intent.getPackage().equals(packageName)) {
            return null;
        }
        // A link goes to the browser even if the app could also open it itself, unless the
        // app addressed the intent to its own package.
        String scheme = intent.getScheme();
        if (intent.getPackage() == null && ("http".equals(scheme) || "https".equals(scheme))) {
            return null;
        }
        Uri data = intent.getData();
        for (PackageParser.Activity activity : mApp.pkg.activities) {
            for (PackageParser.ActivityIntentInfo filter : activity.intents) {
                if (filter.match(intent.getAction(), resolvedType, scheme, data, intent.getCategories(),
                        TAG) >= 0) {
                    return activity.info;
                }
            }
        }
        return null;
    }

    // --- The stack ------------------------------------------------------------------------

    private Record top() {
        return mStack.isEmpty() ? null : mStack.get(mStack.size() - 1);
    }

    private Record find(IBinder token) {
        for (Record record : mStack) {
            if (record.token == token) {
                return record;
            }
        }
        return null;
    }

    private static boolean sameActivity(Record record, ActivityInfo info) {
        return record.info.name.equals(info.name);
    }

    /** Carries out a start request, honouring the launch modes that reuse an existing activity. */
    private void start(Record record) {
        ActivityInfo info = record.info;
        int flags = record.intent.getFlags();
        boolean reuseTop = (flags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0
                || info.launchMode != ActivityInfo.LAUNCH_MULTIPLE;
        boolean clearTop = (flags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_TASK
                || info.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE;

        Record top = top();
        if (top != null && reuseTop && sameActivity(top, info)) {
            deliverNewIntent(top, record.intent);
            return;
        }
        if (clearTop) {
            int index = -1;
            for (int i = mStack.size() - 1; i >= 0 && index < 0; i--) {
                if (sameActivity(mStack.get(i), info)) {
                    index = i;
                }
            }
            if (index >= 0) {
                // Everything above the existing instance goes away, and it either receives
                // the intent or, for a plain activity, is replaced by a new instance.
                while (mStack.size() > index + 1) {
                    destroy(mStack.remove(mStack.size() - 1));
                }
                Record existing = mStack.get(index);
                if (reuseTop) {
                    deliverNewIntent(existing, record.intent);
                    return;
                }
                mStack.remove(index);
                destroy(existing);
            }
        }
        push(record);
    }

    /** Puts a new activity on top of the stack: pauses the current one, launches, resumes. */
    private void push(Record record) {
        Record previous = top();
        if (previous != null && isResumed(previous)) {
            execute(PauseActivityItem.obtain(previous.token, false /* finished */, true /* userLeaving */,
                    false /* dontReport */, false /* autoEnteringPip */));
        }
        mStack.add(record);

        Configuration config = new Configuration(mApp.thread.getConfiguration());
        ActivityWindowInfo windowInfo = new ActivityWindowInfo();
        windowInfo.set(false /* isEmbedded */, mScreen, mScreen);
        String referrer = previous != null ? mApp.pkg.packageName : null;
        execute(LaunchActivityItem.obtain(record.token, record.intent, System.identityHashCode(record),
                        record.info, config, Configuration.EMPTY /* overrideConfig */, 0 /* deviceId */, referrer,
                        null /* voiceInteractor */, ActivityManager.PROCESS_STATE_TOP, null /* state */,
                        null /* persistentState */, null /* pendingResults */, null /* pendingNewIntents */,
                        null /* sceneTransitionInfo */, true /* isForward */, null /* profilerInfo */,
                        new Binder() /* assistToken */, mController, new Binder() /* shareableActivityToken */,
                        false /* launchedFromBubble */, null /* taskFragmentToken */,
                        new Binder() /* initialCallerInfoAccessToken */, windowInfo),
                ResumeActivityItem.obtain(record.token, true /* isForward */,
                        false /* shouldSendCompatFakeFocus */));

        // An activity that fills the screen hides the one below, which is then stopped. A
        // dialog-like or translucent one leaves it visible, merely paused.
        if (previous != null && find(previous.token) != null && coversScreen(record)) {
            execute(StopActivityItem.obtain(previous.token));
        }
    }

    /** Carries out a finish request. */
    private void finish(Record record) {
        if (record.finishing || !mStack.contains(record)) {
            return;
        }
        record.finishing = true;
        boolean wasTop = top() == record;
        if (isResumed(record)) {
            execute(PauseActivityItem.obtain(record.token, true /* finished */, false /* userLeaving */,
                    false /* dontReport */, false /* autoEnteringPip */));
        }
        mStack.remove(record);

        // The activity underneath comes back, receiving the result if it asked for one.
        Record next = top();
        Record resultTo = record.resultTo != null && mStack.contains(record.resultTo) ? record.resultTo : null;
        ClientTransactionItem result = resultTo == null ? null : ActivityResultItem.obtain(resultTo.token,
                Collections.singletonList(new ResultInfo(record.resultWho, record.requestCode,
                        record.resultCode, record.resultData)));
        if (wasTop && next != null) {
            if (result != null && resultTo == next) {
                execute(result, ResumeActivityItem.obtain(next.token, false /* isForward */,
                        false /* shouldSendCompatFakeFocus */));
            } else {
                if (result != null) {
                    execute(result);
                }
                execute(ResumeActivityItem.obtain(next.token, false, false));
            }
        } else if (result != null) {
            execute(result);
        }
        destroy(record);

        if (mStack.isEmpty()) {
            // The app closed its last activity. On Android the user is now back at the home
            // screen with the process still alive, and opening the app again starts a new
            // launcher activity. An iOS app cannot send itself to the home screen, so the
            // second half happens right away.
            Log.i(TAG, "the last activity finished; starting the launcher activity again");
            startLauncher();
        }
    }

    private void destroy(Record record) {
        if (mApp.thread.getActivityClient(record.token) != null) {
            execute(DestroyActivityItem.obtain(record.token, true /* finished */));
        }
    }

    private void deliverNewIntent(Record record, Intent intent) {
        List<ReferrerIntent> intents = new ArrayList<>();
        intents.add(new ReferrerIntent(intent, mApp.pkg.packageName));
        boolean resume = record == top();
        execute(NewIntentItem.obtain(record.token, intents, resume));
        if (resume && !isResumed(record)) {
            execute(ResumeActivityItem.obtain(record.token, false, false));
        }
    }

    private boolean isResumed(Record record) {
        ActivityThread.ActivityClientRecord client = mApp.thread.getActivityClient(record.token);
        return client != null && client.activity != null && !client.paused;
    }

    private boolean coversScreen(Record record) {
        Activity activity = mApp.thread.getActivity(record.token);
        if (activity == null || activity.getWindow() == null) {
            return true;
        }
        TypedArray style = activity.getWindow().getWindowStyle();
        return !style.getBoolean(com.android.internal.R.styleable.Window_windowIsFloating, false)
                && !style.getBoolean(com.android.internal.R.styleable.Window_windowIsTranslucent, false);
    }

    /** Has the app's ActivityThread execute a transaction, here and now. */
    private void execute(ClientTransactionItem... items) {
        ClientTransaction transaction = ClientTransaction.obtain(mApp.thread.getApplicationThread());
        for (ClientTransactionItem item : items) {
            transaction.addTransactionItem(item);
        }
        mApp.thread.executeTransaction(transaction);
    }

    // --- The AIDL interfaces the app talks to -------------------------------------------

    /** android.app.IActivityTaskManager: requests that are not about one existing activity. */
    public final class TaskManagerService {
        public int startActivity(IApplicationThread caller, String callingPackage, String callingFeatureId,
                Intent intent, String resolvedType, IBinder resultTo, String resultWho, int requestCode,
                int flags, ProfilerInfo profilerInfo, Bundle options) {
            return requestStart(intent, resolvedType, resultTo, resultWho, requestCode);
        }

        public int startActivityAsUser(IApplicationThread caller, String callingPackage,
                String callingFeatureId, Intent intent, String resolvedType, IBinder resultTo,
                String resultWho, int requestCode, int flags, ProfilerInfo profilerInfo, Bundle options,
                int userId) {
            return requestStart(intent, resolvedType, resultTo, resultWho, requestCode);
        }

        public int startActivities(IApplicationThread caller, String callingPackage, String callingFeatureId,
                Intent[] intents, String[] resolvedTypes, IBinder resultTo, Bundle options, int userId) {
            int result = ActivityManager.START_SUCCESS;
            for (int i = 0; i < intents.length && result == ActivityManager.START_SUCCESS; i++) {
                result = requestStart(intents[i], resolvedTypes != null ? resolvedTypes[i] : null, null, null, -1);
            }
            return result;
        }

        public IActivityClientController getActivityClientController() {
            return mController;
        }
    }

    /** android.app.IActivityClientController: what an activity tells or asks about itself. */
    public final class ClientController {
        public boolean finishActivity(IBinder token, int resultCode, Intent resultData, int finishTask) {
            return requestFinish(token, resultCode, resultData);
        }

        public boolean finishActivityAffinity(IBinder token) {
            mHandler.post(() -> {
                int index = mStack.indexOf(find(token));
                for (int i = index; i >= 0; i--) {
                    finish(mStack.get(i));
                }
            });
            return true;
        }

        public void finishSubActivity(IBinder token, String resultWho, int requestCode) {
            mHandler.post(() -> {
                Record parent = find(token);
                for (Record record : new ArrayList<>(mStack)) {
                    if (parent != null && record.resultTo == parent && record.requestCode == requestCode
                            && java.util.Objects.equals(record.resultWho, resultWho)) {
                        finish(record);
                    }
                }
            });
        }

        /**
         * The back gesture reached an activity that does not handle it itself. Activities
         * above the first are finished. The first one stays: on Android its task would go
         * to the background, which an iOS app cannot do to itself.
         */
        public void onBackPressed(IBinder token, IRequestFinishCallback callback) throws RemoteException {
            Record record = find(token);
            if (record != null && mStack.indexOf(record) > 0) {
                callback.requestFinish();
            }
        }

        public boolean moveActivityTaskToBack(IBinder token, boolean nonRoot) {
            return false;
        }

        public boolean willActivityBeVisible(IBinder token) {
            return find(token) == top();
        }

        public boolean isTopOfTask(IBinder token) {
            Record record = find(token);
            return record != null && record == top();
        }

        public int getTaskForActivity(IBinder token, boolean onlyRoot) {
            Record record = find(token);
            if (record == null || (onlyRoot && mStack.indexOf(record) != 0)) {
                return -1;
            }
            return 1;
        }

        public ComponentName getCallingActivity(IBinder token) {
            Record record = find(token);
            return record != null && record.resultTo != null
                    ? new ComponentName(record.resultTo.info.packageName, record.resultTo.info.name) : null;
        }

        public String getCallingPackage(IBinder token) {
            Record record = find(token);
            return record != null && record.resultTo != null ? record.resultTo.info.packageName : null;
        }

        public String getLaunchedFromPackage(IBinder token) {
            return mApp.pkg.packageName;
        }

        public int getLaunchedFromUid(IBinder token) {
            return android.os.Process.myUid();
        }

        public int getRequestedOrientation(IBinder token) {
            return ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        }

        public Configuration getTaskConfiguration(IBinder token) {
            return mApp.thread.getConfiguration();
        }
    }
}
