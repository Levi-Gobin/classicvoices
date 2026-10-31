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

/**
 * Apple's own text rules for the Vocalizer voices (the iOS app's
 * PronunciationRules.swift): each iOS release shipped
 * TTSResources/<language>/user_rules.txt and each Mac OS X voice its own
 * user_rules.dat: regular expressions run over the text before the engine
 * saw it ("iOS" -> "I O S", "OS X" -> "OS ten", St -> Street ...).
 *
 * One rule per line: /pattern/flags --> "replacement", with $1 groups,
 * \xHH and \\ escapes, and the flags i and m.
 */
final class Rules {
    private static final Map<String, Rules> CACHE = new HashMap<>();

    // One entry per rule, in order. Patterns are compiled the first time the
    // pre-check says a rule could match (thousands of rules per language).
    private final List<String> sources = new ArrayList<>();
    private final List<Integer> flags = new ArrayList<>();
    private final List<String> replacements = new ArrayList<>();
    private Pattern[] compiled;
    private boolean[] broken;  // patterns Java can't compile: skipped
    private long index;        // core/vxrules.c pre-check, built on first use; 0: none

    String apply(String text) {
        int n = sources.size();
        if (n == 0 || text.isEmpty()) return text;
        byte[] marks = new byte[n];
        mark(text, marks);
        String s = text;
        for (int i = 0; i < n; i++) {
            if (marks[i] == 0) continue;
            Pattern p = pattern(i);
            if (p == null) continue;
            try {
                String t = p.matcher(s).replaceAll(replacements.get(i));
                if (!t.equals(s)) {
                    s = t;
                    mark(s, marks);  // rules later on may match the new text
                }
            } catch (RuntimeException e) {
                // a group the pattern doesn't have: skip the rule
            }
        }
        return s;
    }

    /** Sets marks[i] for the rules that could match text (all of them without the pre-check). */
    private synchronized void mark(String text, byte[] marks) {
        if (index == 0) {
            try {
                index = Native.newRuleIndex();
                for (int i = 0; i < sources.size(); i++)
                    Native.ruleIndexAdd(index, i, utf8(sources.get(i)), (flags.get(i) & Pattern.CASE_INSENSITIVE) != 0);
            } catch (UnsatisfiedLinkError e) {
                index = -1;
            }
        }
        if (index > 0) Native.ruleIndexMark(index, utf8(text), marks);
        else java.util.Arrays.fill(marks, (byte) 1);
    }

    private synchronized Pattern pattern(int i) {
        if (compiled == null) {
            compiled = new Pattern[sources.size()];
            broken = new boolean[sources.size()];
        }
        if (compiled[i] != null || broken[i]) return compiled[i];
        String src = posixClasses(sources.get(i));
        int f = flags.get(i);
        try {
            compiled[i] = Pattern.compile(src, f | Pattern.UNICODE_CHARACTER_CLASS);
        } catch (IllegalArgumentException e) {  // PatternSyntaxException, or a runtime without the flag
            try {
                compiled[i] = Pattern.compile(src, f);
            } catch (IllegalArgumentException e2) {
                broken[i] = true;
            }
        }
        return compiled[i];
    }

    private static byte[] utf8(String s) {
        try {
            return s.getBytes("UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            return new byte[0];
        }
    }

    private static final String[][] POSIX = {
            {"alpha", "\\p{Alpha}"}, {"digit", "\\p{Digit}"}, {"alnum", "\\p{Alnum}"}, {"upper", "\\p{Upper}"},
            {"lower", "\\p{Lower}"}, {"punct", "\\p{Punct}"}, {"space", "\\s"}, {"xdigit", "\\p{XDigit}"},
            {"cntrl", "\\p{Cntrl}"}, {"print", "\\p{Print}"}, {"graph", "\\p{Graph}"}, {"blank", "\\p{Blank}"},
            {"word", "\\w"}};

    /** PCRE's [:punct:] and friends inside a character class, as Java writes them. */
    static String posixClasses(String p) {
        if (!p.contains("[:")) return p;
        StringBuilder out = new StringBuilder();
        int depth = 0;
        for (int i = 0; i < p.length(); i++) {
            char c = p.charAt(i);
            if (c == '\\' && i + 1 < p.length()) {
                out.append(c).append(p.charAt(++i));
                continue;
            }
            if (depth > 0 && c == '[' && p.startsWith("[:", i)) {
                int end = p.indexOf(":]", i + 2);
                String name = end > 0 ? p.substring(i + 2, end) : null;
                String repl = null;
                if (name != null) for (String[] m : POSIX) if (m[0].equals(name)) repl = m[1];
                if (repl != null) {
                    out.append(repl);
                    i = end + 1;
                    continue;
                }
            }
            if (c == '[') {
                depth++;
                out.append(c);
                // a ] right after [ or [^ is a literal
                if (i + 1 < p.length() && p.charAt(i + 1) == '^') out.append(p.charAt(++i));
                if (i + 1 < p.length() && p.charAt(i + 1) == ']') out.append(p.charAt(++i));
                continue;
            }
            if (c == ']' && depth > 0) depth--;
            out.append(c);
        }
        return out.toString();
    }

    static Rules parse(String text) {
        Rules r = new Rules();
        r.add(text);
        return r;
    }

    private void add(String text) {
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
            String fl = left.substring(close + 1);
            int opts = 0;
            if (fl.contains("i")) opts |= Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE;
            if (fl.contains("m")) opts |= Pattern.MULTILINE;
            sources.add(pattern);
            flags.add(opts);
            replacements.add(template(unescape(right.substring(1, right.length() - 1))));
        }
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
     * The rules a voice had in its own release: an iOS voice's firmware's
     * TTSResources/<language>/user_rules.txt (an enhanced voice uses its
     * firmware's), a Mac OS X voice's own user_rules.dat in its folder (same
     * format). null when it had none.
     */
    static File ownFile(Context c, VoiceInfo v) {
        int slash = v.key.indexOf('/');
        if (slash < 0) return null;
        String build = v.key.substring(0, slash);
        File f;
        if (build.startsWith("mac")) {
            f = new File(new File(Data.downloadsDir(c), v.key), "user_rules.dat");
            if (!f.isFile()) f = new File(new File(Data.voicesDir(c), v.key), "user_rules.dat");
        } else {
            if (build.endsWith("e")) build = build.substring(0, build.length() - 1);
            f = new File(new File(new File(new File(Data.voicesDir(c), build), "TTSResources"), v.language), "user_rules.txt");
        }
        return f.isFile() ? f : null;
    }

    /**
     * What runs over a voice's text: its own release's rules (own), then
     * Apple's later pack for its language (pack: a Pronunciation/<language>
     * folder; its dictionary goes into the engine, its rules run here). The
     * pack's override rules go before its user rules: they were loaded last
     * on iOS 12, which gave them precedence, and they're written to get in
     * first ("so that a broken rule ... is skipped"). null when there's
     * nothing to run.
     */
    static Rules forVoice(Context c, VoiceInfo v, boolean own, String pack) {
        List<File> files = new ArrayList<>();
        File f = own ? ownFile(c, v) : null;
        if (f != null) files.add(f);
        if (pack != null) {
            for (String name : new String[]{"vocalizer-override-rules.txt", "vocalizer-user-rules.txt"}) {
                File p = new File(pack, name);
                if (p.isFile()) files.add(p);
            }
        }
        if (files.isEmpty()) return null;
        StringBuilder key = new StringBuilder();
        for (File x : files) key.append(x.getAbsolutePath()).append('\n');
        synchronized (CACHE) {
            if (CACHE.containsKey(key.toString())) return CACHE.get(key.toString());
        }
        Rules r = new Rules();
        for (File x : files) {
            try {
                r.add(readUtf8(x));
            } catch (IOException e) {
                // unreadable: the others still run
            }
        }
        synchronized (CACHE) {
            CACHE.put(key.toString(), r);
        }
        return r;
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
