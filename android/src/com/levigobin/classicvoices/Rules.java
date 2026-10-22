package com.levigobin.classicvoices;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;

/**
 * Apple's own text rules for the Vocalizer voices (the iOS app's
 * PronunciationRules.swift): iOS 5 and 6 shipped
 * TTSResources/<language>/user_rules.txt, regular expressions VoiceServices
 * ran over the text before the engine saw it ("iOS" -> "I O S", "OS X" ->
 * "OS ten", St -> Street ...). The engine never reads the file itself.
 *
 * One rule per line: /pattern/flags --> "replacement", with $1 groups,
 * \xHH and \\ escapes, and the flags i and m.
 */
final class Rules {
    private static final Map<String, Rules> CACHE = new HashMap<>();

    private final List<Pattern> patterns = new ArrayList<>();
    private final List<String> replacements = new ArrayList<>();

    String apply(String text) {
        String s = text;
        for (int i = 0; i < patterns.size(); i++) {
            try {
                s = patterns.get(i).matcher(s).replaceAll(replacements.get(i));
            } catch (RuntimeException e) {
                // a group the pattern doesn't have: skip the rule
            }
        }
        return s;
    }

    static Rules parse(String text) {
        Rules r = new Rules();
        for (String raw : text.split("\r?\n|\r")) {
            String line = raw.trim();
            int arrow = line.indexOf("-->");
            if (!line.startsWith("/") || arrow < 0) continue;
            String left = line.substring(0, arrow).trim();
            String right = line.substring(arrow + 3).trim();
            int hash = right.indexOf("\" #");
            if (hash >= 0) right = right.substring(0, hash) + "\"";
            int close = left.lastIndexOf('/');
            if (close <= 0 || !right.startsWith("\"") || !right.endsWith("\"") || right.length() < 2) continue;
            String pattern = left.substring(1, close);
            String flags = left.substring(close + 1);
            int opts = 0;
            if (flags.contains("i")) opts |= Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE;
            if (flags.contains("m")) opts |= Pattern.MULTILINE;
            try {
                r.patterns.add(Pattern.compile(pattern, opts));
            } catch (PatternSyntaxException e) {
                continue;
            }
            r.replacements.add(template(unescape(right.substring(1, right.length() - 1))));
        }
        return r;
    }

    /** C-style escapes in the file's replacement strings. */
    private static String unescape(String s) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != '\\' || i + 1 >= s.length()) {
                out.append(c);
                continue;
            }
            char n = s.charAt(++i);
            switch (n) {
                case 'x': {
                    int j = i + 1, v = 0, digits = 0;
                    while (j < s.length() && digits < 2 && Character.digit(s.charAt(j), 16) >= 0) {
                        v = v * 16 + Character.digit(s.charAt(j), 16);
                        j++;
                        digits++;
                    }
                    if (digits > 0) out.append((char) v);
                    i = j - 1;
                    break;
                }
                case 'n': out.append('\n'); break;
                case 't': out.append('\t'); break;
                case 'e': out.append('\u001b'); break;
                default: out.append(n);
            }
        }
        return out.toString();
    }

    /** A literal replacement for Matcher.replaceAll, keeping $1 .. $9. */
    private static String template(String s) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '$' && i + 1 < s.length() && Character.isDigit(s.charAt(i + 1))) {
                out.append('$').append(s.charAt(++i));
            } else if (c == '$' || c == '\\') {
                out.append('\\').append(c);
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    /**
     * The rules for a voice's language: iOS 6's file (the most complete set),
     * else the voice's own firmware's, else iOS 5's. null when none has rules.
     */
    static Rules forVoice(Context c, VoiceInfo v) {
        String lang = v.language;
        String[] builds = {"ios6", v.build(), "ios5"};
        for (String b : builds) {
            File f = new File(new File(new File(new File(Data.voicesDir(c), b), "TTSResources"), lang), "user_rules.txt");
            String key = f.getAbsolutePath();
            synchronized (CACHE) {
                if (CACHE.containsKey(key)) return CACHE.get(key);
            }
            if (!f.isFile()) continue;
            try {
                Rules r = parse(readUtf8(f));
                synchronized (CACHE) {
                    CACHE.put(key, r);
                }
                return r;
            } catch (IOException e) {
                // unreadable: try the next build
            }
        }
        return null;
    }

    static String readUtf8(File f) throws IOException {
        InputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
            return new String(b.toByteArray(), "UTF-8");
        } finally {
            in.close();
        }
    }

}
