package com.levigobin.classicvoices;

import android.content.Context;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * The person's own dictionary entries, next to the fetched community files (as the iPhone's DictionaryStore.swift
 * and the NVDA add-on's dicts.py keep them, and ports/host/cv_dict.c reads them): Dictionaries/Mine/<file> holds
 * the entries they added or changed, Mine/<file>.removed the fetched entries they took out (pattern TAB type).
 * Publishing (GitHub) writes the person's changes into the repository, and tidy drops them once the fetched copy
 * has them.
 */
final class DictionaryStore {
    private DictionaryStore() {}

    /** One line of a .dic file. type 0: anywhere, 1: regular expression, 2: whole word. */
    static final class Entry {
        final String pattern, replacement;
        final boolean caseSensitive;
        final int type;

        Entry(String pattern, String replacement, boolean caseSensitive, int type) {
            this.pattern = pattern;
            this.replacement = replacement;
            this.caseSensitive = caseSensitive;
            this.type = type;
        }

        /** From a .dic line; null for comments, blank and bad lines. */
        static Entry parse(String raw) {
            String line = raw.startsWith("﻿") ? raw.substring(1) : raw;
            if (line.isEmpty() || line.startsWith("#")) return null;
            String[] f = line.split("\t", -1);
            if (f.length < 2 || f[0].isEmpty()) return null;
            boolean cs = f.length > 2 && (f[2].equals("1") || f[2].equals("True"));
            int type = 0;
            if (f.length > 3) {
                try {
                    type = Integer.parseInt(f[3].trim());
                } catch (NumberFormatException e) {
                    type = 0;
                }
            }
            return new Entry(f[0], f[1], cs, type);
        }

        String key() { return type + "\t" + pattern; }

        String line() { return pattern + "\t" + replacement + "\t" + (caseSensitive ? 1 : 0) + "\t" + type; }

        @Override
        public boolean equals(Object o) {
            return o instanceof Entry && ((Entry) o).line().equals(line());
        }

        @Override
        public int hashCode() { return line().hashCode(); }
    }

    enum Origin { COMMUNITY, ADDED, CHANGED, REMOVED }

    static final class Row {
        final Entry entry;
        final Origin origin;

        Row(Entry entry, Origin origin) {
            this.entry = entry;
            this.origin = origin;
        }
    }

    static File folder(Context c) { return Dictionaries.dir(c); }

    static File mine(Context c) { return new File(folder(c), "Mine"); }

    private static File communityFile(Context c, String name) { return new File(folder(c), name); }

    private static File mineFile(Context c, String name) { return new File(mine(c), name); }

    private static File removedFile(Context c, String name) { return new File(mine(c), name + ".removed"); }

    static List<String> lines(File f) {
        String t = f.isFile() ? Dictionaries.readText(f) : null;
        List<String> out = new ArrayList<>();
        if (t == null) return out;
        out.addAll(Arrays.asList(t.split("\n", -1)));
        for (int i = 0; i < out.size(); i++) {
            String l = out.get(i);
            if (l.endsWith("\r")) out.set(i, l.substring(0, l.length() - 1));
        }
        if (!out.isEmpty() && out.get(out.size() - 1).isEmpty()) out.remove(out.size() - 1);
        return out;
    }

    static List<String> splitLines(String text) {
        List<String> out = new ArrayList<>();
        if (text == null) return out;
        if (text.startsWith("﻿")) text = text.substring(1);
        for (String l : text.split("\n", -1)) out.add(l.endsWith("\r") ? l.substring(0, l.length() - 1) : l);
        if (!out.isEmpty() && out.get(out.size() - 1).isEmpty()) out.remove(out.size() - 1);
        return out;
    }

    static List<Entry> entries(File f) { return parseAll(lines(f)); }

    static List<Entry> parseAll(List<String> lines) {
        List<Entry> out = new ArrayList<>();
        for (String l : lines) {
            Entry e = Entry.parse(l);
            if (e != null) out.add(e);
        }
        return out;
    }

    private static void write(File f, List<String> lines) {
        if (lines.isEmpty()) {
            //noinspection ResultOfMethodCallIgnored
            f.delete();
            return;
        }
        //noinspection ResultOfMethodCallIgnored
        f.getParentFile().mkdirs();
        StringBuilder b = new StringBuilder();
        for (String l : lines) b.append(l).append('\n');
        try {
            Dictionaries.writeText(f, b.toString());
        } catch (java.io.IOException e) {
            // the folder is the app's own: nothing to tell
        }
    }

    private static List<String> linesOf(List<Entry> es) {
        List<String> out = new ArrayList<>();
        for (Entry e : es) out.add(e.line());
        return out;
    }

    private static Map<String, Entry> byKey(List<Entry> es) {
        Map<String, Entry> m = new LinkedHashMap<>();
        for (Entry e : es) if (!m.containsKey(e.key())) m.put(e.key(), e);
        return m;
    }

    /** The keys (type TAB pattern) of the fetched entries the person removed. */
    static Set<String> removed(Context c, String name) {
        Set<String> out = new HashSet<>();
        for (String l : lines(removedFile(c, name))) {
            String[] f = l.split("\t", -1);
            if (f.length == 0 || f[0].isEmpty()) continue;
            int type = 0;
            if (f.length > 1) {
                try {
                    type = Integer.parseInt(f[1].trim());
                } catch (NumberFormatException e) {
                    type = 0;
                }
            }
            out.add(type + "\t" + f[0]);
        }
        return out;
    }

    private static void writeRemoved(Context c, String name, Set<String> keys) {
        List<String> out = new ArrayList<>();
        for (String k : new TreeSet<>(keys)) {
            int tab = k.indexOf('\t');
            out.add(tab > 0 ? k.substring(tab + 1) + "\t" + k.substring(0, tab) : k);
        }
        write(removedFile(c, name), out);
    }

    /**
     * The file's entries for the editor: the fetched file's in order (with the person's changes and removals shown
     * on them), then the person's additions.
     */
    static List<Row> rows(Context c, String name) {
        List<Entry> community = entries(communityFile(c, name)), mineEntries = entries(mineFile(c, name));
        Set<String> gone = removed(c, name);
        Map<String, Entry> mineByKey = byKey(mineEntries);
        List<Row> out = new ArrayList<>();
        Set<String> shown = new HashSet<>();
        for (Entry e : community) {
            if (!shown.add(e.key())) continue;
            Entry m = mineByKey.get(e.key());
            if (m != null) out.add(new Row(m, Origin.CHANGED));
            else if (gone.contains(e.key())) out.add(new Row(e, Origin.REMOVED));
            else out.add(new Row(e, Origin.COMMUNITY));
        }
        for (Entry e : mineEntries) if (shown.add(e.key())) out.add(new Row(e, Origin.ADDED));
        return out;
    }

    /** Adds or changes an entry (oldKey: the one it replaces, when its pattern or type changed; may be null). */
    static void put(Context c, String name, Entry entry, String oldKey) {
        List<Entry> all = entries(mineFile(c, name));
        // a changed entry keeps its place: entries apply in order, and a regular expression can depend on the one before
        int at = -1;
        for (int i = 0; i < all.size(); i++) {
            String k = all.get(i).key();
            if (k.equals(entry.key()) || k.equals(oldKey)) {
                at = i;
                break;
            }
        }
        List<Entry> m = new ArrayList<>();
        for (Entry e : all) if (!e.key().equals(entry.key()) && !e.key().equals(oldKey)) m.add(e);
        Map<String, Entry> community = byKey(entries(communityFile(c, name)));
        Set<String> gone = removed(c, name);
        if (oldKey != null && !oldKey.equals(entry.key()) && community.containsKey(oldKey)) gone.add(oldKey);
        gone.remove(entry.key());
        if (!entry.equals(community.get(entry.key()))) {
            if (at >= 0) m.add(Math.min(at, m.size()), entry);
            else m.add(entry);
        }
        write(mineFile(c, name), linesOf(m));
        writeRemoved(c, name, gone);
    }

    /** Takes an entry out: the person's goes; a fetched one is listed as removed. */
    static void remove(Context c, String name, String key) {
        List<Entry> m = new ArrayList<>();
        for (Entry e : entries(mineFile(c, name))) if (!e.key().equals(key)) m.add(e);
        write(mineFile(c, name), linesOf(m));
        if (byKey(entries(communityFile(c, name))).containsKey(key)) {
            Set<String> gone = removed(c, name);
            gone.add(key);
            writeRemoved(c, name, gone);
        }
    }

    /** Back to the fetched entry: the person's change or removal of it is dropped. */
    static void restore(Context c, String name, String key) {
        List<Entry> m = new ArrayList<>();
        for (Entry e : entries(mineFile(c, name))) if (!e.key().equals(key)) m.add(e);
        write(mineFile(c, name), linesOf(m));
        Set<String> gone = removed(c, name);
        gone.remove(key);
        writeRemoved(c, name, gone);
    }

    /**
     * After a fetch (or publishing to the source): the person's entries the repository now has as they are, and
     * removals of entries it no longer has, are dropped.
     */
    static void tidy(Context c) {
        String[] names = mine(c).list();
        if (names == null) return;
        for (String name : names) {
            if (!name.endsWith(".dic")) continue;
            File cf = communityFile(c, name);
            if (!cf.isFile()) continue;
            Map<String, Entry> community = byKey(entries(cf));
            List<Entry> m = entries(mineFile(c, name)), keep = new ArrayList<>();
            for (Entry e : m) if (!e.equals(community.get(e.key()))) keep.add(e);
            if (keep.size() != m.size()) write(mineFile(c, name), linesOf(keep));
            Set<String> gone = removed(c, name), still = new HashSet<>();
            for (String k : gone) if (community.containsKey(k)) still.add(k);
            if (!still.equals(gone)) writeRemoved(c, name, still);
        }
    }

    /** What the person changed in a file against the fetched copy. */
    static final class Changes {
        final List<Entry> added = new ArrayList<>(), changed = new ArrayList<>(), removed = new ArrayList<>();

        boolean isEmpty() { return added.isEmpty() && changed.isEmpty() && removed.isEmpty(); }
    }

    static Changes changes(Context c, String name) {
        Changes out = new Changes();
        Map<String, Entry> community = byKey(entries(communityFile(c, name)));
        for (Entry e : entries(mineFile(c, name))) {
            Entry ce = community.get(e.key());
            if (ce == null) out.added.add(e);
            else if (!ce.equals(e)) out.changed.add(e);
        }
        for (String k : new TreeSet<>(removed(c, name))) {
            Entry ce = community.get(k);
            if (ce != null) out.removed.add(ce);
        }
        return out;
    }

    /** {file name: changes} for every file the person changed, by name. */
    static Map<String, Changes> pending(Context c) {
        Map<String, Changes> out = new LinkedHashMap<>();
        for (String name : fileNames(c)) {
            Changes ch = changes(c, name);
            if (!ch.isEmpty()) out.put(name, ch);
        }
        return out;
    }

    /**
     * A fetched file's lines with the person's changes made in them: changed lines in place, removed ones gone,
     * added ones at the end; comments and order kept. An added entry the file has already (published to a fork
     * before, say) is put in its place, not twice.
     */
    static List<String> applyChanges(List<String> lines, Changes ch) {
        Map<String, Entry> pendingByKey = new LinkedHashMap<>();
        for (Entry e : ch.changed) pendingByKey.put(e.key(), e);
        for (Entry e : ch.added) pendingByKey.put(e.key(), e);
        Set<String> gone = new HashSet<>();
        for (Entry e : ch.removed) gone.add(e.key());
        List<String> out = new ArrayList<>();
        for (String l : lines) {
            Entry e = Entry.parse(l);
            if (e == null) {
                out.add(l);
                continue;
            }
            if (gone.contains(e.key())) continue;
            Entry r = pendingByKey.remove(e.key());
            out.add(r != null ? r.line() : l);
        }
        List<Entry> rest = new ArrayList<>(ch.added);
        rest.addAll(ch.changed);
        for (Entry e : rest) if (pendingByKey.remove(e.key()) != null) out.add(e.line());
        return out;
    }

    static final int ALONE = 0, EARLIER = 1, ALL = 2;

    /**
     * The entries a preview of entry runs, as .dic text, in the order the voice applies them (rows: the fetched
     * entries in order, the person's changes in place, removed ones left out, their additions last): the entry takes
     * the place of the one it replaces (oldKey, or its own key), a new one goes at the end. The fetched entries count
     * only when community is on, as for the voice.
     */
    static String previewEntries(Context c, String name, Entry entry, String oldKey, boolean community, int scope) {
        if (scope == ALONE) return entry.line();
        List<Entry> list = new ArrayList<>();
        for (Row r : rows(c, name)) {
            if (r.origin == Origin.REMOVED || (!community && r.origin == Origin.COMMUNITY)) continue;
            list.add(r.entry);
        }
        Set<String> keys = new HashSet<>();
        keys.add(entry.key());
        if (oldKey != null) keys.add(oldKey);
        int i = -1;
        for (int k = 0; k < list.size(); k++) {
            if (keys.contains(list.get(k).key())) {
                i = k;
                break;
            }
        }
        int at;
        if (i >= 0) {
            List<Entry> kept = new ArrayList<>();
            for (int k = 0; k < list.size(); k++) {
                if (k == i) kept.add(entry);
                else if (!keys.contains(list.get(k).key())) kept.add(list.get(k));
            }
            list = kept;
            at = list.indexOf(entry);
        } else {
            list.add(entry);
            at = list.size() - 1;
        }
        if (scope == EARLIER) list = list.subList(0, at + 1);
        StringBuilder b = new StringBuilder();
        for (Entry e : list) {
            if (b.length() > 0) b.append('\n');
            b.append(e.line());
        }
        return b.toString();
    }

    /** Every dictionary file there is, fetched or the person's, by name. */
    static List<String> fileNames(Context c) {
        Set<String> out = new TreeSet<>();
        for (File d : new File[] {folder(c), mine(c)}) {
            String[] names = d.list();
            if (names == null) continue;
            for (String n : names) if (n.endsWith(".dic") && !n.startsWith(".")) out.add(n);
        }
        return new ArrayList<>(out);
    }

    /**
     * [X-SAMPA, example word] for a language: phonemes/<lang>.txt, its lines replaced and added to by the narrower
     * phonemes/<lang-region>.txt.
     */
    static List<String[]> phonemeExamples(Context c, String language) {
        String lang = language.replace('_', '-').toLowerCase(java.util.Locale.ROOT);
        List<String> files = new ArrayList<>();
        int dash = lang.indexOf('-');
        if (dash > 0) files.add(lang.substring(0, dash));
        files.add(lang);
        Map<String, String> by = new LinkedHashMap<>();
        for (String f : files) {
            for (String l : lines(new File(folder(c), "phonemes/" + f + ".txt"))) {
                int tab = l.indexOf('\t');
                if (l.startsWith("#") || tab < 0) continue;
                String ph = l.substring(0, tab).trim(), ex = l.substring(tab + 1).trim();
                if (ph.isEmpty()) continue;
                by.put(ph, ex);
            }
        }
        List<String[]> out = new ArrayList<>();
        for (Map.Entry<String, String> e : by.entrySet()) out.add(new String[] {e.getKey(), e.getValue()});
        return out;
    }

    // ---------------------------------------------------------------- how changes are described

    static final int MAX_NAMES = 5;

    /** The commit message (as gitsync.py's): up to MAX_NAMES patterns per kind on the first line, the lists after. */
    static String message(String name, Changes ch) {
        List<String> parts = new ArrayList<>();
        String[] verbs = {"add", "modify", "remove"};
        List<List<Entry>> kinds = Arrays.asList(ch.added, ch.changed, ch.removed);
        for (int i = 0; i < 3; i++) {
            List<Entry> l = kinds.get(i);
            if (l.isEmpty()) continue;
            StringBuilder names = new StringBuilder();
            for (int k = 0; k < Math.min(MAX_NAMES, l.size()); k++) {
                if (k > 0) names.append(", ");
                names.append(l.get(k).pattern);
            }
            if (l.size() > MAX_NAMES) names.append(" and ").append(l.size() - MAX_NAMES).append(" more");
            parts.add(verbs[i] + " " + l.size() + (l.size() == 1 ? " entry" : " entries") + " - " + names);
        }
        StringBuilder out = new StringBuilder(name).append(": ").append(join(parts, "; "));
        String[] heads = {"Added", "Modified", "Removed"};
        List<String> body = new ArrayList<>();
        for (int i = 0; i < 3; i++) {
            List<Entry> l = kinds.get(i);
            if (l.size() <= MAX_NAMES) continue;
            StringBuilder b = new StringBuilder(heads[i]).append(':');
            for (Entry e : l) b.append("\n- ").append(e.pattern);
            body.add(b.toString());
        }
        if (!body.isEmpty()) out.append("\n\n").append(join(body, "\n\n"));
        return out.toString();
    }

    /** What was published: "3 entries added, 1 changed and 1 removed in en.dic" (several files: "across 2 files"). */
    static final class Summary {
        final List<String> files = new ArrayList<>();
        int added, changed, removed;

        void add(String name, Changes ch) {
            files.add(name);
            added += ch.added.size();
            changed += ch.changed.size();
            removed += ch.removed.size();
        }

        String text() {
            List<String> parts = new ArrayList<>();
            int[] ns = {added, changed, removed};
            String[] verbs = {"added", "changed", "removed"};
            for (int i = 0; i < 3; i++) {
                if (ns[i] == 0) continue;
                parts.add(parts.isEmpty() ? ns[i] + (ns[i] == 1 ? " entry " : " entries ") + verbs[i] : ns[i] + " " + verbs[i]);
            }
            String what = parts.size() > 1 ? join(parts.subList(0, parts.size() - 1), ", ") + " and " + parts.get(parts.size() - 1)
                    : parts.isEmpty() ? "" : parts.get(0);
            return what + (files.size() == 1 ? " in " + files.get(0) : " across " + files.size() + " files");
        }
    }

    /** "en.dic: 3 added, 1 changed; ..." for what's waiting, or null when nothing is. */
    static String pendingText(Context c) {
        Map<String, Changes> p = pending(c);
        if (p.isEmpty()) return null;
        List<String> out = new ArrayList<>();
        for (Map.Entry<String, Changes> e : p.entrySet()) {
            List<String> parts = new ArrayList<>();
            Changes ch = e.getValue();
            if (!ch.added.isEmpty()) parts.add(ch.added.size() + " added");
            if (!ch.changed.isEmpty()) parts.add(ch.changed.size() + " changed");
            if (!ch.removed.isEmpty()) parts.add(ch.removed.size() + " removed");
            out.add(e.getKey() + ": " + join(parts, ", "));
        }
        return join(out, "; ");
    }

    static String join(List<String> parts, String sep) {
        StringBuilder b = new StringBuilder();
        for (String p : parts) {
            if (b.length() > 0) b.append(sep);
            b.append(p);
        }
        return b.toString();
    }
}
