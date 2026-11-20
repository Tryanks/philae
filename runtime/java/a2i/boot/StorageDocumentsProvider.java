package a2i.boot;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;

import com.android.internal.content.FileSystemProvider;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * The documents provider for shared storage: what makes folders and files that the user
 * picked ("content://com.android.externalstorage.documents/tree/primary:Folder") usable
 * through Android's storage access framework. On a device it is a separate system app.
 * It serves one root, "primary", which is the shared storage directory.
 */
public final class StorageDocumentsProvider extends FileSystemProvider {
    public static final String AUTHORITY = "com.android.externalstorage.documents";
    private static final String ROOT = "primary";

    private static final String[] DOCUMENT_PROJECTION = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE,
    };
    private static final String[] ROOT_PROJECTION = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_ICON, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID,
        Root.COLUMN_AVAILABLE_BYTES,
    };

    private File mBase;

    /** The document id of a path below the shared storage directory. */
    public static String documentId(String relativePath) {
        return ROOT + ":" + relativePath;
    }

    @Override
    public boolean onCreate() {
        super.onCreate(DOCUMENT_PROJECTION);
        mBase = Environment.getExternalStorageDirectory();
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor result = new MatrixCursor(projection != null ? projection : ROOT_PROJECTION);
        MatrixCursor.RowBuilder row = result.newRow();
        row.add(Root.COLUMN_ROOT_ID, ROOT);
        row.add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD);
        row.add(Root.COLUMN_TITLE, "Internal storage");
        row.add(Root.COLUMN_DOCUMENT_ID, documentId(""));
        row.add(Root.COLUMN_AVAILABLE_BYTES, mBase.getUsableSpace());
        return result;
    }

    @Override
    protected File getFileForDocId(String documentId, boolean visible) throws FileNotFoundException {
        int colon = documentId.indexOf(':');
        if (colon < 0 || !documentId.substring(0, colon).equals(ROOT)) {
            throw new FileNotFoundException("No root for " + documentId);
        }
        File file = new File(mBase, documentId.substring(colon + 1));
        try {
            // A document id must not lead out of the root.
            String base = mBase.getCanonicalPath();
            String path = file.getCanonicalPath();
            if (!path.equals(base) && !path.startsWith(base + File.separator)) {
                throw new FileNotFoundException("Outside the root: " + documentId);
            }
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        return file;
    }

    @Override
    protected String getDocIdForFile(File file) throws FileNotFoundException {
        try {
            String base = mBase.getCanonicalPath();
            String path = file.getCanonicalPath();
            if (path.equals(base)) {
                return documentId("");
            }
            if (!path.startsWith(base + File.separator)) {
                throw new FileNotFoundException("Outside the root: " + file);
            }
            return documentId(path.substring(base.length() + 1));
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }

    @Override
    protected Uri buildNotificationUri(String documentId) {
        return DocumentsContract.buildChildDocumentsUri(AUTHORITY, documentId);
    }
}
