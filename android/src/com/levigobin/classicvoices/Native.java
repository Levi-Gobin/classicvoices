package com.levigobin.classicvoices;

/** The Classic Voices engine (libclassicvoices.so, see android/jni/cv_jni.c). */
final class Native {
    static {
        System.loadLibrary("classicvoices");
    }

    private Native() {}

    static native boolean init(String voicesRoot, String downloadsRoot, String nativeDir);

    /** key \t name \t voice \t language \t fallback \t quality \t female, one per voice. */
    static native String[] voices();

    /** Starts an utterance; text is UTF-8. Returns the segment count, or -1. */
    static native int begin(String key, String fallback, byte[] text, boolean ssml, int rate, int pitch, int volume);

    /** 16-bit PCM at 22050 Hz into buf: bytes written, 0 if none yet, -1 when done. */
    static native int read(byte[] buf);

    static native void cancel();

    static native void preload(String key);

    static native String lastError();

    static native long nativeStatus(String build);

    static native double[] lastTiming();

    /** Starts the engine afresh (new fast engine code is used from the next utterance). */
    static native void reload();

    /** Unpacks the paths in keep from a .tar.xz into dest; null when done, else what went wrong. */
    static native String untarXz(String archive, String dest, String[] keep, Progress p);

    static final int SAMPLE_RATE = 22050;
}
