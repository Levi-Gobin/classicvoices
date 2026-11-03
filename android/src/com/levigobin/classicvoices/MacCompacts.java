package com.levigobin.classicvoices;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * The compact voices Lion, Mountain Lion and Mavericks installed with the
 * system (Agata, Daniel, Samantha ...). Apple's voice packages have only the
 * premium voices, so these are read out of each release's installer:
 * MacCompacts.json (tools/mac_compacts.py) says which 25 to 40 MB of it hold
 * them and how to unpack that, with a SHA-256 per voice. The steps are the
 * NVDA add-on's (nvda/.../maccompacts.py), which explains them. They go into
 * Downloads/&lt;era&gt;/&lt;Name&gt;Compact/ with their language beside them.
 */
final class MacCompacts {
    private MacCompacts() {}

    static final String MARKER = ".compacts";
    private static JSONObject recipe;

    static synchronized JSONObject recipe(Context c) throws IOException {
        if (recipe == null) {
            try (InputStream in = c.getAssets().open("MacCompacts.json")) {
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
                recipe = new JSONObject(new String(b.toByteArray(), "UTF-8")).getJSONObject("releases");
            } catch (JSONException e) {
                throw new IOException("bad recipe: " + e.getMessage());
            }
        }
        return recipe;
    }

    /** folder -> locale of a release's compacts. */
    static Map<String, String> voices(Context c, String era) throws IOException {
        Map<String, String> out = new TreeMap<>();
        try {
            JSONObject r = recipe(c).optJSONObject(era);
            if (r == null) return out;
            JSONObject v = r.getJSONObject("voices");
            for (java.util.Iterator<String> it = v.keys(); it.hasNext(); ) {
                String k = it.next();
                out.put(k, v.getJSONObject(k).getString("locale"));
            }
            JSONArray f = r.getJSONArray("files");
            for (int i = 0; i < f.length(); i++) out.put(f.getJSONObject(i).getString("voice"), f.getJSONObject(i).getString("locale"));
        } catch (JSONException e) {
            throw new IOException("bad recipe: " + e.getMessage());
        }
        return out;
    }

    static long downloadSize(Context c, String era) throws IOException {
        long n = 0;
        try {
            JSONArray f = recipe(c).getJSONObject(era).getJSONArray("fetch");
            for (int i = 0; i < f.length(); i++) n += f.getJSONArray(i).getLong(1);
        } catch (JSONException e) {
            throw new IOException("bad recipe: " + e.getMessage());
        }
        return n;
    }

    /** The release's compacts (in the languages wanted) that aren't here. */
    static List<String> missing(Context c, String era, java.util.Set<String> wanted) throws IOException {
        List<String> out = new ArrayList<>();
        File dl = new File(Data.downloadsDir(c), era);
        for (Map.Entry<String, String> e : voices(c, era).entrySet()) {
            if (!Languages.isWanted(e.getValue(), wanted)) continue;
            if (!new File(dl, e.getKey() + "/broker.hdr").isFile()) out.add(e.getKey());
        }
        return out;
    }

    /** Fetched here once: voices removed since stay removed. */
    static boolean fetchedBefore(Context c, String era) {
        return new File(Data.downloadsDir(c), era + "/" + MARKER).isFile();
    }

    /**
     * Queues the compacts of every release whose firmware is here, as the Mac
     * had them with the system; once (voices removed stay removed).
     */
    static void startIfNeeded(Context c) {
        try {
            for (Downloads.Item it : Downloads.available(c)) {
                if (!it.kind.equals("compacts") || fetchedBefore(c, it.build)) continue;
                if (ImportService.pending("download", it.id())) continue;
                ImportService.enqueue(c, new ImportService.Job("download", it.id(), it.label));
            }
        } catch (IOException ignored) {
            // the list couldn't be read: the voices can still be downloaded by hand
        }
    }

    // ---------------------------------------------------------------- fetching

    private static final class Fetched {
        final long[][] spans;  // offset, length
        final byte[][] data;
        Fetched(long[][] spans, byte[][] data) { this.spans = spans; this.data = data; }

        byte[] read(long off, int n) throws IOException {
            byte[] out = new byte[n];
            int got = 0;
            for (int i = 0; i < spans.length; i++) {
                long a = Math.max(off, spans[i][0]), b = Math.min(off + n, spans[i][0] + spans[i][1]);
                if (a < b) {
                    System.arraycopy(data[i], (int) (a - spans[i][0]), out, (int) (a - off), (int) (b - a));
                    got += (int) (b - a);
                }
            }
            if (got != n) throw new IOException("the recipe reads outside what was downloaded");
            return out;
        }
    }

    private static final class Streams {
        final Fetched fetched;
        final JSONArray specs;
        final Map<Integer, Object[]> cache = new HashMap<>();  // stream -> {chunk index, bytes}
        Streams(Fetched f, JSONArray specs) { this.fetched = f; this.specs = specs; }

        byte[] read(int stream, long off, int n) throws IOException, JSONException {
            if (stream < 0) return fetched.read(off, n);
            JSONObject spec = specs.getJSONObject(stream);
            int of = spec.getInt("of");
            byte[] out = new byte[n];
            int got = 0;
            long end = off + n;
            JSONArray map = spec.optJSONArray("map");
            if (map != null) {
                for (int i = 0; i < map.length(); i++) {
                    JSONArray p = map.getJSONArray(i);
                    long v = p.getLong(0), len = p.getLong(1), at = p.getLong(2);
                    long a = Math.max(off, v), b = Math.min(end, v + len);
                    if (a >= b) continue;
                    byte[] d = read(of, at + (a - v), (int) (b - a));
                    System.arraycopy(d, 0, out, (int) (a - off), d.length);
                    got += d.length;
                }
            } else {
                JSONArray chunks = spec.getJSONArray("udif");
                for (int i = 0; i < chunks.length(); i++) {
                    JSONArray ch = chunks.getJSONArray(i);
                    long o = ch.getLong(1), len = ch.getLong(2);
                    long a = Math.max(off, o), b = Math.min(end, o + len);
                    if (a >= b) continue;
                    byte[] d = chunk(stream, i, ch, of);
                    System.arraycopy(d, (int) (a - o), out, (int) (a - off), (int) (b - a));
                    got += (int) (b - a);
                }
            }
            if (got != n) throw new IOException("the recipe doesn't cover what it reads");
            return out;
        }

        private byte[] chunk(int stream, int i, JSONArray ch, int of) throws IOException, JSONException {
            Object[] hit = cache.get(stream);
            if (hit != null && (Integer) hit[0] == i) return (byte[]) hit[1];
            long kind = ch.getLong(0);
            int len = (int) ch.getLong(2);
            byte[] d;
            if (kind == 0 || kind == 2) d = new byte[len];
            else if (kind == 1) d = read(of, ch.getLong(3), (int) ch.getLong(4));
            else if (kind == 0x80000005L) d = inflate(read(of, ch.getLong(3), (int) ch.getLong(4)), false, null, len);
            else throw new IOException("disk image chunk type " + Long.toHexString(kind));
            if (d.length < len) d = java.util.Arrays.copyOf(d, len);
            cache.put(stream, new Object[]{i, d});
            return d;
        }
    }

    private static byte[] inflate(byte[] in, boolean raw, byte[] dictionary, int size) throws IOException {
        Inflater z = new Inflater(raw);
        try {
            z.setInput(in);
            if (dictionary != null) z.setDictionary(dictionary);
            ByteArrayOutputStream out = new ByteArrayOutputStream(size > 0 ? size : in.length * 2);
            byte[] buf = new byte[1 << 20];
            while (!z.finished()) {
                int n = z.inflate(buf);
                if (n == 0) {
                    if (z.needsDictionary()) throw new IOException("zlib wants a dictionary");
                    if (z.needsInput()) break;  // the voices' stretch ends inside the stream
                }
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        } catch (DataFormatException e) {
            throw new IOException("the download is damaged: " + e.getMessage());
        } finally {
            z.end();
        }
    }

    /**
     * The voices' part of the payload, decompressed. bzip2 goes by way of a file: the 40 MB
     * download and the 60 MB that come out of it together are already more than many phones
     * give an app (Mountain Lion took about 300 MB, and the app was closed).
     */
    private static byte[] decode(Context c, JSONObject e, byte[] raw) throws IOException, JSONException {
        if (e.getString("codec").equals("gzip")) return gunzipFrom(raw, e.optInt("shift"));
        JSONArray bits = e.getJSONArray("bits");
        File tmp = new File(c.getCacheDir(), "compacts.payload");
        try {
            try (OutputStream o = new FileOutputStream(tmp)) {
                Bzip2.decompress(raw, bits.getLong(0), bits.getLong(1), e.getInt("level"), o);
            }
            return Recompiler.readAll(tmp);
        } finally {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
        }
    }

    /** A deflate block that starts `shift` bits into raw's first byte, to where raw ends. */
    static byte[] gunzipFrom(byte[] raw, int shift) throws IOException {
        byte[] d = raw;
        if (shift > 0) {
            d = new byte[raw.length];
            for (int i = 0; i < raw.length; i++) {
                int next = i + 1 < raw.length ? raw[i + 1] & 0xFF : 0;
                d[i] = (byte) (((raw[i] & 0xFF) >>> shift) | (next << (8 - shift)));
            }
        }
        return inflate(d, true, new byte[32768], 0);
    }

    /** PCMWave of each *Compact.SpeechVoice entry of the cpio archive from `at` on. */
    private static Map<String, byte[]> pcmWaves(byte[] d, int at) {
        Map<String, byte[]> out = new HashMap<>();
        while (at + 76 <= d.length && new String(d, at, 6, java.nio.charset.StandardCharsets.ISO_8859_1).equals("070707")) {
            int nl = Integer.parseInt(new String(d, at + 59, 6, java.nio.charset.StandardCharsets.ISO_8859_1), 8);
            int fs = Integer.parseInt(new String(d, at + 65, 11, java.nio.charset.StandardCharsets.ISO_8859_1), 8);
            if (at + 76 + nl + fs > d.length) break;
            String name = new String(d, at + 76, nl - 1, java.nio.charset.StandardCharsets.UTF_8);
            String[] parts = name.split("/", -1);
            if (parts.length < 6 || !parts[5].endsWith("Compact.SpeechVoice")) break;
            if (name.endsWith("/Contents/Resources/PCMWave")) {
                out.put(parts[5].substring(0, parts[5].length() - ".SpeechVoice".length()),
                        java.util.Arrays.copyOfRange(d, at + 76 + nl, at + 76 + nl + fs));
            }
            at += 76 + nl + fs;
        }
        return out;
    }

    private static String sha256(byte[] d) throws IOException {
        try {
            byte[] h = MessageDigest.getInstance("SHA-256").digest(d);
            StringBuilder b = new StringBuilder();
            for (byte x : h) b.append(String.format("%02x", x & 0xFF));
            return b.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }

    private static byte[] get(String url, long start, long n, long[] done, long total, Progress p) throws IOException {
        byte[] out = new byte[(int) n];
        int got = 0;
        for (int attempt = 0; got < n; attempt++) {
            HttpURLConnection conn = null;
            try {
                conn = Source.open(url, start + got, start + n - 1);
                InputStream in = conn.getInputStream();
                int r;
                while (got < n && (r = in.read(out, got, (int) Math.min(1 << 16, n - got))) > 0) {
                    got += r;
                    done[0] += r;
                    p.fraction((double) done[0] / total);
                    if (p.cancelled()) throw new Progress.Cancelled();
                }
            } catch (Progress.Cancelled e) {
                throw e;
            } catch (IOException e) {
                if (attempt >= 3) throw e;
            } finally {
                if (conn != null) conn.disconnect();
            }
        }
        return out;
    }

    /** Every compact of `era` in the languages wanted, into Downloads/&lt;era&gt;/. */
    static List<String> fetch(Context c, String era, Progress p) throws IOException {
        try {
            JSONObject r = recipe(c).getJSONObject(era);
            String name = r.getString("name"), url = r.getString("url");
            long total = downloadSize(c, era);
            p.step("Downloading the " + name + " compact voices (" + total / 1000000 + " MB)");
            JSONArray f = r.getJSONArray("fetch");
            long[][] spans = new long[f.length()][];
            byte[][] data = new byte[f.length()][];
            long[] done = {0};
            for (int i = 0; i < f.length(); i++) {
                spans[i] = new long[]{f.getJSONArray(i).getLong(0), f.getJSONArray(i).getLong(1)};
                data[i] = get(url, spans[i][0], spans[i][1], done, total, p);
            }
            p.step("Unpacking the " + name + " compact voices");
            Streams streams = new Streams(new Fetched(spans, data), r.getJSONArray("streams"));
            JSONObject e = r.getJSONObject("essentials");
            byte[] raw = streams.read(e.getInt("stream"), e.getLong("from"), (int) (e.getLong("to") - e.getLong("from")));
            JSONArray files = r.getJSONArray("files");  // voices outside the payload (Lion's in BaseSystem.dmg)
            Map<String, byte[]> own = new HashMap<>();
            for (int i = 0; i < files.length(); i++) {
                JSONObject x = files.getJSONObject(i);
                own.put(x.getString("voice"), streams.read(x.getInt("stream"), 0, (int) x.getLong("size")));
            }
            // let the download go before decompressing, and each step's input after it (memory)
            streams = null;
            data = null;
            byte[] payload = decode(c, e, raw);
            raw = null;
            Map<String, byte[]> found = pcmWaves(payload, e.getInt("skip"));
            payload = null;
            found.putAll(own);
            Map<String, String[]> want = new TreeMap<>();  // folder -> sha256, locale
            JSONObject v = r.getJSONObject("voices");
            for (java.util.Iterator<String> it = v.keys(); it.hasNext(); ) {
                String k = it.next();
                want.put(k, new String[]{v.getJSONObject(k).getString("sha256"), v.getJSONObject(k).getString("locale")});
            }
            for (int i = 0; i < files.length(); i++) {
                JSONObject x = files.getJSONObject(i);
                want.put(x.getString("voice"), new String[]{x.getString("sha256"), x.getString("locale")});
            }
            for (Map.Entry<String, String[]> w : want.entrySet()) {
                byte[] d = found.get(w.getKey());
                if (d == null || !sha256(d).equals(w.getValue()[0]))
                    throw new IOException(w.getKey() + " from the " + name + " installer isn't the one expected");
            }
            java.util.Set<String> wanted = Languages.wanted(c);
            File dir = new File(Data.downloadsDir(c), era);
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            List<String> installed = new ArrayList<>();
            for (Map.Entry<String, String[]> w : want.entrySet()) {
                String folder = w.getKey();
                if (!Languages.isWanted(w.getValue()[1], wanted) || folder.contains("/") || folder.contains("..")) continue;
                File part = new File(dir, "." + folder + ".part"), dest = new File(dir, folder);
                Firmware.deleteTree(part);
                //noinspection ResultOfMethodCallIgnored
                part.mkdirs();
                Downloads.untarRot13(new ByteArrayInputStream(found.get(folder)), part);
                OutputStream o = new FileOutputStream(new File(part, "locale"));
                o.write(w.getValue()[1].getBytes("UTF-8"));
                o.close();
                Firmware.deleteTree(dest);
                if (!part.renameTo(dest)) throw new IOException("couldn't put " + folder + " in place");
                installed.add(folder);
            }
            OutputStream m = new FileOutputStream(new File(dir, MARKER));
            m.write(android.text.TextUtils.join("\n", installed).getBytes("UTF-8"));
            m.close();
            p.step("The " + name + " compact voices are ready");
            return installed;
        } catch (JSONException e) {
            throw new IOException("bad recipe: " + e.getMessage());
        }
    }
}
