package com.levigobin.classicvoices;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URLEncoder;
import java.util.Iterator;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;

/**
 * Imports the speech engine and voices from iPhone firmware: streamed from
 * Apple's own servers (only the root filesystem is downloaded, inflated into
 * a temporary file, and deleted afterwards) or read from an IPSW the user
 * has. Nothing of Apple's ships with the app. Follows the NVDA add-on's
 * firmware.py and the Linux classicvoices-data.
 */
final class Firmware {
    static final class Build {
        final String key, build, device, nice, url, purpose;
        final long size;

        Build(String key, String build, String device, String nice, String url, long size, String purpose) {
            this.key = key;
            this.build = build;
            this.device = device;
            this.nice = nice;
            this.url = url;
            this.size = size;
            this.purpose = purpose;
        }
    }

    // Apple's own links (as listed by ipsw.me); keys come from The Apple Wiki.
    static final Build[] BUILDS = {
        new Build("ios6", "10B500", "iPhone2,1", "iOS 6.1.6",
                "https://secure-appldnld.apple.com/iOS6.1/091-3457.20140221.Btt3e/iPhone2,1_6.1.6_10B500_Restore.ipsw",
                822970962L, "The iOS 6 voices. Also needed for the enhanced iOS 6 voices and the Lion and Mountain Lion voices."),
        new Build("ios5", "9B206", "iPhone2,1", "iOS 5.1.1",
                "https://secure-appldnld.apple.com/iOS5.1.1/041-4347.20120427.o2yov/iPhone2,1_5.1.1_9B206_Restore.ipsw",
                720391327L, "The iOS 5 voices, and the engine for the enhanced iOS 5 voices."),
        new Build("ios4", "8L1", "iPhone2,1", "iOS 4.3.5",
                "https://secure-appldnld.apple.com/iPhone4/041-1965.20110721.gxUB5/iPhone2,1_4.3.5_8L1_Restore.ipsw",
                440577895L, "The iOS 4 voices."),
        new Build("ios3", "7E18", "iPhone2,1", "iPhone OS 3.1.3",
                "https://secure-appldnld.apple.com/iPhone/061-7472.20100202.8tugj/iPhone2,1_3.1.3_7E18_Restore.ipsw",
                305122343L, "The iOS 3 voices, as VoiceOver first spoke on the iPhone."),
        new Build("ios7", "11D257", "iPhone3,1", "iOS 7.1.2",
                "https://secure-appldnld.apple.com/iOS7.1/031-4812.20140627.cq6y8/iPhone3,1_7.1.2_11D257_Restore.ipsw",
                1204958925L, "Engine only, for the Mac OS X Mavericks compact voices."),
        new Build("ios10", "14G61", "iPhone5,1", "iOS 10.3.4",
                "https://updates.cdn-apple.com/2019/ios/091-25277-20190722-0C1B94DE-992C-11E9-A2EE-E2C9A77C2E40/iPhone_4.0_32bit_10.3.4_14G61_Restore.ipsw",
                2000796302L, "Engine only, for the Mac OS X Mavericks premium voices."),
    };

    static Build byKey(String key) {
        for (Build b : BUILDS) if (b.key.equals(key)) return b;
        return null;
    }

    static Build byBuild(String build) {
        for (Build b : BUILDS) if (b.build.equals(build)) return b;
        return null;
    }

    // Mac OS X voice eras: common files assembled from firmware files.
    private static final String[][][] MAC_COMMON = {
        {{"mac107"}, {"ios5", "lid.dat"}, {"ios6", "synth_med_fxd_bet2f22.dat"}},
        {{"mac108"}, {"ios6", "lid.dat"}, {"ios6", "synth_med_fxd_bet2f22.dat"}, {"ios7", "synth_med_fxd_bet3f22.dat"}},
        {{"mac109"}, {"ios7", "lid.dat"}, {"ios7", "synth_med_fxd_bet3f22.dat"}},
    };

    // ---------------------------------------------------------------- the zip

    /** {name, method, compressed size, local header offset} of the largest .dmg. */
    static Object[] rootfsEntry(Source s) throws IOException {
        long size = s.size();
        long tailAt = Math.max(0, size - 65536);
        byte[] tail = s.read(tailAt, (int) (size - tailAt));
        int i = lastIndexOf(tail, new byte[] {'P', 'K', 5, 6});
        if (i < 0) throw new IOException("not an IPSW (no zip directory)");
        long cdSize = Source.le32(tail, i + 12), cdOff = Source.le32(tail, i + 16);
        if (cdOff == 0xFFFFFFFFL) {
            int j = lastIndexOf(tail, new byte[] {'P', 'K', 6, 7});
            long z64 = Source.le64(tail, j + 8);
            byte[] z = s.read(z64, 56);
            cdSize = Source.le64(z, 40);
            cdOff = Source.le64(z, 48);
        }
        byte[] cd = cdOff >= tailAt ? java.util.Arrays.copyOfRange(tail, (int) (cdOff - tailAt), (int) (cdOff - tailAt + cdSize))
                : s.read(cdOff, (int) cdSize);
        Object[] best = null;
        long bestSize = -1;
        int p = 0;
        while (p + 46 <= cd.length && cd[p] == 'P' && cd[p + 1] == 'K' && cd[p + 2] == 1 && cd[p + 3] == 2) {
            int method = Source.le16(cd, p + 10);
            long csz = Source.le32(cd, p + 20), usz = Source.le32(cd, p + 24);
            int nl = Source.le16(cd, p + 28), el = Source.le16(cd, p + 30), cl = Source.le16(cd, p + 32);
            long lho = Source.le32(cd, p + 42);
            String name = new String(cd, p + 46, nl, "UTF-8");
            if (csz == 0xFFFFFFFFL || usz == 0xFFFFFFFFL || lho == 0xFFFFFFFFL) {
                int q = p + 46 + nl, end = q + el;
                while (q + 4 <= end) {
                    int id = Source.le16(cd, q), hl = Source.le16(cd, q + 2);
                    if (id == 1) {
                        int v = q + 4;
                        if (usz == 0xFFFFFFFFL) { usz = Source.le64(cd, v); v += 8; }
                        if (csz == 0xFFFFFFFFL) { csz = Source.le64(cd, v); v += 8; }
                        if (lho == 0xFFFFFFFFL) { lho = Source.le64(cd, v); }
                    }
                    q += 4 + hl;
                }
            }
            if (name.toLowerCase().endsWith(".dmg") && usz > bestSize) {
                bestSize = usz;
                String base = name.substring(name.lastIndexOf('/') + 1);
                best = new Object[] {base, method, csz, lho};
            }
            p += 46 + nl + el + cl;
        }
        if (best == null) throw new IOException("no disk images in the firmware");
        return best;
    }

    /** The firmware build named in the IPSW's BuildManifest/Restore.plist ("10B500"), or null. */
    static String identify(Source s) throws IOException {
        // Restore.plist and BuildManifest.plist are small; find either in the directory
        long size = s.size();
        long tailAt = Math.max(0, size - 65536);
        byte[] tail = s.read(tailAt, (int) (size - tailAt));
        int i = lastIndexOf(tail, new byte[] {'P', 'K', 5, 6});
        if (i < 0) return null;
        long cdSize = Source.le32(tail, i + 12), cdOff = Source.le32(tail, i + 16);
        if (cdOff == 0xFFFFFFFFL) return null;
        byte[] cd = cdOff >= tailAt ? java.util.Arrays.copyOfRange(tail, (int) (cdOff - tailAt), (int) (cdOff - tailAt + cdSize))
                : s.read(cdOff, (int) cdSize);
        int p = 0;
        while (p + 46 <= cd.length && cd[p] == 'P' && cd[p + 1] == 'K' && cd[p + 2] == 1 && cd[p + 3] == 2) {
            int method = Source.le16(cd, p + 10);
            long csz = Source.le32(cd, p + 20);
            int nl = Source.le16(cd, p + 28), el = Source.le16(cd, p + 30), cl = Source.le16(cd, p + 32);
            long lho = Source.le32(cd, p + 42);
            String name = new String(cd, p + 46, nl, "UTF-8");
            if ((name.equals("Restore.plist") || name.equals("BuildManifest.plist")) && csz < (4 << 20)) {
                byte[] h = s.read(lho, 30);
                long start = lho + 30 + Source.le16(h, 26) + Source.le16(h, 28);
                InputStream in = s.stream(start, csz);
                if (method == 8) in = new InflaterInputStream(in, new Inflater(true));
                String text = new String(readAll(in), "UTF-8");
                Matcher m = Pattern.compile("<key>ProductBuildVersion</key>\\s*<string>([^<]+)</string>").matcher(text);
                if (m.find()) return m.group(1).trim();
            }
            p += 46 + nl + el + cl;
        }
        return null;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        in.close();
        return b.toByteArray();
    }

    private static int lastIndexOf(byte[] h, byte[] n) {
        for (int i = h.length - n.length; i >= 0; i--) {
            boolean ok = true;
            for (int j = 0; j < n.length && ok; j++) ok = h[i + j] == n[j];
            if (ok) return i;
        }
        return -1;
    }

    /** Inflate the root filesystem out of the IPSW into dest. Returns its name. */
    static String streamRootfs(Source s, File dest, Progress p) throws IOException {
        Object[] e = rootfsEntry(s);
        String name = (String) e[0];
        int method = (Integer) e[1];
        long csize = (Long) e[2], lho = (Long) e[3];
        byte[] h = s.read(lho, 30);
        long start = lho + 30 + Source.le16(h, 26) + Source.le16(h, 28);
        p.step("Downloading the root filesystem (" + (csize >> 20) + " MB)");
        final long[] got = {0};
        final long total = csize;
        final Progress pp = p;
        InputStream raw = s.stream(start, csize);
        InputStream counted = new java.io.FilterInputStream(raw) {
            long lastReport;

            @Override
            public int read(byte[] b, int o, int n) throws IOException {
                if (pp.cancelled()) throw new Progress.Cancelled();
                int r = super.read(b, o, n);
                if (r > 0) {
                    got[0] += r;
                    long now = System.currentTimeMillis();
                    if (now - lastReport > 500) {
                        lastReport = now;
                        pp.fraction(got[0] / (double) total);
                    }
                }
                return r;
            }
        };
        InputStream in = method == 8 ? new InflaterInputStream(counted, new Inflater(true), 1 << 16) : counted;
        OutputStream out = new FileOutputStream(dest);
        try {
            byte[] buf = new byte[1 << 18];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } finally {
            out.close();
            in.close();
        }
        if (got[0] != csize) throw new IOException("the download ended early");
        return name;
    }

    // ---------------------------------------------------------------- keys

    private static final String[] WIKI = {"https://theapplewiki.com/api.php", "https://theapplewiki.com/w/api.php"};

    private static JSONObject wiki(String query) throws IOException {
        IOException last = null;
        for (String base : WIKI) {
            try {
                HttpURLConnection c = Source.open(base + "?" + query + "&format=json", -1, -1);
                try {
                    return new JSONObject(new String(readAll(c.getInputStream()), "UTF-8"));
                } finally {
                    c.disconnect();
                }
            } catch (Exception e) {
                last = e instanceof IOException ? (IOException) e : new IOException(e.toString());
            }
        }
        throw new IOException("The Apple Wiki is unreachable: " + (last != null ? last.getMessage() : "?"));
    }

    private static String enc(String s) throws IOException {
        return URLEncoder.encode(s, "UTF-8");
    }

    /** The root filesystem's VFDecrypt key for one firmware, from The Apple Wiki (firmware.py's fetch_rootfs_key). */
    static String fetchKey(String build, String device, String rootfsName) throws IOException {
        try {
            JSONObject si = wiki("action=query&meta=siteinfo&siprop=namespaces");
            JSONObject nss = si.getJSONObject("query").getJSONObject("namespaces");
            String ns = "0";
            for (Iterator<String> it = nss.keys(); it.hasNext(); ) {
                String k = it.next();
                JSONObject n = nss.getJSONObject(k);
                if ("Keys".equals(n.optString("canonical")) || "Keys".equals(n.optString("*"))) ns = k;
            }
            JSONObject res = wiki("action=query&list=search&srsearch=" + enc(build + " " + device) + "&srnamespace=" + ns
                    + "&srlimit=10");
            JSONArray hits = res.getJSONObject("query").getJSONArray("search");
            String title = null, flat = device.replace(" ", "");
            for (int i = 0; i < hits.length(); i++) {
                String t = hits.getJSONObject(i).getString("title");
                if (t.contains(build) && t.replace(" ", "").contains(flat)) { title = t; break; }
            }
            if (title == null && hits.length() > 0) title = hits.getJSONObject(0).getString("title");
            if (title == null) throw new IOException("no key page found for " + build + " (" + device + ")");
            String html = wiki("action=parse&page=" + enc(title) + "&prop=text").getJSONObject("parse")
                    .getJSONObject("text").getString("*");
            String text = html.replaceAll("<[^>]+>", " ").replaceAll("&[a-zA-Z#0-9]+;", " ").replaceAll("\\s+", " ");
            String[] segs = text.split("Filename:");
            for (int i = 1; i < segs.length; i++) {
                Matcher fn = Pattern.compile("(\\S+\\.dmg)").matcher(segs[i]);
                Matcher k = Pattern.compile("\\bKey\\b\\s*:?\\s*([0-9a-fA-F]{64,80})").matcher(segs[i]);
                if (fn.find() && k.find() && fn.group(1).equals(rootfsName)) return k.group(1).toLowerCase();
            }
            Matcher any = Pattern.compile("\\b([0-9a-fA-F]{72})\\b").matcher(text);
            if (any.find()) return any.group(1).toLowerCase();
            throw new IOException("no decryption key for " + rootfsName + " on " + title);
        } catch (org.json.JSONException e) {
            throw new IOException("unexpected answer from The Apple Wiki: " + e.getMessage());
        }
    }

    // ---------------------------------------------------------------- import

    /** Import one firmware: from Apple (local == null) or from an IPSW on the device. */
    static void importBuild(Context c, Build b, Source local, Progress p) throws IOException {
        File voices = Data.voicesDir(c);
        //noinspection ResultOfMethodCallIgnored
        voices.mkdirs();
        File tmp = new File(Data.root(c), "rootfs.tmp");
        Source src = local != null ? local : new Source.Http(b.url);
        try {
            p.step((local != null ? "Reading " : "Downloading ") + b.nice + " (" + b.build + ")");
            String name = streamRootfs(src, tmp, p);
            RandomAccessFile raf = new RandomAccessFile(tmp, "r");
            Source img = new Source.FileSrc(raf);
            try {
                if (Encrcdsa.is(img)) {
                    p.step("Fetching the decryption key from The Apple Wiki");
                    img = new Encrcdsa(img, fetchKey(b.build, b.device, name));
                }
                android.util.Log.i("ClassicVoices", "import " + b.key + ": " + name + ", " + (tmp.length() >> 20) + " MB");
                p.step("Reading the filesystem");
                Hfs vol = volume(img);
                File dest = new File(voices, b.key + ".importing");
                deleteTree(dest);
                p.step("Copying the " + b.nice + " engine and voices");
                p.fraction(-1);
                if (b.key.equals("ios10")) {
                    // iOS 10 is only the engine for the Mavericks premium voices
                    Hfs.Entry bundle = vol.entry("System/Library/TTSPlugins/TTSSpeechBundle.speechbundle/TTSSpeechBundle");
                    if (bundle == null) throw new IOException("the speech engine isn't in this firmware");
                    vol.copyFile(bundle, new File(dest, "voiced"), p);
                    File fin = new File(voices, b.key);
                    deleteTree(fin);
                    if (!dest.renameTo(fin)) throw new IOException("couldn't move the imported files into place");
                    return;
                }
                Hfs.Entry voiced = vol.entry("System/Library/PrivateFrameworks/VoiceServices.framework/Support/voiced");
                Hfs.Entry res = vol.entry("System/Library/PrivateFrameworks/VoiceServices.framework/TTSResources");
                if (voiced == null || res == null) throw new IOException("the speech engine isn't in this firmware");
                vol.copyFile(voiced, new File(dest, "voiced"), p);
                if (b.key.equals("ios7")) {
                    // iOS 7 is only an engine for the Mavericks voices: its own voices aren't wanted
                    File tr = new File(dest, "TTSResources");
                    for (Hfs.Entry k : vol.children(res)) {
                        if (k.name.contains("/") || k.name.startsWith(".")) continue;
                        if (!k.dir) vol.copyFile(k, new File(tr, k.name), p);
                        else if (k.name.equals("common")) vol.copyTree(k, new File(tr, k.name), p);
                    }
                } else {
                    vol.copyTree(res, new File(dest, "TTSResources"), p);
                }
                File fin = new File(voices, b.key);
                deleteTree(fin);
                if (!dest.renameTo(fin)) throw new IOException("couldn't move the imported files into place");
            } finally {
                raf.close();
            }
        } finally {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
        }
        assembleMacCommon(voices);
        p.step(b.nice + " imported");
    }

    /** The HFS+ volume in a root filesystem image: bare, partitioned, or inside a UDIF image. */
    static Hfs volume(Source img) throws IOException {
        try {
            return new Hfs(img, Hfs.findHfs(img));
        } catch (IOException plain) {
            if (!Udif.is(img)) throw plain;
            Source u = new Udif(img);
            return new Hfs(u, Hfs.findHfs(u));
        }
    }

    static void assembleMacCommon(File voices) throws IOException {
        for (String[][] era : MAC_COMMON) {
            boolean all = true;
            for (int i = 1; i < era.length; i++) {
                all &= new File(voices, era[i][0] + "/TTSResources/common/" + era[i][1]).isFile();
            }
            if (!all) continue;
            File out = new File(voices, era[0][0] + "/common");
            //noinspection ResultOfMethodCallIgnored
            out.mkdirs();
            for (int i = 1; i < era.length; i++) {
                File dest = new File(out, era[i][1]);
                if (!dest.isFile()) copy(new File(voices, era[i][0] + "/TTSResources/common/" + era[i][1]), dest);
            }
        }
    }

    static void copy(File from, File to) throws IOException {
        InputStream in = new java.io.FileInputStream(from);
        OutputStream out = new FileOutputStream(to);
        try {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } finally {
            in.close();
            out.close();
        }
    }

    static void deleteTree(File f) {
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) deleteTree(k);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
