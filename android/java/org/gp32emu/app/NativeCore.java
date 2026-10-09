package org.gp32emu.app;

import android.graphics.Bitmap;

final class NativeCore {
    static { System.loadLibrary("gp32frontend"); }
    static native String open(String system, String saves, String path);
    static native int frame(int input, Bitmap image, short[] pcm);
    static native boolean flush();
    static native String message();
    static native void close();
    private NativeCore() {}
}
