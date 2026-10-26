package com.levigobin.classicvoices;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Map;

/**
 * Builds the fast engine code on the phone, from the firmware imported here,
 * as the Windows and Linux app does on a computer: Recompiler (tools/recomp.py
 * in Java) translates each engine into C, and the Zig C compiler, downloaded
 * from ziglang.org and checked against its published SHA-256, compiles that
 * into cvnative_<build>.so. Nothing of Apple's is in the app; what's built
 * here stays on this phone. Android won't start a program an app downloaded,
 * so Zig runs through libcvzig.so (android/jni/cv_zigrun.c), which the app
 * ships in its lib folder.
 *
 * About 250 MB of space while it works (Zig is deleted when it ends), and a few
 * minutes per firmware.
 */
final class FastCode {
    static final String JOB = "fast";

    /** Tried first: the release this was tested with. Any other comes from the index. */
    private static final String ZIG_VERSION = "0.17.0";
    private static final String ZIG_INDEX = "https://ziglang.org/download/index.json";
    private static final String[] STRIPPED = {"ios3", "ios4", "ios5"};  // recomp.py --scan-bl
    private static final String[] SOURCES = {"thumb.h", "thumb_ops.h", "recomp_rt.h", "native_glue.c"};

    private FastCode() {}

    /** Zig's name for this phone's processor, or null when fast code can't be built here. */
    static String zigArch() {
        String abi = Data.abi();
        if (abi.equals("arm64-v8a")) return "aarch64";
        if (abi.equals("x86_64")) return "x86_64";
        return null;  // 32-bit: too little memory to compile, and an untested calling convention
    }

    static File loader(Context c) {
        return new File(c.getApplicationInfo().nativeLibraryDir, "libcvzig.so");
    }

    static boolean supported(Context c) {
        return zigArch() != null && loader(c).isFile();
    }

    static List<Firmware.Build> imported(Context c) {
        List<Firmware.Build> out = new ArrayList<>();
        for (Firmware.Build b : Firmware.BUILDS) if (Data.hasEngine(c, b.key)) out.add(b);
        return out;
    }

    /** Imported engines without fast code yet. */
    static List<Firmware.Build> missing(Context c) {
        List<Firmware.Build> out = new ArrayList<>();
        for (Firmware.Build b : Firmware.BUILDS) {
            if (Data.hasEngine(c, b.key) && !Library.fastCode(c, b.key)) out.add(b);
        }
        return out;
    }

    /** Set when a build failed or was stopped: then it starts only from the Speed up the voices page. */
    static boolean autoOff(Context c) {
        return Data.prefs(c).getBoolean("fast_auto_off", false);
    }

    static void autoOff(Context c, boolean off) {
        Data.prefs(c).edit().putBoolean("fast_auto_off", off).apply();
    }

    /** Queues a build when an imported engine has no fast code: after imports, and when the app opens. */
    static void startIfNeeded(Context c) {
        if (!supported(c) || missing(c).isEmpty() || ImportService.pending(JOB, JOB) || ImportService.pending(JOB, AGAIN))
            return;
        ImportService.enqueue(c, new ImportService.Job(JOB, JOB, c.getString(R.string.fast_job)));
    }

    /** The job's arg: AGAIN rebuilds every imported engine's fast code, anything else what's missing. */
    static final String AGAIN = "again";

    static void build(Context c, Progress p, boolean again) throws IOException {
        List<Firmware.Build> todo = again ? imported(c) : missing(c);
        if (todo.isEmpty()) return;
        if (!supported(c)) throw new IOException(c.getString(R.string.fast_unsupported));
        File work = new File(c.getCacheDir(), "fastcode");
        Firmware.deleteTree(work);
        //noinspection ResultOfMethodCallIgnored
        work.mkdirs();
        if (work.getUsableSpace() < 400L << 20) throw new IOException(c.getString(R.string.fast_no_space));
        try {
            File zig = ensureZig(c, p);
            File core = new File(work, "core");
            //noinspection ResultOfMethodCallIgnored
            core.mkdirs();
            for (String s : SOURCES) copyAsset(c, "fastcode/" + s, new File(core, s));
            Zig z = new Zig(c, zig, new File(work, "zigcache"));
            // a stand-in libc.so to link against, so the library asks for Android's own (memcpy and
            // the like); it must define what's used, or the linker leaves the dependency out
            File stub = new File(work, "libc.so");
            writeText(new File(work, "stub.c"), "typedef unsigned long size_t;\n"
                    + "void *memcpy(void *d, const void *s, size_t n) { return d; }\n"
                    + "void *memmove(void *d, const void *s, size_t n) { return d; }\n"
                    + "void *memset(void *d, int c, size_t n) { return d; }\n"
                    + "int memcmp(const void *a, const void *b, size_t n) { return 0; }\n"
                    + "void abort(void) { for (;;) {} }\n");
            z.run(p, "cc", "-target", z.target, "-shared", "-nostdlib", "-Wl,-soname,libc.so", "-o", stub.getPath(),
                    new File(work, "stub.c").getPath());

            // translate everything first, so one progress bar covers all the compiling
            List<List<File>> sources = new ArrayList<>();
            long totalBytes = 0;
            for (Firmware.Build b : todo) {
                p.step(c.getString(R.string.fast_translating, b.nice));
                if (p.cancelled()) throw new Progress.Cancelled();
                List<File> files;
                try (InputStream prof = c.getAssets().open("fastcode/profiles/" + b.key + ".txt")) {
                    files = Recompiler.run(Recompiler.readAll(new File(new File(Data.voicesDir(c), b.key), "voiced")),
                            prof, new File(work, "gen_" + b.key), b.key, 1500, isStripped(b.key));
                }
                files.add(new File(core, "native_glue.c"));
                for (File f : files) totalBytes += f.length();
                sources.add(files);
            }

            int jobs = jobs(c);
            long doneBytes = 0;
            for (int i = 0; i < todo.size(); i++) {
                Firmware.Build b = todo.get(i);
                p.step(c.getString(R.string.fast_compiling, b.nice));
                List<File> files = new ArrayList<>(sources.get(i));
                Collections.sort(files, new Comparator<File>() {  // biggest first: the long ones overlap
                    @Override
                    public int compare(File x, File y) {
                        return Long.compare(y.length(), x.length());
                    }
                });
                File objDir = new File(work, "obj_" + b.key);
                //noinspection ResultOfMethodCallIgnored
                objDir.mkdirs();
                List<String> objs = new ArrayList<>();
                List<Running> running = new ArrayList<>();
                try {
                    for (File src : files) {
                        while (running.size() >= jobs) doneBytes += reap(running, p, doneBytes, totalBytes);
                        File o = new File(objDir, src.getName() + ".o");
                        objs.add(o.getPath());
                        running.add(new Running(src.length(), z.start("cc", "-target", z.target, "-O2", "-std=gnu11",
                                "-ffp-contract=off", "-fPIC", "-w", "-I", core.getPath(), "-DCV_BUILD=" + b.key,
                                "-c", src.getPath(), "-o", o.getPath())));
                    }
                    while (!running.isEmpty()) doneBytes += reap(running, p, doneBytes, totalBytes);
                } finally {
                    for (Running r : running) r.proc.destroy();
                }
                File dir = Data.nativeDir(c);
                //noinspection ResultOfMethodCallIgnored
                dir.mkdirs();
                File out = new File(dir, "cvnative_" + b.key + ".so"), tmp = new File(dir, out.getName() + ".new");
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
                List<String> link = new ArrayList<>();
                Collections.addAll(link, "cc", "-target", z.target, "-shared", "-nostdlib",
                        "-Wl,-z,max-page-size=16384", "-s", "-o", tmp.getPath());
                link.addAll(objs);
                link.add("-Wl,--no-as-needed");
                link.add(stub.getPath());
                z.run(p, link.toArray(new String[0]));
                //noinspection ResultOfMethodCallIgnored
                tmp.setWritable(false, false);  // Android 14+ loads code only from read-only files
                if (out.exists()) {
                    //noinspection ResultOfMethodCallIgnored
                    out.setWritable(true);
                    //noinspection ResultOfMethodCallIgnored
                    out.delete();
                }
                if (!tmp.renameTo(out)) throw new IOException(c.getString(R.string.fast_cant_save));
                Firmware.deleteTree(objDir);
                Native.reload();  // the engine picks it up at its next utterance
            }
        } finally {
            Firmware.deleteTree(work);
            Firmware.deleteTree(zigRoot(c));  // ~200 MB, however the build ended; fetched again when needed
        }
    }

    private static boolean isStripped(String key) {
        for (String s : STRIPPED) if (s.equals(key)) return true;
        return false;
    }

    /** As many compilers at once as memory allows: the biggest file takes about 1 GB. */
    private static int jobs(Context c) {
        ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        long spare = mi.availMem - (400L << 20);
        int byMemory = (int) Math.max(1, spare / (1200L << 20));
        return Math.max(1, Math.min(Math.min(byMemory, Runtime.getRuntime().availableProcessors()), 4));
    }

    private static final class Running {
        final long bytes;
        final Process proc;

        Running(long bytes, Process proc) {
            this.bytes = bytes;
            this.proc = proc;
        }
    }

    /** Waits for one compile to finish; returns its source's size, for the progress bar. */
    private static long reap(List<Running> running, Progress p, long doneBytes, long totalBytes) throws IOException {
        while (true) {
            if (p.cancelled()) throw new Progress.Cancelled();
            for (Running r : running) {
                int code;
                try {
                    code = r.proc.exitValue();
                } catch (IllegalThreadStateException still) {
                    continue;
                }
                running.remove(r);
                if (code != 0) throw new IOException("compiling failed: " + Zig.output(r.proc));
                p.fraction((double) (doneBytes + r.bytes) / Math.max(1, totalBytes));
                return r.bytes;
            }
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                throw new Progress.Cancelled();
            }
        }
    }

    // ---------------------------------------------------------------- the compiler

    static File zigRoot(Context c) {
        return new File(c.getNoBackupFilesDir(), "zig");
    }

    /** Zig started through libcvzig.so, its caches in this job's folder. */
    private static final class Zig {
        final String[] env;
        final String loader, target;
        final File out;

        Zig(Context c, File zig, File cache) {
            loader = loader(c).getPath();
            target = zigArch() + "-linux-musl";
            //noinspection ResultOfMethodCallIgnored
            cache.mkdirs();
            out = new File(cache, "out");
            List<String> e = new ArrayList<>();
            for (Map.Entry<String, String> v : System.getenv().entrySet()) e.add(v.getKey() + "=" + v.getValue());
            e.add("CV_ZIG=" + new File(zig, "zig").getPath());
            e.add("ZIG_LIB_DIR=" + new File(zig, "lib").getPath());
            e.add("ZIG_GLOBAL_CACHE_DIR=" + cache.getPath());
            e.add("ZIG_LOCAL_CACHE_DIR=" + cache.getPath());
            e.add("HOME=" + cache.getPath());
            e.add("TMPDIR=" + cache.getPath());
            env = e.toArray(new String[0]);
        }

        private int n;

        Process start(String... args) throws IOException {
            String[] cmd = new String[args.length + 1];
            cmd[0] = loader;
            System.arraycopy(args, 0, cmd, 1, args.length);
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.environment().clear();
            for (String e : env) {
                int eq = e.indexOf('=');
                pb.environment().put(e.substring(0, eq), e.substring(eq + 1));
            }
            pb.redirectErrorStream(true);
            // output goes to a file: a full pipe would stall the compiler
            File log = new File(out.getPath() + (n++) + ".txt");
            if (Build.VERSION.SDK_INT >= 26) pb.redirectOutput(log);
            Process proc = pb.start();
            LOGS.put(proc, log);
            return proc;
        }

        void run(Progress p, String... args) throws IOException {
            Process proc = start(args);
            while (true) {
                if (p.cancelled()) {
                    proc.destroy();
                    throw new Progress.Cancelled();
                }
                try {
                    int code = proc.exitValue();
                    if (code != 0) throw new IOException("the compiler failed: " + output(proc));
                    return;
                } catch (IllegalThreadStateException still) {
                    try {
                        Thread.sleep(50);
                    } catch (InterruptedException e) {
                        throw new Progress.Cancelled();
                    }
                }
            }
        }

        private static final Map<Process, File> LOGS = Collections.synchronizedMap(new java.util.WeakHashMap<Process, File>());

        /** The end of what a finished compile printed. */
        static String output(Process proc) {
            String s = "";
            try {
                File log = LOGS.get(proc);
                InputStream in = log != null && log.isFile() ? new FileInputStream(log) : proc.getInputStream();
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                for (int k; (k = in.read(buf)) > 0; ) b.write(buf, 0, k);
                in.close();
                s = b.toString("UTF-8");
            } catch (IOException ignored) {
                // nothing to add
            }
            s = s.trim();
            return s.length() > 600 ? s.substring(s.length() - 600) : s.isEmpty() ? "no message" : s;
        }
    }

    /** Zig unpacked under zigRoot, downloaded first if it isn't there. Returns its folder. */
    private static File ensureZig(Context c, Progress p) throws IOException {
        File root = zigRoot(c);
        File[] have = root.listFiles();
        if (have != null) {
            for (File d : have) if (new File(d, "zig").isFile() && new File(d, "lib/include").isDirectory()) return d;
        }
        p.step(c.getString(R.string.fast_finding_zig));
        JSONObject index;
        try {
            index = new JSONObject(new String(fetch(ZIG_INDEX), "UTF-8"));
        } catch (org.json.JSONException e) {
            throw new IOException(c.getString(R.string.fast_zig_index));
        }
        String version = index.has(ZIG_VERSION) ? ZIG_VERSION : newest(index);
        String target = zigArch() + "-linux";
        JSONObject entry;
        String url, sha;
        long size;
        try {
            entry = index.getJSONObject(version).getJSONObject(target);
            url = entry.getString("tarball");
            sha = entry.getString("shasum");
            size = entry.optLong("size", 0);
        } catch (org.json.JSONException e) {
            throw new IOException(c.getString(R.string.fast_zig_index));
        }
        Firmware.deleteTree(root);
        //noinspection ResultOfMethodCallIgnored
        root.mkdirs();
        File archive = new File(root, "zig.tar.xz");
        p.step(c.getString(R.string.fast_downloading_zig, Library.size(size)));
        String got = download(url, archive, size, p);
        if (!got.equalsIgnoreCase(sha)) {
            //noinspection ResultOfMethodCallIgnored
            archive.delete();
            throw new IOException(c.getString(R.string.fast_zig_checksum));
        }
        p.step(c.getString(R.string.fast_unpacking_zig));
        File dest = new File(root, version), tmp = new File(root, version + ".tmp");
        Firmware.deleteTree(tmp);
        String arch = zigArch();
        String[] keep = {"zig", "lib/include/", "lib/libc/include/generic-musl/", "lib/libc/include/any-linux-any/",
                "lib/libc/include/" + arch + "-linux-musl/",
                "lib/libc/include/" + (arch.equals("x86_64") ? "x86" : arch) + "-linux-any/"};
        String err = Native.untarXz(archive.getPath(), tmp.getPath(), keep, p);
        //noinspection ResultOfMethodCallIgnored
        archive.delete();
        if (err != null) {
            Firmware.deleteTree(tmp);
            if (err.equals("cancelled")) throw new Progress.Cancelled();
            throw new IOException(err);
        }
        if (!tmp.renameTo(dest)) throw new IOException(c.getString(R.string.fast_cant_save));
        return dest;
    }

    private static String newest(JSONObject index) {
        String best = null;
        for (java.util.Iterator<String> it = index.keys(); it.hasNext(); ) {
            String k = it.next();
            if (!k.matches("\\d+\\.\\d+\\.\\d+")) continue;
            if (best == null || compareVersions(k, best) > 0) best = k;
        }
        return best != null ? best : ZIG_VERSION;
    }

    private static int compareVersions(String a, String b) {
        String[] x = a.split("\\."), y = b.split("\\.");
        for (int i = 0; i < 3; i++) {
            int d = Integer.compare(Integer.parseInt(x[i]), Integer.parseInt(y[i]));
            if (d != 0) return d;
        }
        return 0;
    }

    private static byte[] fetch(String url) throws IOException {
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setConnectTimeout(30000);
        h.setReadTimeout(60000);
        try (InputStream in = h.getInputStream()) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0; ) b.write(buf, 0, k);
            return b.toByteArray();
        } finally {
            h.disconnect();
        }
    }

    /** Downloads url to dest; returns the SHA-256 of what came, in hex. */
    private static String download(String url, File dest, long expected, Progress p) throws IOException {
        MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setConnectTimeout(30000);
        h.setReadTimeout(60000);
        try (InputStream in = h.getInputStream(); OutputStream out = new FileOutputStream(dest)) {
            long total = h.getContentLength() > 0 ? h.getContentLength() : expected, got = 0;
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0; ) {
                if (p.cancelled()) throw new Progress.Cancelled();
                out.write(buf, 0, k);
                sha.update(buf, 0, k);
                got += k;
                if (total > 0) p.fraction((double) got / total);
            }
        } finally {
            h.disconnect();
        }
        StringBuilder s = new StringBuilder();
        for (byte b : sha.digest()) s.append(String.format("%02x", b & 0xFF));
        return s.toString();
    }

    private static void copyAsset(Context c, String name, File dest) throws IOException {
        try (InputStream in = c.getAssets().open(name); OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0; ) out.write(buf, 0, k);
        }
    }

    private static void writeText(File f, String s) throws IOException {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes("UTF-8"));
        }
    }
}
