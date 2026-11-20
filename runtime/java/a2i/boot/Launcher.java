package a2i.boot;

import android.app.A2IActivityTaskManager;
import android.graphics.Bitmap;
import android.os.Handler;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;

/**
 * Command-line entry point for exercising the framework without the iOS app around it:
 *
 *   a2i-vm ... a2i.boot.Launcher <android-root> <native-lib-dir> <data-dir> <apk> <out.png>
 *              <width> <height> <dpi> [step]...
 *
 * Launches the APK's launcher activity, performs the steps in order, and finally writes
 * the screen's last frame to out.png and exits. A step is one of
 *
 *   tap:x,y        touch down and up at a point, in pixels
 *   back           the back key
 *   type:text      text entered on the soft keyboard
 *   compose:text   text being composed on the soft keyboard (not yet final)
 *   delete         the keyboard's delete key
 *   return         the keyboard's return key
 *   background     the app leaves the screen
 *   foreground     ... and returns
 *   shot:file.png  write the current frame to a file
 *   dump:file.txt  write a description of all windows and their view hierarchies
 *   wait:ms        let time pass before the next step (steps are otherwise 150 ms apart)
 *   follow:file    from here on, take steps from a text file as lines are appended to it
 *                  (one step per line), until the line "quit": an interactive session
 */
public final class Launcher {
    private Launcher() {}

    private static final long STEP_INTERVAL_MS = 150;

    private static volatile Bitmap sLastFrame;
    private static int sFrames;
    private static Handler sHandler;
    private static long sStart;
    private static String sOutput;

    public static void main(String[] args) throws Exception {
        AppRuntime.Config config = new AppRuntime.Config();
        config.androidRoot = args[0];
        config.nativeLibraryDir = args[1];
        config.dataDir = args[2];
        config.apkPath = args[3];
        config.appLibraryDir = System.getenv("A2I_APP_LIBS");  // converted with `philae convert-so`
        sOutput = args[4];
        config.width = Integer.parseInt(args[5]);
        config.height = Integer.parseInt(args[6]);
        config.densityDpi = Integer.parseInt(args[7]);

        sStart = System.nanoTime();
        // There is no keyboard on the command line; say when the app asks for one.
        AppRuntime.setKeyboard(new InputMethodService.Keyboard() {
            @Override
            public void show(int inputType, int imeOptions, int fieldBottom) {
                log("keyboard shown: inputType=0x" + Integer.toHexString(inputType) + " imeOptions=0x"
                        + Integer.toHexString(imeOptions) + " fieldBottom=" + fieldBottom);
            }

            @Override
            public void hide() {
                log("keyboard hidden");
            }
        });
        AppRuntime.run(config, frame -> {
            sLastFrame = frame;
            sFrames++;
        }, () -> {
            log("activity resumed");
            sHandler = AppRuntime.mainHandler();
            long at = 300;  // let the first frames settle
            String follow = null;
            for (int i = 8; i < args.length && follow == null; i++) {
                String step = args[i];
                if (step.startsWith("wait:")) {
                    at += Long.parseLong(step.substring(5));
                } else if (step.startsWith("follow:")) {
                    follow = step.substring(7);
                } else {
                    sHandler.postDelayed(() -> perform(step), at);
                    at += STEP_INTERVAL_MS;
                }
            }
            if (follow != null) {
                String file = follow;
                sHandler.postDelayed(() -> new Thread(() -> follow(file), "launcher-follow").start(), at);
            } else {
                sHandler.postDelayed(Launcher::finish, at + 500);
            }
        });
    }

    /** Performs one step now. Must be called on the main thread. */
    private static void perform(String text) {
        String[] step = text.trim().split(":", 2);
        switch (step[0]) {
            case "tap": {
                String[] point = step[1].split(",");
                float x = Float.parseFloat(point[0]), y = Float.parseFloat(point[1]);
                AppRuntime.injectTouch(MotionEvent.ACTION_DOWN, x, y);
                sHandler.postDelayed(() -> AppRuntime.injectTouch(MotionEvent.ACTION_UP, x, y), 60);
                break;
            }
            case "back":
                AppRuntime.injectKey(KeyEvent.KEYCODE_BACK);
                break;
            case "type":
                AppRuntime.withInputMethod(inputMethod -> inputMethod.commitText(step[1]));
                break;
            case "compose":
                AppRuntime.withInputMethod(inputMethod -> inputMethod.setComposingText(step[1]));
                break;
            case "delete":
                AppRuntime.withInputMethod(InputMethodService::deleteBackward);
                break;
            case "return":
                AppRuntime.withInputMethod(InputMethodService::enter);
                break;
            case "background":
                A2IActivityTaskManager.getInstance().setForeground(false);
                break;
            case "foreground":
                A2IActivityTaskManager.getInstance().setForeground(true);
                break;
            case "shot":
                save(step[1]);
                break;
            case "dump":
                try (java.io.PrintWriter out = new java.io.PrintWriter(step[1], "UTF-8")) {
                    out.print(android.view.A2IWindowManager.getInstance().dump());
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
                break;
            default:
                throw new IllegalArgumentException("unknown step " + text);
        }
    }

    /** Reads steps from a file that another process appends to. Runs on its own thread. */
    private static void follow(String file) {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            StringBuilder line = new StringBuilder();
            while (true) {
                int c = reader.read();
                if (c < 0) {
                    Thread.sleep(50);  // at the end of the file for now
                } else if (c != '\n') {
                    line.append((char) c);
                } else {
                    String step = line.toString().trim();
                    line.setLength(0);
                    if (step.equals("quit")) {
                        break;
                    } else if (step.startsWith("wait:")) {
                        Thread.sleep(Long.parseLong(step.substring(5)));
                    } else if (!step.isEmpty()) {
                        sHandler.post(() -> perform(step));
                        Thread.sleep(STEP_INTERVAL_MS);
                    }
                }
            }
        } catch (IOException | InterruptedException e) {
            e.printStackTrace();
        }
        sHandler.post(Launcher::finish);
    }

    private static void finish() {
        save(sOutput);
        log(sFrames + " frames drawn; last one written to " + sOutput);
        System.exit(0);
    }

    private static void save(String file) {
        // Written under another name first, so that a watcher never sees half a file.
        java.io.File target = new java.io.File(file);
        java.io.File partial = new java.io.File(file + ".part");
        try (FileOutputStream out = new FileOutputStream(partial)) {
            sLastFrame.compress(Bitmap.CompressFormat.PNG, 100, out);
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
        partial.renameTo(target);
    }

    private static void log(String message) {
        System.out.println("[" + (System.nanoTime() - sStart) / 1_000_000 + " ms] " + message);
    }
}
