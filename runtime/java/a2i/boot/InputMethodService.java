package a2i.boot;

import android.graphics.Rect;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.text.InputType;
import android.util.Log;
import android.util.SparseArray;
import android.view.A2IWindowManager;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ImeTracker;
import android.view.inputmethod.InputMethodManager;
import android.widget.TextView;
import android.window.ImeOnBackInvokedDispatcher;

import com.android.internal.inputmethod.IImeTracker;
import com.android.internal.inputmethod.IInputMethodClient;
import com.android.internal.inputmethod.IInputMethodSession;
import com.android.internal.inputmethod.IRemoteAccessibilityInputConnection;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.inputmethod.InputBindResult;
import com.android.internal.inputmethod.InputConnectionCommandHeader;
import com.android.internal.inputmethod.StartInputFlags;

/**
 * com.android.internal.view.IInputMethodManager: text input.
 *
 * On a device this service connects the app to the current input method, a separate app
 * that draws the soft keyboard and turns touches on it into edits. The app's side is
 * InputMethodManager: when a text field gains focus it opens an input connection to the
 * field and hands it to this service, and it asks this service to show or hide the
 * keyboard. The input method then edits the field through that connection.
 *
 * Here the input method is the host platform's keyboard. This class keeps the service's
 * half of the protocol (so the framework's InputMethodManager and the app's text fields
 * work unmodified), asks a {@link Keyboard} to appear and disappear, and applies what is
 * typed on it to the current input connection.
 */
public final class InputMethodService {
    private static final String TAG = "a2i.InputMethod";

    /** The host's soft keyboard. Methods may be called on any thread. */
    public interface Keyboard {
        /**
         * Shows the keyboard, or updates it if it is showing, for a field with the given
         * EditorInfo.inputType and imeOptions whose bottom edge is at {@code fieldBottom}
         * on screen, in pixels.
         */
        void show(int inputType, int imeOptions, int fieldBottom);

        void hide();
    }

    private final Object mLock = new Object();
    private final IInputMethodSession mSession = (IInputMethodSession) Services.createInterface(
            "input_method/session", IInputMethodSession.class, new Session(), new Binder());
    private final IImeTracker mTracker = (IImeTracker) Services.createInterface(
            "input_method/tracker", IImeTracker.class, new Tracker(), new Binder());
    private Keyboard mKeyboard;
    private IInputMethodClient mClient;
    private IInputMethodClient mActiveClient;
    /** The text field being edited, if any: its connection, description, and window. */
    private IRemoteInputConnection mConnection;
    private EditorInfo mEditor;
    private IBinder mWindow;
    private int mConnectionSession;
    private int mSequence;
    private boolean mShown;

    public void setKeyboard(Keyboard keyboard) {
        mKeyboard = keyboard;
    }

    public boolean isKeyboardShown() {
        synchronized (mLock) {
            return mShown;
        }
    }

    // --- IInputMethodManager ---------------------------------------------------------------

    public void addClient(IInputMethodClient client, IRemoteInputConnection fallbackInputConnection,
            int untrustedDisplayId) {
        synchronized (mLock) {
            mClient = client;
        }
    }

    /**
     * Called when a window gains focus and when the focused view changes within it. With
     * an input connection, the view is a text field that typing now goes to.
     */
    public InputBindResult startInputOrWindowGainedFocus(int startInputReason, IInputMethodClient client,
            IBinder windowToken, int startInputFlags, int softInputMode, int windowFlags,
            EditorInfo editorInfo, IRemoteInputConnection inputConnection,
            IRemoteAccessibilityInputConnection remoteAccessibilityInputConnection,
            int unverifiedTargetSdkVersion, int userId, ImeOnBackInvokedDispatcher imeDispatcher) {
        boolean show = false, hide = false, activate;
        InputBindResult result;
        synchronized (mLock) {
            activate = inputConnection != null && mActiveClient != client;
            if (activate) {
                mActiveClient = client;
            }
            boolean windowGainedFocus = (startInputFlags & StartInputFlags.WINDOW_GAINED_FOCUS) != 0;
            boolean windowChanged = windowToken != mWindow;
            if (editorInfo == null) {
                // Only a report that the window has focus; the field, if any, follows.
                result = new InputBindResult(InputBindResult.ResultCode.SUCCESS_REPORT_WINDOW_FOCUS_ONLY,
                        null, null, null, null, -1, false);
            } else {
                mConnection = inputConnection;
                mEditor = inputConnection != null ? editorInfo : null;
                mWindow = windowToken;
                mConnectionSession = 0;
                result = inputConnection == null ? InputBindResult.NO_EDITOR
                        : new InputBindResult(InputBindResult.ResultCode.SUCCESS_WITH_IME_SESSION, mSession,
                                new SparseArray<>(), null, "a2i/.HostKeyboard", ++mSequence, false);
            }
            if (windowGainedFocus) {
                // What a window wants to happen to the keyboard when it comes to the front.
                switch (softInputMode & WindowManager.LayoutParams.SOFT_INPUT_MASK_STATE) {
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE:
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE:
                        show = true;
                        break;
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_HIDDEN:
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN:
                        hide = true;
                        break;
                    default:
                        // Left over from another window: it goes away unless this window
                        // has a field for it too.
                        hide = windowChanged && (startInputFlags & StartInputFlags.IS_TEXT_EDITOR) == 0;
                        break;
                }
            }
            if (editorInfo != null && inputConnection == null) {
                hide = true;  // focus moved to something that is not a text field
            }
            if (mShown && !hide && mConnection != null) {
                show = true;  // a different field: the keyboard adapts to it
            }
        }
        if (activate) {
            // Once per client: being activated makes the client start input again.
            try {
                client.setActive(true, false);
            } catch (RemoteException e) {
                // In-process: cannot happen.
            }
        }
        if (hide) {
            setShown(false);
        } else if (show) {
            setShown(true);
        }
        return result;
    }

    public void startInputOrWindowGainedFocusAsync(int startInputReason, IInputMethodClient client,
            IBinder windowToken, int startInputFlags, int softInputMode, int windowFlags,
            EditorInfo editorInfo, IRemoteInputConnection inputConnection,
            IRemoteAccessibilityInputConnection remoteAccessibilityInputConnection,
            int unverifiedTargetSdkVersion, int userId, ImeOnBackInvokedDispatcher imeDispatcher,
            int startInputSeq) throws RemoteException {
        client.onStartInputResult(startInputOrWindowGainedFocus(startInputReason, client, windowToken,
                startInputFlags, softInputMode, windowFlags, editorInfo, inputConnection,
                remoteAccessibilityInputConnection, unverifiedTargetSdkVersion, userId, imeDispatcher),
                startInputSeq);
    }

    public boolean showSoftInput(IInputMethodClient client, IBinder windowToken, ImeTracker.Token statsToken,
            int flags, int lastClickToolType, ResultReceiver resultReceiver, int reason) {
        boolean wasShown = isKeyboardShown();
        boolean shown = setShown(true);
        if (resultReceiver != null) {
            resultReceiver.send(!shown ? InputMethodManager.RESULT_UNCHANGED_HIDDEN
                    : wasShown ? InputMethodManager.RESULT_UNCHANGED_SHOWN : InputMethodManager.RESULT_SHOWN, null);
        }
        return shown;
    }

    public boolean hideSoftInput(IInputMethodClient client, IBinder windowToken, ImeTracker.Token statsToken,
            int flags, ResultReceiver resultReceiver, int reason) {
        boolean wasShown = isKeyboardShown();
        setShown(false);
        if (resultReceiver != null) {
            resultReceiver.send(wasShown ? InputMethodManager.RESULT_HIDDEN
                    : InputMethodManager.RESULT_UNCHANGED_HIDDEN, null);
        }
        return wasShown;
    }

    public IImeTracker getImeTrackerService() {
        return mTracker;
    }

    /**
     * com.android.internal.inputmethod.IImeTracker: the service that follows each request
     * to show or hide the keyboard through its stages, for diagnostics. Requests need a
     * token to be followed by; nothing is recorded.
     */
    public static final class Tracker {
        public ImeTracker.Token onStart(String tag, int uid, int type, int origin, int reason, boolean fromUser) {
            return new ImeTracker.Token(new Binder(), tag);
        }
    }

    /** Shows or hides the keyboard. It is only shown while there is a field to type into. */
    private boolean setShown(boolean shown) {
        Keyboard keyboard = mKeyboard;
        int inputType = 0, imeOptions = 0;
        synchronized (mLock) {
            if (shown && (mConnection == null || keyboard == null)) {
                shown = false;
            }
            if (!shown && !mShown) {
                return false;
            }
            mShown = shown;
            if (shown) {
                inputType = mEditor.inputType;
                imeOptions = mEditor.imeOptions;
            }
        }
        if (shown) {
            Rect field = new Rect();
            int bottom = A2IWindowManager.getInstance().getFocusedViewBounds(field) ? field.bottom : 0;
            keyboard.show(inputType, imeOptions, bottom);
        } else if (keyboard != null) {
            keyboard.hide();
        }
        return shown;
    }

    /** com.android.internal.inputmethod.IInputMethodSession: what the app tells the input method. */
    public final class Session {
        /** The field's content changed under the input method; it gets a fresh connection. */
        public void invalidateInput(EditorInfo editorInfo, IRemoteInputConnection inputConnection, int sessionId) {
            synchronized (mLock) {
                mConnection = inputConnection;
                mEditor = editorInfo;
                mConnectionSession = sessionId;
            }
        }
    }

    // --- What is typed on the keyboard. Call on Android's main thread. ----------------------

    private interface Edit {
        void apply(IRemoteInputConnection connection, InputConnectionCommandHeader header) throws RemoteException;
    }

    private boolean edit(Edit edit) {
        IRemoteInputConnection connection;
        InputConnectionCommandHeader header;
        synchronized (mLock) {
            connection = mConnection;
            header = new InputConnectionCommandHeader(mConnectionSession);
        }
        if (connection == null) {
            return false;
        }
        try {
            edit.apply(connection, header);
        } catch (RemoteException e) {
            Log.w(TAG, "input connection failed", e);
        }
        return true;
    }

    /** Text was entered: it replaces the text being composed, if any, or is inserted at the cursor. */
    public void commitText(String text) {
        edit((connection, header) -> connection.commitText(header, text, 1));
    }

    /**
     * Text is being composed (the letters typed so far for a Chinese character, say): it
     * is shown in the field, marked, until committed or replaced by a later composition.
     */
    public void setComposingText(String text) {
        edit((connection, header) -> connection.setComposingText(header, text, 1));
    }

    /** The delete key: removes the selected text, or else the character before the cursor. */
    public void deleteBackward() {
        View focus = A2IWindowManager.getInstance().getFocusedView();
        boolean selection = focus instanceof TextView && ((TextView) focus).hasSelection();
        boolean edited = edit((connection, header) -> {
            if (selection) {
                connection.commitText(header, "", 1);
            } else {
                connection.deleteSurroundingTextInCodePoints(header, 1, 0);
            }
        });
        if (!edited) {
            AppRuntime.injectKey(KeyEvent.KEYCODE_DEL);
        }
    }

    /**
     * The return key: performs the field's action (search, send, next field, done) if it
     * has one for this key; otherwise it is a line break in a multi-line field and an
     * Enter key press elsewhere.
     */
    public void enter() {
        int options;
        boolean multiLine;
        synchronized (mLock) {
            options = mEditor != null ? mEditor.imeOptions : EditorInfo.IME_ACTION_NONE;
            multiLine = mEditor != null && (mEditor.inputType & InputType.TYPE_TEXT_FLAG_MULTI_LINE) != 0;
        }
        int action = options & EditorInfo.IME_MASK_ACTION;
        if ((options & EditorInfo.IME_FLAG_NO_ENTER_ACTION) == 0 && action != EditorInfo.IME_ACTION_NONE
                && edit((connection, header) -> connection.performEditorAction(header, action))) {
            return;
        }
        if (multiLine && edit((connection, header) -> connection.commitText(header, "\n", 1))) {
            return;
        }
        AppRuntime.injectKey(KeyEvent.KEYCODE_ENTER);
    }

    /** The user dismissed the keyboard (the back gesture does, as the back key does on Android). */
    public void dismiss() {
        setShown(false);
    }
}
