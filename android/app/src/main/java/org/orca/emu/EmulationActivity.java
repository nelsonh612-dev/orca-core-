package org.orca.emu;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;

/** Runs a game: the picture on a SurfaceView, touch controls on top, physical gamepads. */
public final class EmulationActivity extends Activity implements SurfaceHolder.Callback {
    private long handle;
    private boolean started;
    private ControllerOverlay overlay;
    private TextView status;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final GamepadState pad = new GamepadState();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SharedPreferences prefs = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE);
        File nand = new File(getFilesDir(), "nand");
        File key = MainActivity.keyFile(this);
        handle = NativeLib.create(nand.getAbsolutePath(), key.exists() ? key.getAbsolutePath() : "",
                prefs.getBoolean("jit", true), prefs.getInt("region", 0), prefs.getBoolean("gles", true));
        String error;
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(getIntent().getData(), "r");
            error = pfd == null ? "cannot open the file" : NativeLib.loadDisc(handle, pfd.detachFd());
        } catch (Exception e) {
            error = e.getMessage();
        }
        if (error != null) {
            fail("Cannot start the game", error);
            return;
        }
        FrameLayout root = new FrameLayout(this);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface);
        overlay = new ControllerOverlay(this, handle, NativeLib.isWii(handle));
        root.addView(overlay);
        status = new TextView(this);
        status.setTextColor(0xFFFFFF00);
        root.addView(status);
        setContentView(root);
        hideSystemBars(root);
        if (NativeLib.isWii(handle)) NativeLib.connectRemote(handle, 0);
        ui.postDelayed(this::poll, 1000);
    }

    private void hideSystemBars(View v) {
        v.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    private void poll() {
        if (handle == 0) return;
        String stop = NativeLib.stopReason(handle);
        if (stop != null) {
            fail("Emulation stopped", stop);
            return;
        }
        status.setText(NativeLib.status(handle));
        ui.postDelayed(this::poll, 1000);
    }

    private void fail(String title, String message) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(message)
                .setPositiveButton(android.R.string.ok, (d, w) -> finish())
                .setOnCancelListener(d -> finish()).show();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeLib.setSurface(handle, holder.getSurface());
        if (!started) {
            started = true;
            NativeLib.start(handle);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
        NativeLib.setSurface(handle, holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeLib.setSurface(handle, null);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (handle != 0) NativeLib.setPaused(handle, true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (handle != 0) NativeLib.setPaused(handle, false);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacksAndMessages(null);
        if (handle != 0) NativeLib.destroy(handle);
        handle = 0;
    }

    // --- physical gamepads (first controller = GameCube port 1 / Wii Remote 1) ---

    private static boolean isGamepad(InputDevice d) {
        if (d == null) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (handle != 0 && isGamepad(e.getDevice()) && pad.key(e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN)) {
            if (overlay != null) overlay.setPhysical(pad);
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (handle != 0 && isGamepad(e.getDevice()) && e.getAction() == MotionEvent.ACTION_MOVE) {
            pad.motion(e);
            if (overlay != null) overlay.setPhysical(pad);
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
}
