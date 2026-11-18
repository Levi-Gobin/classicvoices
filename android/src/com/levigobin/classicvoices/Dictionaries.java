package com.levigobin.classicvoices;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The community pronunciation dictionaries (github.com/nicopn/classicvoices-dicts unless the person chose another
 * source, Dictionaries/source.json; the "Community pronunciation dictionaries" switch, off until turned on): every
 * .dic file of the repository, and phonemes/<language>.txt (the editor's example words), fetched into
 * <data>/Dictionaries, where the engine reads a voice's own (ports/host/cv_dict.h). Checked when the switch is
 * turned on, when the app opens and when the voices speak, at most every 6 hours; a changed file is used from the
 * next utterance on, and a file the repository no longer has is removed. The person's own entries
 * (Dictionaries/Mine, DictionaryStore) are used even with the switch off; after a fetch, those the repository now
 * has as they are drop out of Mine (DictionaryStore.tidy).
 */
final class Dictionaries {
    static final String DEFAULT_REPO = "nicopn/classicvoices-dicts", DEFAULT_BRANCH = "main";
    private static final long EVERY = 6 * 3600 * 1000L;
    private static final Object LOCK = new Object(), FETCH = new Object();
    private static boolean busy;
    private static long asked;  // this process: when a check last began
    private static volatile boolean editing;

    private Dictionaries() {}

    /**
     * While the dictionary editor is on screen the voices use no dictionary at all (TtsService), so the patterns
     * are heard uncorrected; previews bring their own entries.
     */
    static boolean editing() { return editing; }

    static void setEditing(boolean on) { editing = on; }

    /** The repository and branch the dictionaries come from: {owner/name, branch}. */
    static String[] source(Context c) {
        try {
            JSONObject j = new JSONObject(readText(new File(dir(c), "source.json")));
            String repo = j.optString("repo", ""), branch = j.optString("branch", "");
            if (validRepo(repo)) return new String[] {repo, branch.isEmpty() ? DEFAULT_BRANCH : branch};
        } catch (Exception e) {
            // none chosen
        }
        return new String[] {DEFAULT_REPO, DEFAULT_BRANCH};
    }

    static boolean validRepo(String r) {
        if (r == null) return false;
        String[] parts = r.split("/", -1);
        if (parts.length != 2) return false;
        for (String p : parts) {
            if (p.isEmpty()) return false;
            for (int i = 0; i < p.length(); i++) {
                char ch = p.charAt(i);
                if (!Character.isLetterOrDigit(ch) && "-_.".indexOf(ch) < 0) return false;
            }
        }
        return true;
    }

    /** owner/name from what was typed ("owner/name", a github.com address, with .git), or null. */
    static String parseRepo(String text) {
        String t = text.trim();
        for (String p : new String[] {"https://", "http://", "git@github.com:", "www.github.com/", "github.com/"}) {
            if (t.toLowerCase(java.util.Locale.ROOT).startsWith(p)) t = t.substring(p.length());
        }
        if (t.toLowerCase(java.util.Locale.ROOT).startsWith("github.com/")) t = t.substring(11);
        while (t.startsWith("/")) t = t.substring(1);
        while (t.endsWith("/")) t = t.substring(0, t.length() - 1);
        if (t.endsWith(".git")) t = t.substring(0, t.length() - 4);
        String[] parts = t.split("/+");
        if (parts.length > 2) t = parts[0] + "/" + parts[1];
        return validRepo(t) ? t : null;
    }

    /** Another repository: what was fetched from the old one stops counting, and the next check is now. */
    static void setSource(Context c, String repo, String branch) {
        try {
            JSONObject j = new JSONObject();
            j.put("repo", repo);
            j.put("branch", branch == null || branch.isEmpty() ? DEFAULT_BRANCH : branch);
            File d = dir(c);
            //noinspection ResultOfMethodCallIgnored
            d.mkdirs();
            writeText(new File(d, "source.json"), j.toString());
        } catch (Exception e) {
            return;
        }
        state(c).edit().clear().apply();
        synchronized (LOCK) {
            asked = 0;
        }
    }

    static boolean enabled(Context c) { return Data.prefs(c).getBoolean("community_dictionaries", false); }

    /** A notification when the dictionaries change by themselves (opt in). */
    static boolean notifyChanges(Context c) { return Data.prefs(c).getBoolean("dictionaries_notify", false); }

    /** What a fetch changed (entries by pattern and type), and its "3 entries added, 1 changed ..." text. */
    static final class Changes {
        int added, changed, removed;
        final Set<String> languages = new HashSet<>();

        void compare(String old, String now, String name) {
            java.util.Map<String, String> o = entries(old), n = entries(now);
            int a = 0, r = 0, c = 0;
            for (String k : n.keySet()) {
                if (!o.containsKey(k)) a++;
                else if (!o.get(k).equals(n.get(k))) c++;
            }
            for (String k : o.keySet()) if (!n.containsKey(k)) r++;
            if (a + r + c == 0) return;
            added += a;
            removed += r;
            changed += c;
            String base = name.endsWith(".dic") ? name.substring(0, name.length() - 4) : name;
            int dash = base.indexOf('-');
            languages.add(dash > 0 ? base.substring(0, dash) : base);
        }

        private static java.util.Map<String, String> entries(String text) {
            java.util.Map<String, String> out = new java.util.HashMap<>();
            if (text == null) return out;
            for (String line : text.split("\r?\n|\r")) {
                if (line.isEmpty() || line.startsWith("#")) continue;
                String[] f = line.replace("\uFEFF", "").split("\t", -1);
                if (f.length < 2 || f[0].isEmpty()) continue;
                String type = f.length > 3 ? f[3] : "0";
                out.put(type + "\t" + f[0], line);
            }
            return out;
        }

        /** null when nothing changed. */
        String text() {
            List<String> parts = new ArrayList<>();
            int[] ns = {added, changed, removed};
            String[] verbs = {"added", "changed", "removed"};
            for (int i = 0; i < 3; i++) {
                if (ns[i] == 0) continue;
                parts.add(parts.isEmpty() ? ns[i] + (ns[i] == 1 ? " entry " : " entries ") + verbs[i] : ns[i] + " " + verbs[i]);
            }
            if (parts.isEmpty()) return null;
            String joined = parts.size() == 1 ? parts.get(0)
                    : android.text.TextUtils.join(", ", parts.subList(0, parts.size() - 1)) + " and " + parts.get(parts.size() - 1);
            int n = Math.max(languages.size(), 1);
            return joined + " across " + n + (n == 1 ? " language" : " languages");
        }
    }

    private static void tell(Context c, String body) {
        android.app.NotificationManager nm = (android.app.NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        android.app.Notification.Builder b;
        if (android.os.Build.VERSION.SDK_INT >= 26) {
            if (nm.getNotificationChannel("dictionaries") == null)
                nm.createNotificationChannel(new android.app.NotificationChannel("dictionaries",
                        c.getString(R.string.dictionaries_channel), android.app.NotificationManager.IMPORTANCE_DEFAULT));
            b = new android.app.Notification.Builder(c, "dictionaries");
        } else {
            b = new android.app.Notification.Builder(c);
        }
        b.setSmallIcon(R.drawable.ic_notify).setContentTitle(c.getString(R.string.dictionaries_updated)).setContentText(body)
                .setStyle(new android.app.Notification.BigTextStyle().bigText(body)).setAutoCancel(true);
        nm.notify(4242, b.build());
    }

    static File dir(Context c) { return new File(Data.root(c), "Dictionaries"); }

    private static SharedPreferences state(Context c) {
        return Data.storageContext(c).getSharedPreferences("dictionaries", Context.MODE_PRIVATE);
    }

    /** Checks in the background when it's on and 6 hours have passed (now when force). */
    static void refreshIfDue(Context c) { refresh(c, false); }

    static void refresh(Context c, final boolean force) {
        final Context app = c.getApplicationContext();
        if (!enabled(app)) return;
        long now = System.currentTimeMillis();
        synchronized (LOCK) {
            if (busy) return;
            if (!force && (now - asked < EVERY || now - state(app).getLong("checked", 0) < EVERY)) return;
            busy = true;
            asked = now;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    String text = fetchAll(app).text();
                    if (text != null && notifyChanges(app)) tell(app, text);
                } catch (Exception e) {
                    // offline or refused: tried again at the next check
                } finally {
                    synchronized (LOCK) {
                        busy = false;
                    }
                }
            }
        }, "dictionaries").start();
    }

    /** Download the Latest Dictionaries (the editor): now, switch or not; done(changes, or null when it failed) on
     *  a background thread. */
    interface Done {
        void fetched(Changes changes);
    }

    static void fetchNow(Context c, final Done done) {
        final Context app = c.getApplicationContext();
        new Thread(new Runnable() {
            @Override
            public void run() {
                Changes ch;
                try {
                    ch = fetchAll(app);
                } catch (Exception e) {
                    ch = null;
                }
                done.fetched(ch);
            }
        }, "dictionaries").start();
    }

    private static Changes fetchAll(Context c) throws Exception {
        synchronized (FETCH) {
            Changes changes = new Changes();
            File d = dir(c);
            //noinspection ResultOfMethodCallIgnored
            new File(d, "phonemes").mkdirs();
            String[] src = source(c);
            String contents = "https://api.github.com/repos/" + src[0] + "/contents/";
            String raw = "https://raw.githubusercontent.com/" + src[0] + "/" + src[1] + "/";
            String ref = "?ref=" + java.net.URLEncoder.encode(src[1], "UTF-8");
            Set<String> names = new HashSet<>();
            names.addAll(listing(contents + ref, "", ".dic"));
            names.addAll(listing(contents + "phonemes" + ref, "phonemes/", ".txt"));
            SharedPreferences st = state(c);
            SharedPreferences.Editor ed = st.edit();
            for (String n : names) {
                File f = new File(d, n);
                String[] etag = {f.isFile() ? st.getString("etag:" + n, null) : null};
                byte[] data = get(raw + n, etag[0], etag);
                if (data == null) continue;  // not changed
                if (n.endsWith(".dic")) changes.compare(f.isFile() ? readText(f) : null, new String(data, "UTF-8"), n);
                File part = new File(f.getPath() + ".part");
                OutputStream out = new FileOutputStream(part);
                try {
                    out.write(data);
                } finally {
                    out.close();
                }
                if (!part.renameTo(f)) throw new IOException("couldn't move " + n + " into place");
                ed.putString("etag:" + n, etag[0]);
            }
            // files the repository no longer has
            for (String sub : new String[] {"", "phonemes/"}) {
                File[] here = new File(d, sub).listFiles();
                if (here == null) continue;
                String ext = sub.isEmpty() ? ".dic" : ".txt";
                for (File f : here) {
                    String n = sub + f.getName();
                    if (f.isFile() && f.getName().endsWith(ext) && !names.contains(n)) {
                        if (ext.equals(".dic")) changes.compare(readText(f), null, f.getName());
                        //noinspection ResultOfMethodCallIgnored
                        f.delete();
                        ed.remove("etag:" + n);
                    }
                }
            }
            ed.putLong("checked", System.currentTimeMillis()).apply();
            DictionaryStore.tidy(c);
            return changes;
        }
    }

    /** The repository's files in a folder with this extension, as prefix + name; a folder it hasn't got: none. */
    private static List<String> listing(String url, String prefix, String ext) throws Exception {
        List<String> out = new ArrayList<>();
        byte[] body;
        try {
            body = get(url, null, null);
        } catch (FileNotFoundException e) {
            if (prefix.isEmpty()) throw e;
            return out;  // no phonemes folder
        }
        JSONArray list = new JSONArray(new String(body, "UTF-8"));
        for (int i = 0; i < list.length(); i++) {
            JSONObject o = list.optJSONObject(i);
            if (o == null || !"file".equals(o.optString("type", "file"))) continue;
            String n = o.optString("name", "");
            if (n.endsWith(ext) && !n.contains("/") && !n.startsWith(".")) out.add(prefix + n);
        }
        return out;
    }

    static void writeText(File f, String text) throws IOException {
        File part = new File(f.getPath() + ".part");
        OutputStream out = new FileOutputStream(part);
        try {
            out.write(text.getBytes("UTF-8"));
        } finally {
            out.close();
        }
        if (!part.renameTo(f)) throw new IOException("couldn't write " + f.getName());
    }

    static String readText(File f) {
        try {
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            try {
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int k;
                while ((k = in.read(buf)) > 0) b.write(buf, 0, k);
                return new String(b.toByteArray(), "UTF-8");
            } finally {
                in.close();
            }
        } catch (IOException e) {
            return null;
        }
    }

    /** The body, or null when unchanged (304 for etag); etagOut[0] gets the new ETag. */
    private static byte[] get(String url, String etag, String[] etagOut) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestProperty("User-Agent", Source.UA);
        c.setRequestProperty("Accept", url.startsWith("https://api.github.com/") ? "application/vnd.github+json" : "*/*");
        if (etag != null) c.setRequestProperty("If-None-Match", etag);
        c.setConnectTimeout(30000);
        c.setReadTimeout(60000);
        try {
            int code = c.getResponseCode();
            if (code == 304) return null;
            if (code == 404) throw new FileNotFoundException(url);
            if (code != 200) throw new IOException("HTTP " + code + " for " + url);
            if (etagOut != null) etagOut[0] = c.getHeaderField("ETag");
            InputStream in = c.getInputStream();
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
            return b.toByteArray();
        } finally {
            c.disconnect();
        }
    }
}
