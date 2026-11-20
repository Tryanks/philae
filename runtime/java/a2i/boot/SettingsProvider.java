package a2i.boot;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;

import java.util.HashMap;
import java.util.Map;

/**
 * The "settings" content provider: android.provider.Settings.System/Secure/Global.
 *
 * The framework reads a handful of settings while setting up windows and views (animation
 * scales, accessibility state, font scale, ...). On a device they are rows in the
 * SettingsProvider app's database. Here a setting that has not been written simply has no
 * value, which makes every caller fall back to its built-in default; values written by the
 * app are kept for the lifetime of the process.
 */
public final class SettingsProvider extends ContentProvider {
    private final Map<String, String> mValues = new HashMap<>();

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Bundle call(String method, String name, Bundle extras) {
        // Methods are "GET_global", "PUT_system", ...: one namespace per table.
        int separator = method.indexOf('_');
        if (separator < 0) {
            return null;
        }
        String key = method.substring(separator + 1) + "/" + name;
        Bundle result = new Bundle();
        synchronized (mValues) {
            if (method.startsWith("GET_")) {
                result.putString(Settings.NameValueTable.VALUE, mValues.get(key));
            } else if (method.startsWith("PUT_") && extras != null) {
                mValues.put(key, extras.getString(Settings.NameValueTable.VALUE));
            }
        }
        return result;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
