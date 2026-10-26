package com.levigobin.classicvoices;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * What the app shows, as in the iOS, Mac, Windows and Linux apps: each
 * version's voices, the ones that are here and the ones Apple still serves,
 * and the firmware each version needs.
 */
final class Library {
    private Library() {}

    static final class Version {
        final String id, name, number;
        final boolean mac;
        final String[] firmware;  // its own voices' (iOS), or the engine and common files (Mac)

        Version(String id, String name, String number, boolean mac, String... firmware) {
            this.id = id;
            this.name = name;
            this.number = number;
            this.mac = mac;
            this.firmware = firmware;
        }

        String menuName() { return name + " (" + number + ")"; }
    }

    static final Version[] VERSIONS = {
        new Version("ios3", "iOS 3", "3.1.3", false, "ios3"),
        new Version("ios4", "iOS 4", "4.3.5", false, "ios4"),
        new Version("ios5", "iOS 5", "5.1.1", false, "ios5"),
        new Version("ios6", "iOS 6", "6.1.6", false, "ios6"),
        new Version("mac107", "Lion", "10.7", true, "ios5", "ios6"),
        new Version("mac108", "Mountain Lion", "10.8", true, "ios6", "ios7"),
        new Version("mac109", "Mavericks", "10.9", true, "ios7", "ios10"),
    };

    static Version version(String id) {
        for (Version v : VERSIONS) if (v.id.equals(id)) return v;
        return null;
    }

    static List<Version> versions(boolean mac) {
        List<Version> out = new ArrayList<>();
        for (Version v : VERSIONS) if (v.mac == mac) out.add(v);
        return out;
    }

    /** One voice as the pages show it. */
    static final class Item {
        final String id;          // the voice key: "ios6/en-US", "ios6e/en-US", "mac107/Karen"
        final Version version;
        final String name, kind;  // "Samantha", "compact" / "enhanced" / "premium" / "premium and compact"
        final String language;    // "en-US"
        final boolean builtIn;    // from firmware: can't be removed
        final boolean installed;
        final long size;
        final Downloads.Item download;  // what to download, when it isn't here

        Item(String id, Version version, String name, String kind, String language, boolean builtIn,
             boolean installed, long size, Downloads.Item download) {
            this.id = id;
            this.version = version;
            this.name = name;
            this.kind = kind;
            this.language = language;
            this.builtIn = builtIn;
            this.installed = installed;
            this.size = size;
            this.download = download;
        }

        String title() { return name + " (" + kind + ")"; }

        boolean here() { return builtIn || installed; }

        /** The ImportService job that downloads it. */
        String jobArg() { return download != null ? download.id() : null; }
    }

    /** Everything, here or downloadable. */
    static List<Item> items(Context c) {
        List<Item> out = new ArrayList<>();
        Map<String, String> compactNames = new HashMap<>();
        File dl = Data.downloadsDir(c);
        for (VoiceInfo v : Data.allVoices(c)) {
            String build = v.key.substring(0, v.key.indexOf('/')), rest = v.key.substring(build.length() + 1);
            if (build.matches("ios[3-6]")) {
                compactNames.put(build + "/" + rest, v.voice);
                out.add(new Item(v.key, version(build), v.voice, "compact", v.language, true, true, 0, null));
            } else if (build.endsWith("e")) {
                String b = build.substring(0, build.length() - 1);
                out.add(new Item(v.key, version(b), v.voice, "enhanced", v.language, false, true,
                        folderSize(new File(dl, b + "/" + rest)), null));
            } else if (version(build) != null) {
                boolean compact = rest.endsWith("Compact");
                out.add(new Item(v.key, version(build), v.voice, compact ? "compact" : "premium", v.language, false, true,
                        folderSize(new File(dl, build + "/" + rest)), null));
            }
        }
        List<Downloads.Item> more;
        try {
            more = Downloads.available(c);
        } catch (IOException e) {
            more = new ArrayList<>();
        }
        for (Downloads.Item d : more) {
            if (d.kind.equals("enhanced")) {
                String name = compactNames.get(d.build + "/" + d.name);
                out.add(new Item(d.build + "e/" + d.name, version(d.build), name != null ? name : languageName(d.name),
                        "enhanced", d.name, false, false, d.size, d));
            } else {
                out.add(new Item(d.build + "/" + d.name, version(d.build), d.name, "premium and compact", d.locale,
                        false, false, d.size, d));
            }
        }
        return out;
    }

    static boolean firmwareImported(Context c, String key) { return Data.hasEngine(c, key); }

    static boolean ready(Context c, Version v) {
        for (String f : v.firmware) if (!firmwareImported(c, f)) return false;
        return !v.mac || new File(Data.voicesDir(c), v.id + "/common").isDirectory();
    }

    static boolean fastCode(Context c, String build) {
        return new File(Data.nativeDir(c), "cvnative_" + build + ".so").isFile();
    }

    /** "English (United States)", in the phone's language; Fiona's "en-scotland" is Scottish English. */
    static String languageName(String tag) {
        Locale l = VoiceInfo.toLocale(tag);
        if (!l.getVariant().isEmpty()) return l.getDisplayLanguage() + " (Scotland)";
        return l.getDisplayName();
    }

    /** Grouping key: "en-US" and "en_US" are the same language. */
    static String languageKey(String tag) {
        return VoiceInfo.toLocale(tag).toString();
    }

    static List<Item> filter(List<Item> all, boolean mac, String versionId, String languageKey) {
        List<Item> out = new ArrayList<>();
        for (Item i : all) {
            if (i.version == null || i.version.mac != mac) continue;
            if (versionId != null && !versionId.equals("all") && !i.version.id.equals(versionId)) continue;
            if (languageKey != null && !languageKey(i.language).equals(languageKey)) continue;
            out.add(i);
        }
        return out;
    }

    /** Across both tabs and every version: by name, language or version. */
    static List<Item> search(List<Item> all, String query) {
        List<Item> out = new ArrayList<>();
        String q = query.trim().toLowerCase(Locale.getDefault());
        if (q.isEmpty()) return out;
        for (Item i : all) {
            if (i.name.toLowerCase(Locale.getDefault()).contains(q)
                    || languageName(i.language).toLowerCase(Locale.getDefault()).contains(q)
                    || i.version.name.toLowerCase(Locale.getDefault()).contains(q)) out.add(i);
        }
        return out;
    }

    /** Deletes a downloaded voice (a Mac voice's premium and compact go together). */
    static void remove(Context c, Item it) {
        File dl = Data.downloadsDir(c);
        String rest = it.id.substring(it.id.indexOf('/') + 1);
        if (!it.version.mac) {
            Firmware.deleteTree(new File(dl, it.version.id + "/" + rest));
        } else {
            String base = rest.endsWith("Compact") ? rest.substring(0, rest.length() - 7) : rest;
            Firmware.deleteTree(new File(dl, it.version.id + "/" + base));
            Firmware.deleteTree(new File(dl, it.version.id + "/" + base + "Compact"));
        }
        Data.refresh(c);
    }

    static long folderSize(File f) {
        if (f.isFile()) return f.length();
        long n = 0;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) n += folderSize(k);
        return n;
    }

    /** "12.3 MB": one decimal place, decimal units, as in the other apps. */
    static String size(long bytes) {
        double v = Math.max(bytes, 0);
        if (v >= 1e9) return String.format(Locale.getDefault(), "%.1f GB", v / 1e9);
        if (v >= 1e6) return String.format(Locale.getDefault(), "%.1f MB", v / 1e6);
        if (v >= 1e3) return String.format(Locale.getDefault(), "%.1f KB", v / 1e3);
        return (long) v + " bytes";
    }
}
