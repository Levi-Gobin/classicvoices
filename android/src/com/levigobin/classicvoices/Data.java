package com.levigobin.classicvoices;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.Locale;

/**
 * Where the voices live and which ones there are. Everything is kept in
 * device-protected storage (Android 7 and later), so the voices can speak on
 * the lock screen after a restart, before the phone is first unlocked:
 *
 *   files/data/Voices/<build>/...        engines and voices from firmware
 *   files/data/Downloads/<build|era>/... enhanced iOS and Mac OS X voices
 *   files/data/Native/<abi>/cvnative_<build>.so   optional fast engine code
 */
final class Data {
    private static final Object LOCK = new Object();
    private static List<VoiceInfo> voices, all;
    private static boolean engineReady;

    private Data() {}

    static Context storageContext(Context c) {
        if (Build.VERSION.SDK_INT >= 24) {
            Context dp = c.getApplicationContext().createDeviceProtectedStorageContext();
            return dp;
        }
        return c.getApplicationContext();
    }

    static File root(Context c) {
        File r = new File(storageContext(c).getFilesDir(), "data");
        //noinspection ResultOfMethodCallIgnored
        r.mkdirs();
        return r;
    }

    static File voicesDir(Context c) { return new File(root(c), "Voices"); }

    static File downloadsDir(Context c) { return new File(root(c), "Downloads"); }

    static String abi() {
        String[] abis = Build.VERSION.SDK_INT >= 21 ? Build.SUPPORTED_ABIS : new String[] {Build.CPU_ABI};
        String lib = System.getProperty("os.arch", "");
        // the ABI this process actually runs (a 32-bit app on a 64-bit phone uses the 32-bit one)
        boolean is64 = lib.contains("64");
        for (String a : abis) {
            if (a.contains("64") == is64) return a;
        }
        return abis.length > 0 ? abis[0] : "unknown";
    }

    static File nativeDir(Context c) { return new File(new File(root(c), "Native"), abi()); }

    static SharedPreferences prefs(Context c) {
        return storageContext(c).getSharedPreferences("settings", Context.MODE_PRIVATE);
    }

    /** Starts the engine (once) and rescans the voices; returns the system's (see voices). */
    static List<VoiceInfo> refresh(Context c) {
        synchronized (LOCK) {
            File v = voicesDir(c), d = downloadsDir(c);
            //noinspection ResultOfMethodCallIgnored
            v.mkdirs();
            //noinspection ResultOfMethodCallIgnored
            d.mkdirs();
            if (!engineReady) {
                engineReady = Native.init(v.getAbsolutePath(), d.getAbsolutePath(), nativeDir(c).getAbsolutePath());
            }
            List<VoiceInfo> out = new ArrayList<>(), shown = new ArrayList<>();
            Set<String> hidden = hidden(c);
            for (String line : Native.voices()) {
                VoiceInfo vi = new VoiceInfo(line);
                out.add(vi);
                if (!hidden.contains(vi.key)) shown.add(vi);
            }
            all = Collections.unmodifiableList(out);
            voices = Collections.unmodifiableList(shown);
            return voices;
        }
    }

    /** The voices Android sees: all but those taken out with "Use as a system voice". */
    static List<VoiceInfo> voices(Context c) {
        synchronized (LOCK) {
            if (voices != null) return voices;
        }
        return refresh(c);
    }

    /** Every voice here, for the app's own pages. */
    static List<VoiceInfo> allVoices(Context c) {
        synchronized (LOCK) {
            if (all != null) return all;
        }
        refresh(c);
        synchronized (LOCK) {
            return all;
        }
    }

    /** Voices the user took out of the system's list (the app's "Use as a system voice" off). */
    static Set<String> hidden(Context c) {
        return new HashSet<>(prefs(c).getStringSet("hidden", Collections.<String>emptySet()));
    }

    static void setHidden(Context c, String key, boolean hide) {
        Set<String> h = hidden(c);
        if (hide) h.add(key);
        else h.remove(key);
        prefs(c).edit().putStringSet("hidden", h).commit();
        refresh(c);
    }

    /** Any voice here, hidden ones too: the app plays a hidden voice by its name. */
    static VoiceInfo byAndroidName(Context c, String name) {
        if (name == null) return null;
        for (VoiceInfo v : allVoices(c)) if (v.androidName().equals(name) || v.key.equals(name)) return v;
        return null;
    }

    /** How well a voice matches: 3 language+country, 2 language, 0 not at all. */
    static int match(VoiceInfo v, String lang, String country) {
        if (lang == null || lang.isEmpty()) return 0;
        Locale l = v.locale;
        boolean sameLang = lang.equalsIgnoreCase(l.getLanguage()) || lang.equalsIgnoreCase(iso3(l, true))
                || (lang.equalsIgnoreCase("no") || lang.equalsIgnoreCase("nor")) && l.getLanguage().equals("nb");
        if (!sameLang) return 0;
        if (country == null || country.isEmpty()) return 2;
        return country.equalsIgnoreCase(l.getCountry()) || country.equalsIgnoreCase(iso3(l, false)) ? 3 : 2;
    }

    static String iso3(Locale l, boolean language) {
        try {
            return language ? l.getISO3Language() : l.getISO3Country();
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * The voice for a language: the one chosen in the app for it, else the
     * iOS 6 voice (VoiceOver's own in that era), else enhanced, else any.
     */
    static VoiceInfo defaultFor(Context c, String lang, String country) {
        List<VoiceInfo> all = voices(c);
        VoiceInfo best = null;
        int bestScore = -1;
        for (VoiceInfo v : all) {
            int m = match(v, lang, country);
            if (m == 0) continue;
            String chosen = prefs(c).getString("default_" + v.language, null);
            int score = m * 100 + (v.key.equals(chosen) ? 50 : 0) + rank(v);
            if (score > bestScore) {
                bestScore = score;
                best = v;
            }
        }
        return best;
    }

    private static int rank(VoiceInfo v) {
        if (v.key.startsWith("ios6/")) return 20;
        if (v.key.startsWith("ios6e/")) return 15;
        if (v.key.startsWith("ios5")) return 10;
        if (v.key.startsWith("mac")) return 8;
        return 5;
    }

    static boolean rulesEnabled(Context c) { return prefs(c).getBoolean("rules", true); }

    static boolean fallbackEnabled(Context c) { return prefs(c).getBoolean("pitch_fallback", true); }

    static boolean hasEngine(Context c, String build) {
        return new File(new File(voicesDir(c), build), "voiced").isFile();
    }
}
