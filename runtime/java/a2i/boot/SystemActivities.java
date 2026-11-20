package a2i.boot;

import android.app.A2IActivityTaskManager;
import android.app.Activity;
import android.app.ActivityThread;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;

/**
 * Activities that belong to the system rather than to the app, answered here.
 *
 * On a device an intent that no activity of the app takes goes to another app: the
 * documents UI, a browser, the share sheet. Those that have a platform-independent answer
 * are handled in this class; the rest are passed on to the host platform.
 */
final class SystemActivities implements A2IActivityTaskManager.ExternalStarter {
    private static final String TAG = "a2i.SystemActivities";

    private final A2IActivityTaskManager.ExternalStarter mHost;  // may be null

    SystemActivities(A2IActivityTaskManager.ExternalStarter host) {
        mHost = host;
    }

    @Override
    public boolean canStartActivity(Intent intent) {
        return Intent.ACTION_OPEN_DOCUMENT_TREE.equals(intent.getAction())
                || (mHost != null && mHost.canStartActivity(intent));
    }

    @Override
    public Instrumentation.ActivityResult startActivity(Intent intent) {
        if (Intent.ACTION_OPEN_DOCUMENT_TREE.equals(intent.getAction())) {
            return pickFolder();
        }
        return mHost.startActivity(intent);
    }

    /**
     * "Let the user choose a folder for the app to use." There is no folder picker yet,
     * and little to choose from: shared storage is this app's alone. The answer is a
     * folder in it named after the app, created if need be, which is what a user would
     * typically make and pick by hand.
     */
    private Instrumentation.ActivityResult pickFolder() {
        Application app = ActivityThread.currentApplication();
        String name = app.getApplicationInfo().loadLabel(app.getPackageManager()).toString()
                .replaceAll("[/\\\\:]", "_");
        File folder = new File(Environment.getExternalStorageDirectory(), name);
        if (!folder.isDirectory() && !folder.mkdirs()) {
            Log.w(TAG, "cannot create " + folder);
            return new Instrumentation.ActivityResult(Activity.RESULT_CANCELED, null);
        }
        Intent result = new Intent();
        result.setData(DocumentsContract.buildTreeDocumentUri(StorageDocumentsProvider.AUTHORITY,
                StorageDocumentsProvider.documentId(name)));
        result.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        Log.i(TAG, "folder chosen for the app: " + folder);
        return new Instrumentation.ActivityResult(Activity.RESULT_OK, result);
    }
}
