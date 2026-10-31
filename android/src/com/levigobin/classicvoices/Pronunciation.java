package com.levigobin.classicvoices;

import android.content.Context;
import android.util.Base64;
import android.util.Xml;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Apple's later pronunciation packs (the "Newer pronunciation rules" switch):
 * one VoiceResources MobileAsset per language, as iOS 12 used them: a user
 * dictionary the engine loads (vxs_set_rules_pack) and rule sets Rules runs
 * after the voice's own. Downloaded from Apple, all languages at once (about
 * 5 MB), into Pronunciation/<language>.
 */
final class Pronunciation {
    static final String JOB = "pronunciation";
    private static final String CATALOG = "https://mesu.apple.com/assets/com_apple_MobileAsset_VoiceServices_VoiceResources/"
            + "com_apple_MobileAsset_VoiceServices_VoiceResources.xml";
    private static final long COMPATIBILITY = 8;  // the packs iOS 12 used
    private static final String[] FILES = {"vocalizer-user-dict.dat", "vocalizer-user-rules.txt",
            "vocalizer-override-rules.txt"};
    private static final String DONE = ".complete";

    private Pronunciation() {}

    static boolean enabled(Context c) { return Data.prefs(c).getBoolean("newer_rules", false); }

    static File dir(Context c) { return new File(Data.root(c), "Pronunciation"); }

    static boolean downloaded(Context c) { return new File(dir(c), DONE).isFile(); }

    /** The pack folder for a voice when the switch is on and its language has one, else null. */
    static String packFor(Context c, VoiceInfo v) {
        if (v == null || !enabled(c)) return null;
        File d = new File(dir(c), v.language);
        return new File(d, "vocalizer-user-rules.txt").isFile() ? d.getAbsolutePath() : null;
    }

    /** Queues the download when the switch is on and the packs aren't here yet. */
    static void startIfNeeded(Context c) {
        if (!enabled(c) || downloaded(c) || ImportService.pending(JOB, JOB)) return;
        ImportService.enqueue(c, new ImportService.Job(JOB, JOB, c.getString(R.string.newer_rules_job)));
    }

    static void download(Context c, Progress p) throws IOException {
        p.step(c.getString(R.string.newer_rules_catalog));
        List<Map<String, Object>> assets = new ArrayList<>();
        HttpURLConnection conn = Source.open(CATALOG, -1, -1);
        try {
            Object root = parsePlist(conn.getInputStream());
            Object list = root instanceof Map ? ((Map<?, ?>) root).get("Assets") : null;
            if (!(list instanceof List)) throw new IOException(c.getString(R.string.newer_rules_bad_catalog));
            for (Object o : (List<?>) list) {
                if (!(o instanceof Map)) continue;
                @SuppressWarnings("unchecked") Map<String, Object> a = (Map<String, Object>) o;
                Object compat = a.get("_CompatibilityVersion"), langs = a.get("Languages");
                if (compat instanceof Long && (Long) compat == COMPATIBILITY && langs instanceof List
                        && !((List<?>) langs).isEmpty() && a.get("__BaseURL") != null && a.get("__RelativePath") != null)
                    assets.add(a);
            }
        } finally {
            conn.disconnect();
        }
        if (assets.isEmpty()) throw new IOException(c.getString(R.string.newer_rules_bad_catalog));
        File root = dir(c);
        //noinspection ResultOfMethodCallIgnored
        root.mkdirs();
        for (int i = 0; i < assets.size(); i++) {
            if (p.cancelled()) throw new Progress.Cancelled();
            Map<String, Object> a = assets.get(i);
            String lang = String.valueOf(((List<?>) a.get("Languages")).get(0));
            if (lang.isEmpty() || lang.contains("/") || lang.startsWith(".")) continue;
            p.step(c.getString(R.string.newer_rules_downloading, lang));
            p.fraction(i / (double) assets.size());
            Object m = a.get("_Measurement");
            fetch(a.get("__BaseURL") + String.valueOf(a.get("__RelativePath")), m instanceof byte[] ? (byte[]) m : null,
                    new File(root, lang));
        }
        new FileOutputStream(new File(root, DONE)).close();
        p.fraction(1);
        p.step(c.getString(R.string.newer_rules_done));
    }

    /** One language's zip, checked against its SHA-1, into dest (replaced only when it's whole). */
    private static void fetch(String url, byte[] sha1, File dest) throws IOException {
        File tmp = new File(dest.getPath() + ".downloading");
        Firmware.deleteTree(tmp);
        //noinspection ResultOfMethodCallIgnored
        tmp.mkdirs();
        try {
            HttpURLConnection conn = Source.open(Downloads.https(url), -1, -1);
            final MessageDigest md;
            try {
                md = MessageDigest.getInstance("SHA-1");
            } catch (NoSuchAlgorithmException e) {
                throw new IOException(e.toString());
            }
            try {
                InputStream raw = new FilterInputStream(conn.getInputStream()) {
                    @Override
                    public int read() throws IOException {
                        int r = super.read();
                        if (r >= 0) md.update((byte) r);
                        return r;
                    }

                    @Override
                    public int read(byte[] b, int o, int n) throws IOException {
                        int r = super.read(b, o, n);
                        if (r > 0) md.update(b, o, r);
                        return r;
                    }
                };
                ZipInputStream z = new ZipInputStream(raw);
                ZipEntry e;
                byte[] buf = new byte[1 << 16];
                while ((e = z.getNextEntry()) != null) {
                    String name = e.getName().replace('\\', '/');
                    String base = name.substring(name.lastIndexOf('/') + 1);
                    boolean want = false;
                    for (String f : FILES) want |= f.equals(base);
                    if (e.isDirectory() || !want || !name.contains("AssetData/")) continue;
                    OutputStream out = new FileOutputStream(new File(tmp, base));
                    try {
                        int n;
                        while ((n = z.read(buf)) > 0) out.write(buf, 0, n);
                    } finally {
                        out.close();
                    }
                }
                while (raw.read(buf) > 0) { /* the rest of the file, for the checksum */ }
            } finally {
                conn.disconnect();
            }
            if (sha1 != null && !MessageDigest.isEqual(sha1, md.digest()))
                throw new IOException("the download was damaged (SHA-1 mismatch)");
            if (!new File(tmp, "vocalizer-user-rules.txt").isFile()) throw new IOException("the archive had no rules in it");
            Firmware.deleteTree(dest);
            if (!tmp.renameTo(dest)) throw new IOException("couldn't move the rules into place");
        } finally {
            Firmware.deleteTree(tmp);
        }
    }

    // ---------------------------------------------------------------- XML plist

    private static Object parsePlist(InputStream in) throws IOException {
        try {
            XmlPullParser x = Xml.newPullParser();
            x.setInput(in, "UTF-8");
            int t;
            while ((t = x.next()) != XmlPullParser.END_DOCUMENT) {
                if (t == XmlPullParser.START_TAG && !x.getName().equals("plist")) return value(x);
            }
            return null;
        } catch (XmlPullParserException e) {
            throw new IOException("bad catalog: " + e.getMessage());
        }
    }

    /** The value whose start tag the parser is on; leaves it on the matching end tag. */
    private static Object value(XmlPullParser x) throws IOException, XmlPullParserException {
        String tag = x.getName();
        switch (tag) {
            case "dict": {
                Map<String, Object> m = new HashMap<>();
                String key = null;
                int t;
                while ((t = x.next()) != XmlPullParser.END_TAG) {
                    if (t != XmlPullParser.START_TAG) continue;
                    if (x.getName().equals("key")) key = x.nextText();
                    else {
                        Object v = value(x);
                        if (key != null) m.put(key, v);
                        key = null;
                    }
                }
                return m;
            }
            case "array": {
                List<Object> l = new ArrayList<>();
                int t;
                while ((t = x.next()) != XmlPullParser.END_TAG) if (t == XmlPullParser.START_TAG) l.add(value(x));
                return l;
            }
            case "true":
            case "false":
                x.next();
                return tag.equals("true");
            case "integer":
                try {
                    return Long.parseLong(x.nextText().trim());
                } catch (NumberFormatException e) {
                    return null;
                }
            case "data":
                return Base64.decode(x.nextText().replaceAll("\\s", ""), Base64.DEFAULT);
            default:
                return x.nextText();  // string, date, real
        }
    }
}
