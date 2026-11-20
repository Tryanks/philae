package android.app;

import android.content.ComponentName;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageParser;
import android.content.pm.ParceledListSlice;
import android.content.pm.ServiceInfo;
import android.content.res.CompatibilityInfo;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

/**
 * Services and broadcasts for this single-app system: the part of the activity manager
 * that decides which of the app's services exist, who is bound to them, and which
 * receivers get a broadcast.
 *
 * As with activities ({@link A2IActivityTaskManager}), the app's side is the framework's
 * own: this class tells the app's ActivityThread to create, start, bind and destroy
 * services and to run manifest receivers, through the same calls the system server makes,
 * and the framework does the rest (instantiating the component, the lifecycle callbacks,
 * delivering the binder to the connection). There are no other apps, so a service is
 * only ever used by its own app, and a broadcast only reaches the app that sent it.
 *
 * Requests arrive on any thread and are carried out on the main thread, in order.
 */
public final class A2IComponentManager {
    private static final String TAG = "a2i.Components";
    private static final A2IComponentManager sInstance = new A2IComponentManager();

    private static final class Binding {
        final Intent intent;
        final List<IServiceConnection> connections = new ArrayList<>();
        IBinder published;
        boolean requested;

        Binding(Intent intent) {
            this.intent = intent;
        }
    }

    private static final class Service {
        final IBinder token = new Binder();
        final ServiceInfo info;
        final ComponentName name;
        final Map<Intent.FilterComparison, Binding> bindings = new HashMap<>();
        boolean created;
        boolean started;
        int lastStartId;

        Service(ServiceInfo info) {
            this.info = info;
            this.name = new ComponentName(info.packageName, info.name);
        }
    }

    private static final class Receiver {
        final IIntentReceiver receiver;
        final IntentFilter filter;

        Receiver(IIntentReceiver receiver, IntentFilter filter) {
            this.receiver = receiver;
            this.filter = filter;
        }
    }

    private final Map<ComponentName, Service> mServices = new HashMap<>();
    private final List<Receiver> mReceivers = new ArrayList<>();
    private final Map<String, Intent> mSticky = new HashMap<>();
    private A2IActivityLauncher.LoadedApp mApp;
    private Handler mHandler;

    private A2IComponentManager() {
        // What a device always has on offer for anyone who asks: the battery's state.
        // (Reported as full and charging; iOS's battery level is not bridged.)
        Intent battery = new Intent(Intent.ACTION_BATTERY_CHANGED);
        battery.putExtra(BatteryManager.EXTRA_LEVEL, 100);
        battery.putExtra(BatteryManager.EXTRA_SCALE, 100);
        battery.putExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_FULL);
        battery.putExtra(BatteryManager.EXTRA_PLUGGED, BatteryManager.BATTERY_PLUGGED_AC);
        battery.putExtra(BatteryManager.EXTRA_PRESENT, true);
        mSticky.put(Intent.ACTION_BATTERY_CHANGED, battery);
    }

    public static A2IComponentManager getInstance() {
        return sInstance;
    }

    /** Sets the app whose components these are. Call on the main thread once it is loaded. */
    public void attach(A2IActivityLauncher.LoadedApp app) {
        mApp = app;
        mHandler = new Handler(Looper.getMainLooper());
    }

    private IApplicationThread appThread() {
        return mApp.thread.getApplicationThread();
    }

    // --- Services -------------------------------------------------------------------------

    private synchronized Service resolveService(Intent intent, String resolvedType) {
        if (mApp == null || intent == null) {
            return null;
        }
        ComponentName component = intent.getComponent();
        if (component != null && mServices.containsKey(component)) {
            return mServices.get(component);
        }
        for (PackageParser.Service candidate : mApp.pkg.services) {
            boolean matches;
            if (component != null) {
                matches = candidate.getComponentName().equals(component);
            } else {
                matches = false;
                for (PackageParser.ServiceIntentInfo filter : candidate.intents) {
                    matches |= filter.match(intent.getAction(), resolvedType, intent.getScheme(), intent.getData(),
                            intent.getCategories(), TAG) >= 0;
                }
            }
            if (matches) {
                candidate.info.applicationInfo = mApp.pkg.applicationInfo;
                ComponentName name = candidate.getComponentName();
                Service service = mServices.get(name);
                if (service == null) {
                    service = new Service(candidate.info);
                    mServices.put(name, service);
                }
                return service;
            }
        }
        return null;
    }

    private Service serviceForToken(IBinder token) {
        for (Service service : mServices.values()) {
            if (service.token == token) {
                return service;
            }
        }
        return null;
    }

    private void ensureCreated(Service service) throws RemoteException {
        if (!service.created) {
            service.created = true;
            appThread().scheduleCreateService(service.token, service.info, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                    ActivityManager.PROCESS_STATE_TOP);
        }
    }

    /** Destroys a service that is neither started nor bound any more. */
    private void destroyIfUnused(Service service) throws RemoteException {
        if (service.created && !service.started && service.bindings.isEmpty()) {
            service.created = false;
            mServices.remove(service.name);
            appThread().scheduleStopService(service.token);
        }
    }

    private interface Action {
        void run() throws RemoteException;
    }

    private void post(Action action) {
        mHandler.post(() -> {
            try {
                synchronized (A2IComponentManager.this) {
                    action.run();
                }
            } catch (RemoteException e) {
                throw e.rethrowAsRuntimeException();  // in-process: cannot happen
            }
        });
    }

    public ComponentName startService(Intent intent, String resolvedType) {
        final Service service = resolveService(intent, resolvedType);
        if (service == null) {
            Log.w(TAG, "no service for " + intent);
            return null;
        }
        final Intent copy = new Intent(intent);
        post(() -> {
            ensureCreated(service);
            service.started = true;
            ServiceStartArgs args = new ServiceStartArgs(false /* taskRemoved */, ++service.lastStartId,
                    0 /* flags */, copy);
            appThread().scheduleServiceArgs(service.token, new ParceledListSlice<>(Collections.singletonList(args)));
        });
        return service.name;
    }

    public int stopService(Intent intent, String resolvedType) {
        final Service service = resolveService(intent, resolvedType);
        if (service == null) {
            return 0;
        }
        post(() -> {
            service.started = false;
            destroyIfUnused(service);
        });
        return 1;
    }

    /** Service.stopSelf(): {@code startId} is the start request it answers, or -1 for any. */
    public boolean stopServiceToken(IBinder token, int startId) {
        post(() -> {
            Service service = serviceForToken(token);
            if (service != null && (startId < 0 || startId == service.lastStartId)) {
                service.started = false;
                destroyIfUnused(service);
            }
        });
        return true;
    }

    public int bindService(Intent intent, String resolvedType, IServiceConnection connection) {
        final Service service = resolveService(intent, resolvedType);
        if (service == null) {
            Log.w(TAG, "no service to bind for " + intent);
            return 0;
        }
        final Intent copy = new Intent(intent);
        post(() -> {
            ensureCreated(service);
            Intent.FilterComparison key = new Intent.FilterComparison(copy);
            Binding binding = service.bindings.get(key);
            if (binding == null) {
                binding = new Binding(copy);
                service.bindings.put(key, binding);
            }
            binding.connections.add(connection);
            if (binding.published != null) {
                connection.connected(service.name, binding.published, false /* dead */);
            } else if (!binding.requested) {
                binding.requested = true;
                appThread().scheduleBindService(service.token, copy, false /* rebind */,
                        ActivityManager.PROCESS_STATE_TOP, 0 /* bindSeq */);
            }
        });
        return 1;
    }

    /** The service answered a bind request with its binder. */
    public void publishService(IBinder token, Intent intent, IBinder binder) {
        post(() -> {
            Service service = serviceForToken(token);
            Binding binding = service != null ? service.bindings.get(new Intent.FilterComparison(intent)) : null;
            if (binding == null) {
                return;
            }
            binding.published = binder;
            for (IServiceConnection connection : binding.connections) {
                connection.connected(service.name, binder, false /* dead */);
            }
        });
    }

    public boolean unbindService(IServiceConnection connection) {
        post(() -> {
            for (Service service : new ArrayList<>(mServices.values())) {
                for (Iterator<Binding> i = service.bindings.values().iterator(); i.hasNext();) {
                    Binding binding = i.next();
                    if (binding.connections.remove(connection) && binding.connections.isEmpty()) {
                        i.remove();
                        appThread().scheduleUnbindService(service.token, binding.intent);
                    }
                }
                destroyIfUnused(service);
            }
        });
        return true;
    }

    // --- Broadcasts -----------------------------------------------------------------------

    public synchronized Intent registerReceiver(IIntentReceiver receiver, IntentFilter filter) {
        if (receiver != null) {
            mReceivers.add(new Receiver(receiver, filter));
        }
        // The answer is the last "sticky" broadcast that matches, which is also how the
        // current value of such a broadcast is read without registering anything.
        for (Intent sticky : mSticky.values()) {
            if (matches(filter, sticky)) {
                return new Intent(sticky);
            }
        }
        return null;
    }

    public synchronized void unregisterReceiver(IIntentReceiver receiver) {
        mReceivers.removeIf(r -> r.receiver.asBinder() == receiver.asBinder());
    }

    private static boolean matches(IntentFilter filter, Intent intent) {
        return filter.match(intent.getAction(), intent.getType(), intent.getScheme(), intent.getData(),
                intent.getCategories(), TAG) >= 0;
    }

    public int broadcastIntent(Intent intent, IIntentReceiver resultTo, int resultCode, String resultData,
            Bundle resultExtras, boolean ordered, boolean sticky, int userId) {
        if (mApp == null || intent == null) {
            return ActivityManager.BROADCAST_SUCCESS;
        }
        final Intent copy = new Intent(intent);
        final String packageName = mApp.pkg.packageName;
        if (copy.getPackage() != null && !copy.getPackage().equals(packageName)) {
            return ActivityManager.BROADCAST_SUCCESS;  // addressed to another app: there is none
        }
        post(() -> {
            if (sticky) {
                mSticky.put(copy.getAction(), new Intent(copy));
            }
            ComponentName component = copy.getComponent();
            // Receivers declared in the manifest are instantiated for the occasion...
            for (PackageParser.Activity candidate : mApp.pkg.receivers) {
                boolean hit;
                if (component != null) {
                    hit = candidate.getComponentName().equals(component);
                } else {
                    hit = false;
                    for (PackageParser.ActivityIntentInfo filter : candidate.intents) {
                        hit |= matches(filter, copy);
                    }
                }
                if (hit) {
                    ActivityInfo info = candidate.info;
                    info.applicationInfo = mApp.pkg.applicationInfo;
                    appThread().scheduleReceiver(new Intent(copy), info, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                            resultCode, resultData, resultExtras, false /* ordered */, true /* assumeDelivered */,
                            userId, ActivityManager.PROCESS_STATE_TOP, android.os.Process.myUid(), packageName);
                }
            }
            // ...and those registered at run time are called.
            if (component == null) {
                for (Receiver receiver : new ArrayList<>(mReceivers)) {
                    if (matches(receiver.filter, copy)) {
                        receiver.receiver.performReceive(new Intent(copy), resultCode, resultData, resultExtras,
                                false /* ordered */, sticky, userId);
                    }
                }
            }
            if (resultTo != null) {
                resultTo.performReceive(new Intent(copy), resultCode, resultData, resultExtras, false, false, userId);
            }
        });
        return ActivityManager.BROADCAST_SUCCESS;
    }
}
