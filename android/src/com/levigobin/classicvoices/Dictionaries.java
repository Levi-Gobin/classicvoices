package com.levigobin.classicvoices;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.HashSet;
import java.util.Set;

/**
 * The community pronunciation dictionaries (github.com/nicopn/classicvoices-dicts; the "Community pronunciation
 * dictionaries" switch, off until turned on): every .dic file of the repository, fetched into
 * <data>/Dictionaries, where the engine reads a voice's own (ports/host/cv_dict.h). Checked when the switch is
 * turned on, when the app opens and when the voices speak, at most every 6 hours; a changed file is used from the
 * next utterance on, and a file the repository no longer has is removed. The person's own entries
 * (Dictionaries/Mine, ports/host/cv_dict.h) are used even with the switch off; nothing here touches that folder.
 */
final class Dictionaries {
    private static final String CONTENTS = "https://api.github.com/repos/nicopn/classicvoices-dicts/contents/";
    private static final String RAW = "https://raw.githubusercontent.com/nicopn/classicvoices-dicts/main/";
    private static final long EVERY = 6 * 3600 * 1000L;
    private static final Object LOCK = new Object();
    private static boolean busy;
    private static long asked;  // this process: when a check last began

    private Dictionaries() {}

    static boolean enabled(Context c) { return Data.prefs(c).getBoolean("community_dictionaries", false); }

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
                    fetchAll(app);
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

    private static void fetchAll(Context c) throws Exception {
        File d = dir(c);
        //noinspection ResultOfMethodCallIgnored
        d.mkdirs();
        JSONArray list = new JSONArray(new String(get(CONTENTS, null, null), "UTF-8"));
        Set<String> names = new HashSet<>();
        for (int i = 0; i < list.length(); i++) {
            JSONObject o = list.optJSONObject(i);
            String n = o != null ? o.optString("name", "") : "";
            if (n.endsWith(".dic") && !n.contains("/") && !n.startsWith(".")) names.add(n);
        }
        SharedPreferences st = state(c);
        SharedPreferences.Editor ed = st.edit();
        for (String n : names) {
            File f = new File(d, n);
            String[] etag = {f.isFile() ? st.getString("etag:" + n, null) : null};
            byte[] data = get(RAW + n, etag[0], etag);
            if (data == null) continue;  // not changed
            File part = new File(d, n + ".part");
            OutputStream out = new FileOutputStream(part);
            try {
                out.write(data);
            } finally {
                out.close();
            }
            if (!part.renameTo(f)) throw new IOException("couldn't move " + n + " into place");
            ed.putString("etag:" + n, etag[0]);
        }
        File[] here = d.listFiles();
        if (here != null) {
            for (File f : here) {
                if (f.isFile() && f.getName().endsWith(".dic") && !names.contains(f.getName())) {
                    //noinspection ResultOfMethodCallIgnored
                    f.delete();
                    ed.remove("etag:" + f.getName());
                }
            }
        }
        ed.putLong("checked", System.currentTimeMillis()).apply();
    }

    /** The body, or null when unchanged (304 for etag); etagOut[0] gets the new ETag. */
    private static byte[] get(String url, String etag, String[] etagOut) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestProperty("User-Agent", Source.UA);
        c.setRequestProperty("Accept", url.startsWith(CONTENTS) ? "application/vnd.github+json" : "*/*");
        if (etag != null) c.setRequestProperty("If-None-Match", etag);
        c.setConnectTimeout(30000);
        c.setReadTimeout(60000);
        try {
            int code = c.getResponseCode();
            if (code == 304) return null;
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
