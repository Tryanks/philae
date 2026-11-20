package a2i.boot;

import android.app.A2IActivityTaskManager;
import android.app.A2IComponentManager;
import android.app.IServiceConnection;
import android.content.ComponentName;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.IBinder;
import android.app.ActivityThread;
import android.app.ContentProviderHolder;
import android.content.ContentProvider;
import android.app.IApplicationThread;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser;
import android.content.pm.ProviderInfo;
import android.content.pm.UserInfo;
import android.graphics.Point;
import android.os.Bundle;
import android.os.Environment;
import android.os.UserHandle;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.InputDevice;
import android.view.KeyCharacterMap;

import java.io.File;
import java.util.HashMap;
import java.util.Map;

/**
 * The system services this process provides to itself; see {@link Services}.
 *
 * Every service an app's framework code may look up is declared here with its AIDL
 * interface, so that lookups succeed and unimplemented calls show up in the log. Only
 * the services with an implementation object below do real work.
 */
final class SystemServices {
    private SystemServices() {}

    private static InputMethodService sInputMethod;

    /** The input method service, where the host connects its keyboard. Null before {@link #registerAll}. */
    static InputMethodService inputMethod() {
        return sInputMethod;
    }

    /** Registers the services that do not depend on the app. Parsing an APK already needs some. */
    static void registerAll(DisplayConfig display) {
        Services.register("display", "android.hardware.display.IDisplayManager", new DisplayService(display));
        Services.register("activity", "android.app.IActivityManager", new ActivityService());
        Services.register("user", "android.os.IUserManager", new UserService());
        Services.register("input", "android.hardware.input.IInputManager", new InputService());
        sInputMethod = new InputMethodService();
        Services.register("input_method", "com.android.internal.view.IInputMethodManager", sInputMethod);
        Services.register("connectivity", "android.net.IConnectivityManager", new ConnectivityService());
        Services.register("media_metrics", "android.media.metrics.IMediaMetricsManager", new MediaMetricsService());
        Services.register("mount", "android.os.storage.IStorageManager", new StorageService());
        Services.register("notification", "android.app.INotificationManager", new NotificationService());
        Services.register("activity_task", "android.app.IActivityTaskManager",
                A2IActivityTaskManager.getInstance().service());

        // Declared, with every method answered neutrally until an app needs more. (Not
        // every service can be declared this way: autofill is absent on purpose, as on a
        // device without that feature, because its client waits for a reply to a callback.)
        String[][] declared = {
            {"window", "android.view.IWindowManager"},
            {"accessibility", "android.view.accessibility.IAccessibilityManager"},
            {"appops", "com.android.internal.app.IAppOpsService"},
            {"power", "android.os.IPowerManager"},
            {"clipboard", "android.content.IClipboard"},
            {"alarm", "android.app.IAlarmManager"},
            {"vibrator_manager", "android.os.IVibratorManagerService"},
            {"audio", "android.media.IAudioService"},
            {"uimode", "android.app.IUiModeManager"},
            {"content_capture", "android.view.contentcapture.IContentCaptureManager"},
            {"textservices", "com.android.internal.textservice.ITextServicesManager"},
            {"permissionmgr", "android.permission.IPermissionManager"},
            {"permission", "android.os.IPermissionController"},
            {"device_policy", "android.app.admin.IDevicePolicyManager"},
            {"statusbar", "com.android.internal.statusbar.IStatusBarService"},
            {"batterystats", "com.android.internal.app.IBatteryStats"},
            {"platform_compat", "com.android.internal.compat.IPlatformCompat"},
            {"content", "android.content.IContentService"},
            {"locale", "android.app.ILocaleManager"},
            {"wallpaper", "android.app.IWallpaperManager"},
            {"search", "android.app.ISearchManager"},
            {"uri_grants", "android.app.IUriGrantsManager"},
            {"color_display", "android.hardware.display.IColorDisplayManager"},
            {"device_state", "android.hardware.devicestate.IDeviceStateManager"},
            {"sensor_privacy", "android.hardware.ISensorPrivacyManager"},
            {"graphicsstats", "android.view.IGraphicsStats"},
            {"voiceinteraction", "com.android.internal.app.IVoiceInteractionManagerService"},
            {"sensitive_content_protection_service", "android.view.ISensitiveContentProtectionManager"},
        };
        for (String[] service : declared) {
            Services.register(service[0], service[1], null);
        }
    }

    /** Registers the services that describe the installed app. */
    static void registerPackage(PackageParser.Package pkg) {
        Services.register("package", "android.content.pm.IPackageManager", new PackageService(pkg));
    }

    /** android.app.IActivityManager: process-level services of the activity manager. */
    public static final class ActivityService {
        private final Map<String, ContentProviderHolder> mProviders = new HashMap<>();

        /**
         * Content providers that exist outside the app, as parts of the system: settings,
         * and the documents provider for shared storage.
         */
        public synchronized ContentProviderHolder getContentProvider(IApplicationThread caller,
                String callingPackage, String authority, int userId, boolean stable) {
            ContentProviderHolder holder = mProviders.get(authority);
            if (holder != null) {
                return holder;
            }
            ContentProvider provider;
            ProviderInfo info = new ProviderInfo();
            if (authority.equals(Settings.AUTHORITY)) {
                provider = new SettingsProvider();
            } else if (authority.equals(StorageDocumentsProvider.AUTHORITY)) {
                provider = new StorageDocumentsProvider();
                // What every documents provider must declare.
                info.grantUriPermissions = true;
                info.readPermission = info.writePermission = android.Manifest.permission.MANAGE_DOCUMENTS;
            } else {
                return null;
            }
            info.authority = authority;
            info.name = provider.getClass().getName();
            info.packageName = "android";
            info.applicationInfo = ActivityThread.currentApplication().getApplicationInfo();
            info.exported = true;
            provider.attachInfo(ActivityThread.currentApplication(), info);
            holder = new ContentProviderHolder(info);
            holder.provider = provider.getIContentProvider();
            holder.noReleaseNeeded = true;
            mProviders.put(authority, holder);
            return holder;
        }

        // Services and broadcasts: see A2IComponentManager.

        public ComponentName startService(IApplicationThread caller, Intent service, String resolvedType,
                boolean requireForeground, String callingPackage, String callingFeatureId, int userId) {
            return A2IComponentManager.getInstance().startService(service, resolvedType);
        }

        public int stopService(IApplicationThread caller, Intent service, String resolvedType, int userId) {
            return A2IComponentManager.getInstance().stopService(service, resolvedType);
        }

        public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
            return A2IComponentManager.getInstance().stopServiceToken(token, startId);
        }

        public int bindService(IApplicationThread caller, IBinder token, Intent service, String resolvedType,
                IServiceConnection connection, long flags, String callingPackage, int userId) {
            return A2IComponentManager.getInstance().bindService(service, resolvedType, connection);
        }

        public int bindServiceInstance(IApplicationThread caller, IBinder token, Intent service,
                String resolvedType, IServiceConnection connection, long flags, String instanceName,
                String callingPackage, int userId) {
            return A2IComponentManager.getInstance().bindService(service, resolvedType, connection);
        }

        public boolean unbindService(IServiceConnection connection) {
            return A2IComponentManager.getInstance().unbindService(connection);
        }

        public void publishService(IBinder token, Intent intent, IBinder service) {
            A2IComponentManager.getInstance().publishService(token, intent, service);
        }

        public Intent registerReceiverWithFeature(IApplicationThread caller, String callerPackage,
                String callingFeatureId, String receiverId, IIntentReceiver receiver, IntentFilter filter,
                String requiredPermission, int userId, int flags) {
            return A2IComponentManager.getInstance().registerReceiver(receiver, filter);
        }

        public void unregisterReceiver(IIntentReceiver receiver) {
            A2IComponentManager.getInstance().unregisterReceiver(receiver);
        }

        public int broadcastIntentWithFeature(IApplicationThread caller, String callingFeatureId, Intent intent,
                String resolvedType, IIntentReceiver resultTo, int resultCode, String resultData,
                Bundle resultExtras, String[] requiredPermissions, String[] excludedPermissions,
                String[] excludedPackages, int appOp, Bundle options, boolean serialized, boolean sticky,
                int userId) {
            return A2IComponentManager.getInstance().broadcastIntent(intent, resultTo, resultCode, resultData,
                    resultExtras, serialized, sticky, userId);
        }

        public int checkPermission(String permission, int pid, int uid) {
            return PackageManager.PERMISSION_GRANTED;
        }

        public int checkPermissionForDevice(String permission, int pid, int uid, int deviceId) {
            return PackageManager.PERMISSION_GRANTED;
        }

        public boolean isUserAMonkey() {
            return false;
        }
    }

    /** android.os.IUserManager: a single, always-unlocked user 0. */
    public static final class UserService {
        public boolean isUserUnlocked(int userId) {
            return true;
        }

        public boolean isUserUnlockingOrUnlocked(int userId) {
            return true;
        }

        public boolean isUserRunning(int userId) {
            return true;
        }

        public boolean isUserForeground(int userId) {
            return true;
        }

        public int[] getProfileIds(int userId, boolean enabledOnly) {
            return new int[] {userId};
        }

        public int getUserSerialNumber(int userId) {
            return userId;
        }

        public int getUserHandle(int serialNumber) {
            return serialNumber;
        }

        public Bundle getUserRestrictions(int userId) {
            return new Bundle();
        }

        public UserInfo getUserInfo(int userId) {
            return new UserInfo(userId, "Owner", UserInfo.FLAG_PRIMARY | UserInfo.FLAG_ADMIN | UserInfo.FLAG_FULL);
        }
    }

    /**
     * android.hardware.input.IInputManager: the input devices. Touches come from the iOS
     * touch screen and need no device entry; the one device here is the virtual keyboard
     * that Android always has, which menus and text widgets consult for key mappings.
     */
    public static final class InputService {
        private InputDevice mVirtualKeyboard;

        public synchronized InputDevice getInputDevice(int deviceId) {
            if (deviceId != KeyCharacterMap.VIRTUAL_KEYBOARD) {
                return null;
            }
            if (mVirtualKeyboard == null) {
                mVirtualKeyboard = new InputDevice.Builder()
                        .setId(KeyCharacterMap.VIRTUAL_KEYBOARD)
                        .setName("Virtual")
                        .setDescriptor("virtual-keyboard")
                        .setSources(InputDevice.SOURCE_KEYBOARD)
                        .setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC)
                        .setKeyCharacterMap(KeyCharacterMap.obtainEmptyMap(KeyCharacterMap.VIRTUAL_KEYBOARD))
                        .build();
            }
            return mVirtualKeyboard;
        }

        public int[] getInputDeviceIds() {
            return new int[] {KeyCharacterMap.VIRTUAL_KEYBOARD};
        }
    }

    /**
     * android.os.storage.IStorageManager: the storage volumes. There is one, the "shared
     * storage" every Android device has (what /sdcard points to). Here it is a directory
     * in the app's own container, so it is shared with nobody.
     */
    public static final class StorageService {
        private final StorageVolume mPrimary;

        StorageService() {
            File path = new File(Environment.getDataDirectory(), "media/0");
            path.mkdirs();
            mPrimary = new StorageVolume("emulated;0", path, path, "Internal shared storage", true /* primary */,
                    false /* removable */, true /* emulated */, false /* externallyManaged */,
                    false /* allowMassStorage */, 0 /* maxFileSize */, UserHandle.of(0), null /* uuid */,
                    null /* fsUuid */, Environment.MEDIA_MOUNTED);
        }

        public StorageVolume[] getVolumeList(int userId, String callingPackage, int flags) {
            return new StorageVolume[] {mPrimary};
        }

        public boolean isCeStorageUnlocked(int userId) {
            return true;
        }
    }

    /**
     * android.media.metrics.IMediaMetricsManager: collects playback statistics. Players
     * open a session with it as a matter of course; sessions get an id, and what is
     * reported to them is not kept.
     */
    public static final class MediaMetricsService {
        private static String newSessionId() {
            return java.util.UUID.randomUUID().toString();
        }

        public String getPlaybackSessionId(int userId) {
            return newSessionId();
        }

        public String getRecordingSessionId(int userId) {
            return newSessionId();
        }

        public String getTranscodingSessionId(int userId) {
            return newSessionId();
        }

        public String getEditingSessionId(int userId) {
            return newSessionId();
        }

        public String getBundleSessionId(int userId) {
            return newSessionId();
        }
    }

    /** The screen as Android sees it: size in pixels and density. */
    static final class DisplayConfig {
        final int width;
        final int height;
        final int densityDpi;

        DisplayConfig(int width, int height, int densityDpi) {
            this.width = width;
            this.height = height;
            this.densityDpi = densityDpi;
        }
    }

    /** android.hardware.display.IDisplayManager: one built-in display, always on. */
    public static final class DisplayService {
        private final DisplayInfo mInfo = new DisplayInfo();

        DisplayService(DisplayConfig config) {
            mInfo.displayId = Display.DEFAULT_DISPLAY;
            mInfo.type = Display.TYPE_INTERNAL;
            mInfo.name = "Built-in Screen";
            mInfo.uniqueId = "local:0";
            mInfo.state = Display.STATE_ON;
            mInfo.flags = Display.FLAG_SUPPORTS_PROTECTED_BUFFERS | Display.FLAG_SECURE;
            mInfo.logicalWidth = mInfo.appWidth = config.width;
            mInfo.logicalHeight = mInfo.appHeight = config.height;
            mInfo.smallestNominalAppWidth = mInfo.smallestNominalAppHeight = Math.min(config.width, config.height);
            mInfo.largestNominalAppWidth = mInfo.largestNominalAppHeight = Math.max(config.width, config.height);
            mInfo.logicalDensityDpi = config.densityDpi;
            mInfo.physicalXDpi = mInfo.physicalYDpi = config.densityDpi;
            mInfo.rotation = android.view.Surface.ROTATION_0;
            mInfo.modeId = mInfo.defaultModeId = 1;
            mInfo.renderFrameRate = 60f;
            mInfo.supportedModes = new Display.Mode[] {new Display.Mode(1, config.width, config.height, 60f)};
            mInfo.appsSupportedModes = mInfo.supportedModes;
        }

        public DisplayInfo getDisplayInfo(int displayId) {
            return displayId == Display.DEFAULT_DISPLAY ? mInfo : null;
        }

        public int[] getDisplayIds(boolean includeDisabled) {
            return new int[] {Display.DEFAULT_DISPLAY};
        }

        public Point getStableDisplaySize() {
            return new Point(mInfo.logicalWidth, mInfo.logicalHeight);
        }
    }
}
