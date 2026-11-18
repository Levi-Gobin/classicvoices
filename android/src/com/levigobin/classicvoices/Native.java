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

    /**
     * Apple's later pronunciation pack (a Pronunciation/<language> folder) for
     * the utterances begun from now on; null for none.
     */
    static native void setRulesPack(String dir);

    /**
     * Roll off the top of the band as iPhone and Mac playback does, for the
     * utterances begun from now on (Android keeps it, which sounds brighter).
     */
    static native void setAppleTone(boolean on);

    /**
     * Pauses as long as asked at any rate (off: they shorten as the rate goes
     * up, as the voices do), and pauses before, between and after spelled
     * letters, for the utterances begun from now on.
     */
    static native void setPauses(boolean steady, boolean spelling, boolean spellLowercase);

    /**
     * Starts an utterance; text is UTF-8. The dictionaries in dictDir (the person's own entries, and the fetched
     * community ones when community is set) and Apple's rules (the voice's own release's when ownRules, then the pack's, setRulesPack) run on its text,
     * never on markup or spelled text. Returns the segment count, or -1.
     */
    static native int begin(String key, String fallback, String language, byte[] text, boolean ssml, int rate, int pitch,
                            int volume, boolean ownRules, String dictDir, boolean community);

    /**
     * begin with the dictionary given as text (UTF-8 .dic lines; empty: none) instead of the files: the dictionary
     * editor's previews.
     */
    static native int beginWithEntries(String key, String fallback, String language, byte[] text, boolean ssml, int rate,
                                       int pitch, int volume, boolean ownRules, byte[] entries);

    /** Engine text that says ipa (UTF-8) in the voice, or null when it can't. */
    static native String phonemeText(String key, String language, byte[] ipa);

    /** IPA for X-SAMPA (UTF-8), or null when it isn't X-SAMPA. */
    static native String xsampaToIpa(byte[] xsampa);

    static native boolean ipaHasVowel(byte[] ipa);

    /** How the voice says a word, as X-SAMPA without stress marks, or null (Convert to Phonemes). */
    static native String textXsampa(String key, String language, byte[] word);

    /** Whether a regular expression (UTF-8) compiles as the voices run it. */
    static native boolean regexCompiles(byte[] pattern);

    /** 16-bit PCM at 22050 Hz into buf: bytes written, 0 if none yet, -1 when done. */
    static native int read(byte[] buf);

    static native void cancel();

    static native void preload(String key);

    static native String lastError();

    static native long nativeStatus(String build);

    static native double[] lastTiming();

    /** Starts the engine afresh (new fast engine code is used from the next utterance). */
    static native void reload();

    /** Automatic language switching (vxlang.h vx_lang_choose): the index in keys of the voice for language
     *  while current speaks, with choices (languages.txt's text) applied; -1 if none speaks it. */
    static native int langPick(String choices, String current, String language, String[] keys, String[] langs);

    /** Unpacks the paths in keep from a .tar.xz into dest; null when done, else what went wrong. */
    static native String untarXz(String archive, String dest, String[] keep, Progress p);

    static final int SAMPLE_RATE = 22050;
}
