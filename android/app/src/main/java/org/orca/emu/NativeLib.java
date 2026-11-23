package org.orca.emu;

import android.view.Surface;

/** The Orca Core native library (see app/src/main/cpp/orca_jni.cpp). */
final class NativeLib {
    static {
        System.loadLibrary("orca_android");
    }

    private NativeLib() {}

    /** region: 0 USA, 1 Europe, 2 Japan, 3 Korea (used when the NAND is created). */
    static native long create(String nandDir, String keyFile, boolean jit, int region, boolean gles, String dspRom,
                              String dspCoef);
    /** Returns null on success, otherwise the reason. The descriptor is owned by the core afterwards. */
    static native String loadDisc(long handle, int fd);
    static native boolean isWii(long handle);
    static native void setSurface(long handle, Surface surface);
    static native void start(long handle);
    static native void setPaused(long handle, boolean paused);
    static native void destroy(long handle);
    static native void setPad(long handle, int port, int buttons, int stickX, int stickY, int cX, int cY, int l, int r);
    static native void setRemote(long handle, int index, int buttons, float pointerX, float pointerY, int ax, int ay,
                                 int az, boolean nunchuk, int nunchukX, int nunchukY, boolean c, boolean z);
    static native void connectRemote(long handle, int index);
    static native void pressButton(long handle, boolean power);
    static native String status(long handle);
    static native String stopReason(long handle);
}
