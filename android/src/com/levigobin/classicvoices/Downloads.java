package com.levigobin.classicvoices;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

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
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.GZIPInputStream;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Voices from Apple's own servers, once the matching engine is imported:
 * enhanced iOS 5 and 6 voices (MobileAsset zips, EnhancedCatalog.json) and
 * the Mac OS X Lion, Mountain Lion and Mavericks voices (software-update
 * packages, MacVoiceCatalog.json). Both are streamed straight into place: a
 * package is xar -> Payload (gzip) -> cpio -> <Name>.SpeechVoice whose
 * PCMWave is a tar with ROT13 member names (the NVDA add-on's download.py).
 */
final class Downloads {
    static final class Item {
        final String kind;   // "enhanced" or "mac"
        final String label;  // "Samantha, enhanced (iOS 6, en-US)"
        final String build;  // ios5 / ios6, or the Mac era
        final String name;   // language for enhanced, voice name for Mac
        final String locale, url, sha1;
        final long size;

        Item(String kind, String label, String build, String name, String locale, String url, long size, String sha1) {
            this.kind = kind;
            this.label = label;
            this.build = build;
            this.name = name;
            this.locale = locale;
            this.url = url;
            this.size = size;
            this.sha1 = sha1;
        }

        String id() { return kind + ":" + build + ":" + name; }
    }

    private static JSONArray enhanced, mac;

    private static synchronized void load(Context c) throws IOException {
        if (enhanced != null) return;
        try {
            enhanced = new JSONArray(asset(c, "EnhancedCatalog.json"));
            mac = new JSONObject(asset(c, "MacVoiceCatalog.json")).getJSONArray("voices");
        } catch (JSONException e) {
            throw new IOException("bad catalogue: " + e.getMessage());
        }
    }

    private static String asset(Context c, String name) throws IOException {
        InputStream in = c.getAssets().open(name);
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        in.close();
        return new String(b.toByteArray(), "UTF-8");
    }

    private static final String[][] ERAS = {{"mac107", "Lion", "ios6"}, {"mac108", "Mountain Lion", "ios6"},
            {"mac109", "Mavericks", "ios7"}};

    /** What can be downloaded now (engine present, not installed yet). */
    static List<Item> available(Context c) throws IOException {
        load(c);
        List<Item> out = new ArrayList<>();
        File dl = Data.downloadsDir(c), voices = Data.voicesDir(c);
        try {
            for (int i = 0; i < enhanced.length(); i++) {
                JSONObject e = enhanced.getJSONObject(i);
                String build = e.getString("build"), lang = e.getString("language");
                if (!Data.hasEngine(c, build)) continue;
                if (new File(dl, build + "/" + lang + "/broker.hdr.asset").isFile()) continue;
                // the compact voice of the same language names the voice
                String voice = voiceName(new File(voices, build + "/TTSResources/" + lang + "/broker.hdr"));
                String label = (voice != null ? voice + ", enhanced" : "Enhanced voice") + " ("
                        + (build.equals("ios5") ? "iOS 5" : "iOS 6") + ", " + lang + ")";
                out.add(new Item("enhanced", label, build, lang, lang, https(e.getString("url")), e.getLong("size"),
                        e.optString("sha1", null)));
            }
            for (int i = 0; i < mac.length(); i++) {
                JSONObject v = mac.getJSONObject(i);
                String era = v.getString("era");
                String[] info = null;
                for (String[] x : ERAS) if (x[0].equals(era)) info = x;
                if (info == null || !Data.hasEngine(c, info[2])) continue;
                if (!new File(voices, era + "/common").isDirectory()) continue;
                if (new File(dl, era + "/" + v.getString("name") + "/broker.hdr").isFile()) continue;
                String label = v.getString("name") + " (Mac OS X " + info[1] + ", " + v.optString("locale") + ")";
                out.add(new Item("mac", label, era, v.getString("name"), v.optString("locale"), v.getString("url"),
                        v.getLong("size"), null));
            }
        } catch (JSONException e) {
            throw new IOException("bad catalogue: " + e.getMessage());
        }
        return out;
    }

    /** Apple serves the enhanced voices over HTTPS from secure-appldnld (same files, same SHA-1). */
    static String https(String url) {
        return url.startsWith("http://appldnld.apple.com/") ? "https://secure-appldnld.apple.com/" + url.substring(26) : url;
    }

    static String voiceName(File broker) {
        try {
            byte[] b = new byte[65536];
            java.io.FileInputStream in = new java.io.FileInputStream(broker);
            int n = in.read(b);
            in.close();
            String s = new String(b, 0, Math.max(0, n), "UTF-8");
            if (s.startsWith("defaultvoice:")) {
                int e = s.indexOf('\n');
                String v = s.substring(13, e > 0 ? e : s.length()).trim();
                return v.isEmpty() ? null : Character.toUpperCase(v.charAt(0)) + v.substring(1);
            }
            Matcher m = Pattern.compile("<voice>([^<]+)</voice>").matcher(s);
            if (m.find()) {
                String v = m.group(1).trim();
                return Character.toUpperCase(v.charAt(0)) + v.substring(1);
            }
        } catch (IOException e) {
            // no name
        }
        return null;
    }

    // ---------------------------------------------------------------- streams

    private static final class Counting extends FilterInputStream {
        long got;
        final long total;
        final Progress p;
        long last;

        Counting(InputStream in, long total, Progress p) {
            super(in);
            this.total = total;
            this.p = p;
        }

        @Override
        public int read() throws IOException {
            byte[] one = new byte[1];
            return read(one, 0, 1) < 0 ? -1 : one[0] & 0xFF;
        }

        @Override
        public int read(byte[] b, int o, int n) throws IOException {
            if (p.cancelled()) throw new Progress.Cancelled();
            int r = super.read(b, o, n);
            if (r > 0) {
                got += r;
                long now = System.currentTimeMillis();
                if (now - last > 500 && total > 0) {
                    last = now;
                    p.fraction(got / (double) total);
                }
            }
            return r;
        }
    }

    private static final class Digesting extends FilterInputStream {
        final MessageDigest md;

        Digesting(InputStream in) throws IOException {
            super(in);
            try {
                md = MessageDigest.getInstance("SHA-1");
            } catch (NoSuchAlgorithmException e) {
                throw new IOException(e.toString());
            }
        }

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

        String hex() {
            StringBuilder s = new StringBuilder();
            for (byte x : md.digest()) s.append(String.format("%02x", x & 0xFF));
            return s.toString();
        }
    }

    /** At most n bytes of the stream, never closing it. */
    private static final class Bounded extends InputStream {
        final InputStream in;
        long left;

        Bounded(InputStream in, long n) {
            this.in = in;
            left = n;
        }

        @Override
        public int read() throws IOException {
            if (left <= 0) return -1;
            int r = in.read();
            if (r >= 0) left--;
            return r;
        }

        @Override
        public int read(byte[] b, int o, int n) throws IOException {
            if (left <= 0) return -1;
            int r = in.read(b, o, (int) Math.min(n, left));
            if (r > 0) left -= r;
            return r;
        }

        void drain() throws IOException {
            Source.skipFully(in, left);
            left = 0;
        }
    }

    private static boolean safeName(String n) {
        return !n.isEmpty() && !n.contains("/") && !n.contains("\\") && !n.equals(".") && !n.equals("..")
                && !n.startsWith(".");
    }

    private static void write(InputStream in, long n, File dest) throws IOException {
        OutputStream out = new FileOutputStream(dest);
        try {
            byte[] buf = new byte[1 << 16];
            while (n > 0) {
                int r = in.read(buf, 0, (int) Math.min(buf.length, n));
                if (r < 0) throw new IOException("the download ended early");
                out.write(buf, 0, r);
                n -= r;
            }
        } finally {
            out.close();
        }
    }

    // ---------------------------------------------------------------- enhanced

    static void downloadEnhanced(Context c, Item it, Progress p) throws IOException {
        File tmp = new File(Data.downloadsDir(c), it.build + "/" + it.name + ".downloading");
        try {
            downloadEnhanced(c, it, tmp, p);
        } finally {
            Firmware.deleteTree(tmp);  // stopped or failed: nothing is left behind
        }
    }

    private static void downloadEnhanced(Context c, Item it, File tmp, Progress p) throws IOException {
        File dest = new File(Data.downloadsDir(c), it.build + "/" + it.name);
        Firmware.deleteTree(tmp);
        //noinspection ResultOfMethodCallIgnored
        tmp.mkdirs();
        p.step("Downloading " + it.label + " (" + (it.size >> 20) + " MB)");
        HttpURLConnection conn = Source.open(it.url, -1, -1);
        try {
            Digesting dig = new Digesting(new Counting(conn.getInputStream(), it.size, p));
            ZipInputStream z = new ZipInputStream(dig);
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                String[] parts = e.getName().replace('\\', '/').split("/");
                boolean asset = false;
                for (String part : parts) asset |= part.equals("AssetData");
                String name = parts[parts.length - 1];
                if (e.isDirectory() || !asset || !safeName(name)) continue;
                OutputStream out = new FileOutputStream(new File(tmp, name));
                try {
                    int n;
                    while ((n = z.read(buf)) > 0) out.write(buf, 0, n);
                } finally {
                    out.close();
                }
            }
            while (dig.read(buf) > 0) { /* the rest of the file, for the checksum */ }
            if (it.sha1 != null && !it.sha1.equalsIgnoreCase(dig.hex())) throw new IOException("the download was damaged (SHA-1 mismatch)");
        } finally {
            conn.disconnect();
        }
        if (!new File(tmp, "broker.hdr.asset").isFile()) {
            Firmware.deleteTree(tmp);
            throw new IOException("the archive had no voice in it");
        }
        Firmware.deleteTree(dest);
        if (!tmp.renameTo(dest)) throw new IOException("couldn't move the voice into place");
        p.step(it.label + " installed");
    }

    // ---------------------------------------------------------------- Mac OS X

    private static final Pattern PCMWAVE = Pattern.compile("(?:^|/)([^/]+)\\.SpeechVoice/Contents/Resources/PCMWave$");

    static List<String> downloadMac(Context c, Item it, Progress p) throws IOException {
        File eraDir = new File(Data.downloadsDir(c), it.build);
        //noinspection ResultOfMethodCallIgnored
        eraDir.mkdirs();
        p.step("Downloading " + it.label + " (" + (it.size >> 20) + " MB)");
        List<String> installed = new ArrayList<>();
        HttpURLConnection conn = Source.open(it.url, -1, -1);
        try {
            InputStream in = new Counting(conn.getInputStream(), it.size, p);
            // xar: header, zlib table of contents, then the heap
            byte[] head = new byte[28];
            Source.readFully(in, head, 0, 28);
            if (head[0] != 'x' || head[1] != 'a' || head[2] != 'r' || head[3] != '!') throw new IOException("not a flat package");
            int hsize = Source.be16(head, 4);
            long tocC = Source.be64(head, 8);
            Source.skipFully(in, hsize - 28);
            byte[] tocz = new byte[(int) tocC];
            Source.readFully(in, tocz, 0, tocz.length);
            String toc = new String(inflateAll(tocz), "UTF-8");
            long off = -1, len = -1;
            Matcher fm = Pattern.compile("<file\\b.*?</file>", Pattern.DOTALL).matcher(toc);
            while (fm.find()) {
                String block = fm.group();
                if (!block.contains("<name>Payload</name>")) continue;
                Matcher mo = Pattern.compile("<offset>(\\d+)</offset>").matcher(block);
                Matcher ml = Pattern.compile("<length>(\\d+)</length>").matcher(block);
                if (mo.find() && ml.find()) {
                    off = Long.parseLong(mo.group(1));
                    len = Long.parseLong(ml.group(1));
                }
            }
            if (off < 0) throw new IOException("the package has no Payload");
            Source.skipFully(in, off);  // the heap starts right after the TOC
            InputStream payload = new GZIPInputStream(new Bounded(in, len), 1 << 16);
            cpio(payload, eraDir, installed, p);
        } finally {
            conn.disconnect();
        }
        if (installed.isEmpty()) throw new IOException("no voices found inside the package");
        for (String folder : installed) {
            OutputStream o = new FileOutputStream(new File(new File(eraDir, folder), "locale"));
            o.write(it.locale.getBytes("UTF-8"));
            o.close();
        }
        p.step(join(installed) + " installed");
        return installed;
    }

    private static String join(List<String> l) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < l.size(); i++) s.append(i == 0 ? "" : " and ").append(l.get(i));
        return s.toString();
    }

    private static byte[] inflateAll(byte[] z) throws IOException {
        InputStream in = new InflaterInputStream(new java.io.ByteArrayInputStream(z), new Inflater());
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toByteArray();
    }

    private static void cpio(InputStream s, File eraDir, List<String> installed, Progress p) throws IOException {
        byte[] magic = new byte[6];
        while (true) {
            Source.readFully(s, magic, 0, 6);
            String m = new String(magic, "ISO-8859-1");
            long size, pad;
            String name;
            if (m.equals("070707")) {
                byte[] h = new byte[70];
                Source.readFully(s, h, 0, 70);
                int namesize = Integer.parseInt(new String(h, 53, 6, "ISO-8859-1"), 8);
                size = Long.parseLong(new String(h, 59, 11, "ISO-8859-1"), 8);
                byte[] nb = new byte[namesize];
                Source.readFully(s, nb, 0, namesize);
                name = new String(nb, 0, Math.max(0, namesize - 1), "UTF-8");
                pad = 0;
            } else if (m.equals("070701") || m.equals("070702")) {
                byte[] h = new byte[104];
                Source.readFully(s, h, 0, 104);
                size = Long.parseLong(new String(h, 48, 8, "ISO-8859-1"), 16);
                int namesize = Integer.parseInt(new String(h, 88, 8, "ISO-8859-1"), 16);
                byte[] nb = new byte[namesize];
                Source.readFully(s, nb, 0, namesize);
                name = new String(nb, 0, Math.max(0, namesize - 1), "UTF-8");
                Source.skipFully(s, (4 - ((110 + namesize) % 4)) % 4);
                pad = (4 - (size % 4)) % 4;
            } else {
                throw new IOException("not a cpio archive");
            }
            if (name.equals("TRAILER!!!")) return;
            Matcher w = PCMWAVE.matcher(name);
            if (w.find() && safeName(w.group(1))) {
                String folder = w.group(1);
                p.step("Unpacking " + folder);
                File dest = new File(eraDir, folder + ".unpacking");
                Firmware.deleteTree(dest);
                //noinspection ResultOfMethodCallIgnored
                dest.mkdirs();
                Bounded member = new Bounded(s, size);
                try {
                    untarRot13(member, dest);
                    member.drain();
                } catch (IOException | RuntimeException x) {
                    Firmware.deleteTree(dest);  // stopped or failed: nothing is left behind
                    throw x;
                }
                File fin = new File(eraDir, folder);
                if (new File(dest, "broker.hdr").isFile()) {
                    Firmware.deleteTree(fin);
                    if (!dest.renameTo(fin)) throw new IOException("couldn't move " + folder + " into place");
                    installed.add(folder);
                } else {
                    Firmware.deleteTree(dest);
                }
            } else {
                Source.skipFully(s, size);
            }
            Source.skipFully(s, pad);
        }
    }

    private static String rot13(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (char c : s.toCharArray()) {
            if (c >= 'a' && c <= 'z') c = (char) ('a' + (c - 'a' + 13) % 26);
            else if (c >= 'A' && c <= 'Z') c = (char) ('A' + (c - 'A' + 13) % 26);
            b.append(c);
        }
        return b.toString();
    }

    /** A tar stream whose member names are ROT13: regular files into dest. */
    private static void untarRot13(InputStream in, File dest) throws IOException {
        byte[] h = new byte[512];
        String longName = null;
        while (true) {
            int got = 0;
            while (got < 512) {
                int r = in.read(h, got, 512 - got);
                if (r < 0) return;
                got += r;
            }
            boolean zero = true;
            for (byte x : h) if (x != 0) { zero = false; break; }
            if (zero) return;
            String name = cstr(h, 0, 100);
            String prefix = cstr(h, 345, 155);
            if (!prefix.isEmpty() && new String(h, 257, 5, "ISO-8859-1").equals("ustar")) name = prefix + "/" + name;
            long size = parseOctal(h, 124, 12);
            char type = (char) h[156];
            long padded = (size + 511) / 512 * 512;
            if (type == 'L') {  // GNU long name
                byte[] nb = new byte[(int) padded];
                Source.readFully(in, nb, 0, nb.length);
                longName = cstr(nb, 0, (int) size);
                continue;
            }
            if (longName != null) {
                name = longName;
                longName = null;
            }
            if (type == '0' || type == 0) {
                String base = rot13(name.substring(name.lastIndexOf('/') + 1));
                if (safeName(base)) {
                    write(in, size, new File(dest, base));
                    Source.skipFully(in, padded - size);
                    continue;
                }
            }
            Source.skipFully(in, padded);
        }
    }

    private static String cstr(byte[] b, int o, int n) throws IOException {
        int e = o;
        while (e < o + n && b[e] != 0) e++;
        return new String(b, o, e - o, "UTF-8");
    }

    private static long parseOctal(byte[] b, int o, int n) {
        if ((b[o] & 0x80) != 0) {  // base-256 size
            long v = 0;
            for (int i = 1; i < n; i++) v = (v << 8) | (b[o + i] & 0xFF);
            return v;
        }
        long v = 0;
        for (int i = o; i < o + n; i++) {
            if (b[i] == 0 || b[i] == ' ') {
                if (v != 0) break;
                continue;
            }
            v = v * 8 + (b[i] - '0');
        }
        return v;
    }
}
