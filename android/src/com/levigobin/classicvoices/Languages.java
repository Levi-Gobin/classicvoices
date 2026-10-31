package com.levigobin.classicvoices;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Automatic language switching: the voices chosen for text in other
 * languages, in <data>/languages.txt (the same file and format on every
 * platform; vxlang.h reads it):
 *
 *     * de ios6/de-DE
 *     ios70/en-US de ios70e/de-DE
 *
 * "<for> <language> <voice key>", <for> being "*" (whichever voice reads) or a
 * voice key; languages are bare codes ("de"), except Chinese, chosen per
 * region (zh-CN, zh-TW, zh-HK).
 */
final class Languages {
    private Languages() {}

    static final String EVERY = "*";

    static File file(Context c) { return new File(Data.root(c), "languages.txt"); }

    static String base(String tag) {
        return tag == null ? "" : tag.replace('_', '-').split("-")[0].toLowerCase(java.util.Locale.ROOT);
    }

    /** What a choice is kept under: "de" for de-DE, but "zh-TW" for zh-TW. */
    static String choiceLanguage(String tag) {
        String[] p = (tag == null ? "" : tag).replace('_', '-').split("-");
        String b = p[0].toLowerCase(java.util.Locale.ROOT);
        if (b.equals("zh") && p.length > 1 && !p[1].isEmpty()) return b + "-" + p[1].toUpperCase(java.util.Locale.ROOT);
        return b;
    }

    static String text(Context c) {
        File f = file(c);
        try {
            FileInputStream in = new FileInputStream(f);
            byte[] b = new byte[(int) Math.min(f.length(), 65536)];
            int n = 0, r;
            while (n < b.length && (r = in.read(b, n, b.length - n)) > 0) n += r;
            in.close();
            return new String(b, 0, n, "UTF-8");
        } catch (IOException e) {
            return "";
        }
    }

    /** for ("*" or a voice key) -> language -> voice key. */
    static Map<String, Map<String, String>> load(Context c) {
        Map<String, Map<String, String>> out = new HashMap<>();
        for (String line : text(c).split("\n")) {
            int hash = line.indexOf('#');
            String[] f = (hash >= 0 ? line.substring(0, hash) : line).trim().split("\\s+");
            if (f.length != 3) continue;
            Map<String, String> t = out.get(f[0]);
            if (t == null) out.put(f[0], t = new HashMap<>());
            t.put(choiceLanguage(f[1]), f[2]);
        }
        return out;
    }

    /** key null: back to automatic. forVoice null: whichever voice reads. */
    static void set(Context c, String language, String key, String forVoice) {
        Map<String, Map<String, String>> all = load(c);
        String who = forVoice != null ? forVoice : EVERY;
        Map<String, String> t = all.get(who);
        if (t == null) all.put(who, t = new HashMap<>());
        if (key != null) t.put(choiceLanguage(language), key);
        else t.remove(choiceLanguage(language));
        if (t.isEmpty()) all.remove(who);
        StringBuilder sb = new StringBuilder("# Classic Voices: the voice for each language when the text changes language.\n"
                + "# <for> <language> <voice key>; <for> is * (every voice) or a voice key.\n");
        List<String> whos = new ArrayList<>(all.keySet());
        Collections.sort(whos);  // "*" sorts first
        for (String w : whos)
            for (Map.Entry<String, String> e : new TreeMap<>(all.get(w)).entrySet())
                sb.append(w).append(' ').append(e.getKey()).append(' ').append(e.getValue()).append('\n');
        File f = file(c), tmp = new File(f.getPath() + ".tmp");
        try {
            FileOutputStream out = new FileOutputStream(tmp);
            out.write(sb.toString().getBytes("UTF-8"));
            out.close();
            //noinspection ResultOfMethodCallIgnored
            tmp.renameTo(f);
            Data.languagesChanged();
        } catch (IOException ignored) {
            // the choice isn't kept; automatic stays
        }
    }
}
