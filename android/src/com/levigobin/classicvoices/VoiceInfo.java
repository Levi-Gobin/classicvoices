package com.levigobin.classicvoices;

import java.util.Locale;

/** One voice from the engine's catalogue (ports/host/cv_catalog.c). */
final class VoiceInfo {
    final String key;       // "ios6/en-US", "ios6e/en-US", "mac107/Karen"
    final String name;      // "Samantha (iOS 6)"
    final String voice;     // "Samantha"
    final String language;  // "en-US"
    final String fallback;  // compact voice for pitch changes, or ""
    final int quality;      // 1 compact, 2 enhanced / premium
    final int female;       // 1, 0, -1 unknown
    final Locale locale;

    VoiceInfo(String line) {
        String[] f = line.split("\t", -1);
        key = f[0];
        name = f[1];
        voice = f[2];
        language = f[3];
        fallback = f.length > 4 ? f[4] : "";
        quality = f.length > 5 ? parse(f[5]) : 1;
        female = f.length > 6 ? parse(f[6]) : -1;
        locale = toLocale(language);
    }

    private static int parse(String s) {
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /**
     * "en-US" -> Locale("en", "US"). Apple's "no-NO" is Norwegian Bokmål, and
     * Fiona's "en-scotland" is British English with a Scottish variant.
     */
    static Locale toLocale(String tag) {
        String[] p = tag.replace('_', '-').split("-");
        String lang = p.length > 0 ? p[0].toLowerCase(Locale.ROOT) : "";
        String country = p.length > 1 ? p[1].toUpperCase(Locale.ROOT) : "";
        if (lang.equals("no")) lang = "nb";
        if (country.length() > 3) return new Locale(lang, "GB".equals(country) || "SCOTLAND".equals(country) ? "GB" : "", country);
        return new Locale(lang, country);
    }

    /** The voice name Android sees: stable, unique, no spaces. */
    String androidName() {
        return "classicvoices-" + key.replace('/', '-');
    }

    /** The firmware build whose engine speaks this voice. */
    String build() {
        if (key.startsWith("mac109/")) return key.endsWith("Compact") ? "ios7" : "ios10";
        if (key.startsWith("mac10")) return "ios6";
        String b = key.substring(0, key.indexOf('/'));
        return b.endsWith("e") ? b.substring(0, b.length() - 1) : b;
    }

    /** "iOS 6", "iOS 6 enhanced", "Mac OS X Lion premium" ... */
    String generation() {
        int open = name.indexOf('(');
        return open >= 0 ? name.substring(open + 1, name.length() - 1) : key;
    }
}
