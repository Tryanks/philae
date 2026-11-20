package a2i.boot;

import android.os.Binder;
import android.os.IBinder;
import android.os.IServiceManager;
import android.os.ServiceManager;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Android's system services, inside the app's own process.
 *
 * On a device, framework classes reach system_server over Binder: they ask the service
 * manager for a named service and call it through an AIDL interface. Here there is no
 * other process. The service manager is replaced by this registry, and each service is a
 * local object implementing the same AIDL interface, so a "remote" call is a method call.
 *
 * A service is described by its AIDL interface and an optional implementation object.
 * The object only defines the methods that matter; a proxy answers every other method of
 * the interface with a neutral value (null, 0, false) and logs the first call to it, which
 * is how one finds out what an app actually needs.
 */
public final class Services {
    private Services() {}

    private static final String TAG = "a2i.Services";

    private static final class Entry {
        final String interfaceName;
        final Object implementation;
        IBinder binder;

        Entry(String interfaceName, Object implementation) {
            this.interfaceName = interfaceName;
            this.implementation = implementation;
        }
    }

    private static final Map<String, Entry> sServices = new HashMap<>();
    private static final Set<String> sReported = new HashSet<>();

    /** Declares a service: its name, AIDL interface, and the object that implements part of it (or null). */
    public static synchronized void register(String name, String interfaceName, Object implementation) {
        sServices.put(name, new Entry(interfaceName, implementation));
    }

    /** Routes {@link ServiceManager} lookups to this registry. */
    public static void install() throws ReflectiveOperationException {
        IServiceManager manager = new IServiceManager.Default() {
            @Override
            public IBinder getService(String name) {
                return lookup(name);
            }

            @Override
            public IBinder checkService(String name) {
                return lookup(name);
            }

            @Override
            public boolean isDeclared(String name) {
                return sServices.containsKey(name);
            }

            @Override
            public String[] listServices(int dumpPriority) {
                synchronized (Services.class) {
                    return sServices.keySet().toArray(new String[0]);
                }
            }
        };
        Field field = ServiceManager.class.getDeclaredField("sServiceManager");
        field.setAccessible(true);
        field.set(null, manager);
    }

    /**
     * Services that are absent on purpose, as on a device without the feature. Their
     * client classes cope with absence, but not with a service that never answers: they
     * wait for a callback.
     */
    private static final Set<String> sAbsent = new HashSet<>(java.util.Arrays.asList(
            "autofill"));

    private static synchronized IBinder lookup(String name) {
        Entry entry = sServices.get(name);
        if (entry == null) {
            if (sAbsent.contains(name)) {
                if (sReported.add("service " + name)) {
                    Log.w(TAG, "no such service: " + name);
                }
                return null;
            }
            // Android has a couple of hundred system services, and code that looks one up
            // generally assumes it exists. One nobody has declared is answered like a
            // declared one without an implementation; see UndeclaredService.
            if (sReported.add("service " + name)) {
                Log.i(TAG, "undeclared service: " + name);
            }
            entry = new Entry(null, null);
            entry.binder = new UndeclaredService(name);
            sServices.put(name, entry);
        }
        if (entry.binder == null) {
            try {
                entry.binder = createBinder(name, entry);
            } catch (ReflectiveOperationException e) {
                Log.e(TAG, "cannot create service " + name, e);
                return null;
            }
        }
        return entry.binder;
    }

    /**
     * The binder of a service that was looked up by a name nobody declared. Which AIDL
     * interface it has is only known when the caller converts the binder to one
     * (IFoo.Stub.asInterface asks the binder for its local "IFoo"); at that point it
     * becomes an all-neutral implementation of that interface.
     */
    private static final class UndeclaredService extends Binder {
        private final String mName;
        private final Map<String, android.os.IInterface> mInterfaces = new HashMap<>();

        UndeclaredService(String name) {
            mName = name;
        }

        @Override
        public synchronized android.os.IInterface queryLocalInterface(String descriptor) {
            android.os.IInterface local = mInterfaces.get(descriptor);
            if (local == null) {
                try {
                    // An AIDL interface's descriptor is its class name.
                    Class<?> aidl = Class.forName(descriptor);
                    local = (android.os.IInterface) Proxy.newProxyInstance(Services.class.getClassLoader(),
                            new Class<?>[] {aidl}, new ServiceHandler(mName, aidl, null, this));
                } catch (ClassNotFoundException | IllegalArgumentException e) {
                    return null;
                }
                mInterfaces.put(descriptor, local);
            }
            return local;
        }
    }

    private static IBinder createBinder(String name, Entry entry) throws ReflectiveOperationException {
        Binder binder = new Binder();
        createInterface(name, Class.forName(entry.interfaceName), entry.implementation, binder);
        return binder;
    }

    /**
     * Creates an object implementing the AIDL interface {@code aidl} whose calls go to
     * {@code implementation} where it defines them, and attaches it to {@code binder}.
     */
    public static Object createInterface(String name, Class<?> aidl, Object implementation, Binder binder) {
        Object proxy = Proxy.newProxyInstance(Services.class.getClassLoader(), new Class<?>[] {aidl},
                new ServiceHandler(name, aidl, implementation, binder));
        // With a local interface attached, IFoo.Stub.asInterface(binder) returns the proxy
        // itself instead of building a remote stub. An AIDL interface's descriptor is its
        // fully qualified name.
        binder.attachInterface((android.os.IInterface) proxy, aidl.getName());
        return proxy;
    }

    private static final class ServiceHandler implements InvocationHandler {
        private final String mName;
        private final Class<?> mInterface;
        private final Object mImplementation;
        private final IBinder mBinder;
        private final Map<Method, Method> mTargets = new HashMap<>();

        ServiceHandler(String name, Class<?> aidl, Object implementation, IBinder binder) {
            mName = name;
            mInterface = aidl;
            mImplementation = implementation;
            mBinder = binder;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            if (method.getDeclaringClass() == Object.class) {
                switch (method.getName()) {
                    case "hashCode": return System.identityHashCode(proxy);
                    case "equals": return proxy == args[0];
                    default: return mInterface.getSimpleName() + "@" + mName;
                }
            }
            if (method.getName().equals("asBinder") && method.getParameterCount() == 0) {
                return mBinder;
            }
            Method target = findTarget(method);
            if (target != null) {
                try {
                    return target.invoke(mImplementation, args);
                } catch (InvocationTargetException e) {
                    throw e.getCause();
                }
            }
            synchronized (Services.class) {
                if (sReported.add(mName + "." + method.getName())) {
                    Log.i(TAG, "unimplemented: " + mInterface.getSimpleName() + "." + method.getName());
                }
            }
            Class<?> type = method.getReturnType();
            if (type.isInterface() && android.os.IInterface.class.isAssignableFrom(type)) {
                // A service handing out another AIDL interface (a session, a controller):
                // answer with an object of that interface which is itself all-neutral,
                // the same one each time.
                synchronized (this) {
                    Object sub = mSubInterfaces.get(method);
                    if (sub == null) {
                        sub = createInterface(mName + "/" + method.getName(), type, null, new Binder());
                        mSubInterfaces.put(method, sub);
                    }
                    return sub;
                }
            }
            return neutralValue(type);
        }

        private final Map<Method, Object> mSubInterfaces = new HashMap<>();

        private synchronized Method findTarget(Method method) {
            if (mImplementation == null) {
                return null;
            }
            if (!mTargets.containsKey(method)) {
                Method target = null;
                try {
                    target = mImplementation.getClass().getMethod(method.getName(), method.getParameterTypes());
                    target.setAccessible(true);
                } catch (NoSuchMethodException e) {
                    // Not implemented by this service: answered with a neutral value.
                }
                mTargets.put(method, target);
            }
            return mTargets.get(method);
        }
    }

    /**
     * The answer for a call nobody implements: "nothing", in whatever form the caller can
     * handle. Callers of system services iterate over returned lists and arrays without
     * checking for null, since a real service never returns it.
     */
    private static Object neutralValue(Class<?> type) {
        if (type.isArray()) return java.lang.reflect.Array.newInstance(type.getComponentType(), 0);
        if (type == java.util.List.class) return new java.util.ArrayList<>();
        if (type == java.util.Map.class) return new java.util.HashMap<>();
        if (type == android.content.pm.ParceledListSlice.class) {
            return android.content.pm.ParceledListSlice.emptyList();
        }
        if (type == android.os.LocaleList.class) {
            return android.os.LocaleList.getEmptyLocaleList();
        }
        if (!type.isPrimitive() || type == void.class) return null;
        if (type == boolean.class) return false;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        return (char) 0;
    }
}
