package com.levigobin.classicvoices;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.security.MessageDigest;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Is there a newer Classic Voices? GitHub is asked at most once a day, in the
 * background, and each new version is offered once. If the person says yes,
 * its APK is downloaded, checked against the SHA-256 GitHub publishes for it,
 * and handed to Android's package installer, which asks them to confirm (the
 * first time Android also asks them to let Classic Voices install apps; the
 * update goes on when they come back).
 */
final class Update {
    private static final String RELEASES = "https://api.github.com/repos/Levi-Gobin/classicvoices/releases?per_page=10";
    /** The dev channel: the build of the newest commit on the dev branch, which CI replaces on every push. */
    private static final String DEV_RELEASE = "https://api.github.com/repos/Levi-Gobin/classicvoices/releases/tags/dev-latest";
    private static final String PAGE = "https://github.com/Levi-Gobin/classicvoices/releases";
    private static final String ASSET = "ClassicVoices.apk";
    private static final long DAY = 24L * 3600 * 1000;

    private Update() {}

    /** A newer release: its version and page, and its APK (url null if it has none). */
    static final class Release {
        String version, page, url, sha256, commit;
        long size;
    }

    /**
     * "stable" (the releases) or "dev" (every change on the dev branch). A build from the dev branch, the first time
     * it runs, sets dev (someone who installs one wants its updates); the person's choice holds after that, until
     * another dev build is put in.
     */
    static boolean dev(Context c) {
        SharedPreferences p = c.getSharedPreferences("update", 0);
        if ("dev".equals(BuildInfo.CHANNEL) && !BuildInfo.COMMIT.equals(p.getString("channel_build", ""))) {
            p.edit().putString("channel", "dev").putString("channel_build", BuildInfo.COMMIT).apply();
            return true;
        }
        return "dev".equals(p.getString("channel", "stable"));
    }

    static void setDev(Activity a, boolean on) {
        a.getSharedPreferences("update", 0).edit().putString("channel", on ? "dev" : "stable").remove("checked").apply();
        if (on) check(a);
    }

    static void check(final Activity a) {
        final SharedPreferences p = a.getSharedPreferences("update", 0);
        final boolean dev = dev(a);
        if (System.currentTimeMillis() - p.getLong("checked", 0) < (dev ? DAY / 24 : DAY)) return;
        final String mine;
        try {
            mine = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                Release found = dev ? newestDev(p.getString("announced_dev", "")) : newest(mine, p.getString("announced", "0"));
                SharedPreferences.Editor e = p.edit().putLong("checked", System.currentTimeMillis());
                if (found != null) e.putString(dev ? "announced_dev" : "announced", dev ? found.commit : found.version);
                e.apply();
                if (found != null) offer(a, found);
            }
        }, "update").start();
    }

    /** After the person let Classic Voices install apps: the update they asked for goes on. */
    static void resume(Activity a) {
        SharedPreferences p = a.getSharedPreferences("update", 0);
        String url = p.getString("pending_url", null);
        if (url == null || !canInstall(a)) return;
        Release r = new Release();
        r.version = p.getString("pending_version", "");
        r.page = p.getString("pending_page", PAGE);
        r.url = url;
        r.sha256 = p.getString("pending_sha256", null);
        r.size = p.getLong("pending_size", 0);
        p.edit().remove("pending_url").apply();
        download(a, r);
    }

    /** The newest release above both, or null (also offline). */
    private static Release newest(String mine, String announced) {
        try {
            HttpURLConnection c = Source.open(RELEASES, -1, -1);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] b = new byte[16384];
                for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
            } finally {
                c.disconnect();
            }
            JSONArray rels = new JSONArray(out.toString("UTF-8"));
            Release best = null;
            for (int i = 0; i < rels.length(); i++) {
                JSONObject r = rels.getJSONObject(i);
                String v = r.optString("tag_name").replaceFirst("^v", "");
                if (r.optBoolean("draft") || compare(v, mine) <= 0 || compare(v, announced) <= 0) continue;
                if (best != null && compare(v, best.version) <= 0) continue;
                best = new Release();
                best.version = v;
                best.page = r.optString("html_url", PAGE);
                JSONArray assets = r.optJSONArray("assets");
                for (int k = 0; assets != null && k < assets.length(); k++) {
                    JSONObject x = assets.getJSONObject(k);
                    if (!ASSET.equals(x.optString("name"))) continue;
                    best.url = x.optString("browser_download_url", null);
                    best.size = x.optLong("size");
                    String d = x.optString("digest", "");
                    best.sha256 = d.startsWith("sha256:") ? d.substring(7).toLowerCase(java.util.Locale.ROOT) : null;
                }
            }
            return best;
        } catch (Exception e) {
            return null;
        }
    }

    /** The dev-latest build, when its commit is newer than this one's (BuildInfo) and not yet announced; or null. */
    private static Release newestDev(String announced) {
        try {
            JSONObject r = new JSONObject(fetchText(DEV_RELEASE));
            JSONArray assets = r.optJSONArray("assets");
            String info = null;
            Release best = new Release();
            for (int k = 0; assets != null && k < assets.length(); k++) {
                JSONObject x = assets.getJSONObject(k);
                if ("dev.json".equals(x.optString("name"))) info = x.optString("browser_download_url", null);
                if (!ASSET.equals(x.optString("name"))) continue;
                best.url = x.optString("browser_download_url", null);
                best.size = x.optLong("size");
                String d = x.optString("digest", "");
                best.sha256 = d.startsWith("sha256:") ? d.substring(7).toLowerCase(java.util.Locale.ROOT) : null;
            }
            if (info == null || BuildInfo.COMMIT.isEmpty()) return null;
            JSONObject j = new JSONObject(fetchText(info));
            String commit = j.optString("commit");
            if (commit.isEmpty() || commit.equals(BuildInfo.COMMIT) || commit.equals(announced)
                    || j.optLong("time") <= BuildInfo.TIME) return null;
            best.commit = commit;
            best.version = j.optString("version", "dev") + "-dev." + commit.substring(0, Math.min(7, commit.length()));
            best.page = r.optString("html_url", PAGE);
            return best;
        } catch (Exception e) {
            return null;
        }
    }

    private static String fetchText(String url) throws Exception {
        HttpURLConnection c = Source.open(url, -1, -1);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (InputStream in = c.getInputStream()) {
            byte[] b = new byte[16384];
            for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
        } finally {
            c.disconnect();
        }
        return out.toString("UTF-8");
    }

    private static int compare(String a, String b) {
        int[] x = number(a), y = number(b);
        for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        return 0;
    }

    private static int[] number(String v) {
        int[] n = new int[3];
        Matcher m = Pattern.compile("\\d+").matcher(v == null ? "" : v);
        for (int i = 0; i < 3 && m.find(); i++) n[i] = Integer.parseInt(m.group());
        return n;
    }

    private static boolean canInstall(Context c) {
        return Build.VERSION.SDK_INT < 26 || c.getPackageManager().canRequestPackageInstalls();
    }

    private static void offer(final Activity a, final Release r) {
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (a.isFinishing()) return;
                AlertDialog.Builder b = new AlertDialog.Builder(a).setNegativeButton(R.string.update_later, null);
                if (r.url == null) {
                    b.setMessage(a.getString(R.string.update_available, r.version))
                            .setPositiveButton(R.string.update_open, new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(r.page)));
                                }
                            });
                } else {
                    b.setMessage(a.getString(R.string.update_install_question, r.version))
                            .setPositiveButton(R.string.update_install, new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    install(a, r);
                                }
                            });
                }
                b.show();
            }
        });
    }

    private static void install(Activity a, Release r) {
        if (!canInstall(a)) {
            // Android asks once whether Classic Voices may install apps; resume() goes on afterwards
            a.getSharedPreferences("update", 0).edit().putString("pending_url", r.url).putString("pending_version", r.version)
                    .putString("pending_page", r.page).putString("pending_sha256", r.sha256).putLong("pending_size", r.size)
                    .apply();
            Toast.makeText(a, R.string.update_allow, Toast.LENGTH_LONG).show();
            a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName())));
            return;
        }
        download(a, r);
    }

    private static void download(final Activity a, final Release r) {
        Toast.makeText(a, a.getString(R.string.update_downloading, r.version), Toast.LENGTH_SHORT).show();
        final Context app = a.getApplicationContext();
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    File apk = new File(app.getCacheDir(), "update.apk");
                    fetch(r, apk);
                    PackageInstaller pi = app.getPackageManager().getPackageInstaller();
                    PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
                            PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                    params.setAppPackageName(app.getPackageName());
                    int id = pi.createSession(params);
                    PackageInstaller.Session s = pi.openSession(id);
                    try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("update.apk", 0, apk.length())) {
                        byte[] b = new byte[65536];
                        for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
                        s.fsync(out);
                    }
                    Intent done = new Intent(app, UpdateReceiver.class);
                    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                    s.commit(PendingIntent.getBroadcast(app, id, done, flags).getIntentSender());
                    s.close();
                    //noinspection ResultOfMethodCallIgnored
                    apk.delete();
                } catch (final Exception e) {
                    a.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            Toast.makeText(app, app.getString(R.string.update_failed, e.getMessage()), Toast.LENGTH_LONG).show();
                        }
                    });
                }
            }
        }, "update").start();
    }

    private static void fetch(Release r, File dest) throws Exception {
        HttpURLConnection c = Source.open(r.url, -1, -1);
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        long n = 0;
        try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(dest)) {
            byte[] b = new byte[65536];
            for (int k; (k = in.read(b)) > 0; ) {
                out.write(b, 0, k);
                sha.update(b, 0, k);
                n += k;
            }
        } finally {
            c.disconnect();
        }
        if (r.size > 0 && n != r.size) throw new java.io.IOException("the download was cut short");
        if (r.sha256 != null) {
            StringBuilder hex = new StringBuilder();
            for (byte x : sha.digest()) hex.append(String.format("%02x", x));
            if (!hex.toString().equals(r.sha256)) throw new java.io.IOException("the download doesn't match what GitHub published");
        }
    }
}
