package a2i.boot;

import android.app.A2IActivityTaskManager;
import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser;
import android.content.pm.ParceledListSlice;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.pkg.FrameworkPackageUserState;
import android.os.Process;

import java.util.Collections;

/**
 * android.content.pm.IPackageManager for a system with exactly one installed app: the APK
 * this process was started for. Everything it reports comes from that APK's manifest, as
 * parsed by the framework's own PackageParser.
 */
public final class PackageService {
    private final PackageParser.Package mPackage;
    private final long mInstallTime = System.currentTimeMillis();

    PackageService(PackageParser.Package pkg) {
        mPackage = pkg;
    }

    private boolean isThisApp(String packageName) {
        return mPackage.packageName.equals(packageName);
    }

    public boolean isPackageAvailable(String packageName, int userId) {
        return isThisApp(packageName);
    }

    /**
     * The package "android": the system itself, whose resources are the framework's. Apps
     * look it up by name, for instance to compare other packages' signatures with the
     * platform's.
     */
    private static PackageInfo systemPackage() {
        ApplicationInfo application = new ApplicationInfo();
        application.packageName = "android";
        application.processName = "system";
        application.uid = Process.SYSTEM_UID;
        application.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        application.sourceDir = application.publicSourceDir =
                System.getenv("ANDROID_ROOT") + "/framework/framework-res.apk";
        application.targetSdkVersion = android.os.Build.VERSION.SDK_INT;
        PackageInfo info = new PackageInfo();
        info.packageName = "android";
        info.applicationInfo = application;
        info.versionName = android.os.Build.VERSION.RELEASE;
        // The platform's signing certificate. There is no signed platform here; this
        // stands in for one, so that "the platform's signature" is a definite value that
        // no app's signature equals.
        info.signatures = new android.content.pm.Signature[] {
            new android.content.pm.Signature("philae platform".getBytes(java.nio.charset.StandardCharsets.UTF_8)),
        };
        return info;
    }

    public PackageInfo getPackageInfo(String packageName, long flags, int userId) {
        if ("android".equals(packageName)) {
            return systemPackage();
        }
        if (!isThisApp(packageName)) {
            return null;
        }
        return PackageParser.generatePackageInfo(mPackage, new int[0], (int) flags, mInstallTime, mInstallTime,
                Collections.emptySet(), FrameworkPackageUserState.DEFAULT, userId);
    }

    public ApplicationInfo getApplicationInfo(String packageName, long flags, int userId) {
        if ("android".equals(packageName)) {
            return systemPackage().applicationInfo;
        }
        return isThisApp(packageName) ? mPackage.applicationInfo : null;
    }

    public int getPackageUid(String packageName, long flags, int userId) {
        return isThisApp(packageName) ? Process.myUid() : -1;
    }

    public int getTargetSdkVersion(String packageName) {
        return isThisApp(packageName) ? mPackage.applicationInfo.targetSdkVersion : -1;
    }

    public String[] getPackagesForUid(int uid) {
        return uid == Process.myUid() ? new String[] {mPackage.packageName} : null;
    }

    public String getNameForUid(int uid) {
        return uid == Process.myUid() ? mPackage.packageName : null;
    }

    public ActivityInfo getActivityInfo(ComponentName component, long flags, int userId) {
        for (PackageParser.Activity activity : mPackage.activities) {
            if (activity.getComponentName().equals(component)) {
                return PackageParser.generateActivityInfo(activity, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
            }
        }
        return null;
    }

    public ActivityInfo getReceiverInfo(ComponentName component, long flags, int userId) {
        for (PackageParser.Activity receiver : mPackage.receivers) {
            if (receiver.getComponentName().equals(component)) {
                return PackageParser.generateActivityInfo(receiver, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
            }
        }
        return null;
    }

    public ServiceInfo getServiceInfo(ComponentName component, long flags, int userId) {
        for (PackageParser.Service service : mPackage.services) {
            if (service.getComponentName().equals(component)) {
                return PackageParser.generateServiceInfo(service, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
            }
        }
        return null;
    }

    public ProviderInfo getProviderInfo(ComponentName component, long flags, int userId) {
        for (PackageParser.Provider provider : mPackage.providers) {
            if (provider.getComponentName().equals(component)) {
                return PackageParser.generateProviderInfo(provider, (int) flags, FrameworkPackageUserState.DEFAULT, userId);
            }
        }
        return null;
    }

    public ProviderInfo resolveContentProvider(String authority, long flags, int userId) {
        for (PackageParser.Provider provider : mPackage.providers) {
            if (provider.info.authority == null) {
                continue;
            }
            for (String name : provider.info.authority.split(";")) {
                if (name.equals(authority)) {
                    return PackageParser.generateProviderInfo(provider, (int) flags,
                            FrameworkPackageUserState.DEFAULT, userId);
                }
            }
        }
        return null;
    }

    /**
     * Who would handle an intent to start an activity. Apps ask before starting something
     * outside themselves. The answer is one of the app's own activities, or, for what the
     * host can do with iOS's means (open a link, share), a stand-in for "the system".
     */
    public ResolveInfo resolveIntent(Intent intent, String resolvedType, long flags, int userId) {
        A2IActivityTaskManager activities = A2IActivityTaskManager.getInstance();
        ResolveInfo result = new ResolveInfo();
        result.activityInfo = activities.resolve(intent, resolvedType);
        if (result.activityInfo == null) {
            if (!activities.startsExternally(intent)) {
                return null;
            }
            result.activityInfo = new ActivityInfo();
            result.activityInfo.packageName = "android";
            result.activityInfo.name = "com.android.internal.app.ResolverActivity";
            result.activityInfo.applicationInfo = new ApplicationInfo();
            result.activityInfo.applicationInfo.packageName = "android";
            result.activityInfo.exported = true;
        }
        result.match = IntentFilter.MATCH_CATEGORY_EMPTY;
        return result;
    }

    public ParceledListSlice<ResolveInfo> queryIntentActivities(Intent intent, String resolvedType, long flags,
            int userId) {
        ResolveInfo info = resolveIntent(intent, resolvedType, flags, userId);
        return new ParceledListSlice<>(info != null ? Collections.singletonList(info) : Collections.emptyList());
    }

    /** The app holds every permission it declares; iOS enforces its own at the point of use. */
    public int checkPermission(String permission, String packageName, int userId) {
        return isThisApp(packageName) && mPackage.requestedPermissions.contains(permission)
                ? PackageManager.PERMISSION_GRANTED
                : PackageManager.PERMISSION_DENIED;
    }

    public int getComponentEnabledSetting(ComponentName component, int userId) {
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    public int getApplicationEnabledSetting(String packageName, int userId) {
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    public String getInstallerPackageName(String packageName) {
        return null;
    }
}
