package a2i.boot;

import android.util.Log;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Objects for the platform's large provider interfaces, of which only a part has an
 * implementation here yet. Like a system service (see {@link Services}), such an object
 * is described by its interface and an object that defines the methods that matter;
 * every other method does nothing, returns a neutral value, and is logged the first time
 * it is called.
 */
public final class Neutral {
    private Neutral() {}

    private static final String TAG = "a2i.Neutral";
    private static final Set<String> sReported = Collections.synchronizedSet(new HashSet<>());

    /** Creates an {@code iface} whose calls go to {@code implementation} (may be null) where it defines them. */
    public static <T> T proxy(Class<T> iface, Object implementation) {
        return iface.cast(Proxy.newProxyInstance(Neutral.class.getClassLoader(), new Class<?>[] {iface},
                new Handler(iface, implementation)));
    }

    private static final class Handler implements InvocationHandler {
        private final Class<?> mInterface;
        private final Object mImplementation;
        private final Map<Method, Method> mTargets = new HashMap<>();

        Handler(Class<?> iface, Object implementation) {
            mInterface = iface;
            mImplementation = implementation;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            if (method.getDeclaringClass() == Object.class) {
                switch (method.getName()) {
                    case "hashCode": return System.identityHashCode(proxy);
                    case "equals": return proxy == args[0];
                    default: return mInterface.getSimpleName() + "@" + Integer.toHexString(System.identityHashCode(proxy));
                }
            }
            Method target = findTarget(method);
            if (target != null) {
                try {
                    return target.invoke(mImplementation, args);
                } catch (InvocationTargetException e) {
                    throw e.getCause();
                }
            }
            if (method.isDefault()) {
                // The interface's own default is the right answer for an unimplemented method.
                try {
                    return java.lang.invoke.MethodHandles
                            .privateLookupIn(method.getDeclaringClass(), java.lang.invoke.MethodHandles.lookup())
                            .unreflectSpecial(method, method.getDeclaringClass())
                            .bindTo(proxy)
                            .invokeWithArguments(args != null ? args : new Object[0]);
                } catch (IllegalAccessException | UnsupportedOperationException e) {
                    return value(method.getReturnType());
                }
            }
            if (sReported.add(mInterface.getName() + "." + method.getName())) {
                Log.i(TAG, "unimplemented: " + mInterface.getSimpleName() + "." + method.getName());
            }
            return value(method.getReturnType());
        }

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
                    // Not implemented: answered with a neutral value.
                }
                mTargets.put(method, target);
            }
            return mTargets.get(method);
        }
    }

    /** "Nothing", in the form a caller of the given return type can handle. */
    static Object value(Class<?> type) {
        if (type.isArray()) return java.lang.reflect.Array.newInstance(type.getComponentType(), 0);
        if (type == java.util.List.class) return new java.util.ArrayList<>();
        if (type == java.util.Map.class) return new HashMap<>();
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
