package com.levigobin.classicvoices;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Is there a newer Classic Voices? GitHub is asked at most once a day, in the
 * background, and each new version is announced once, offering its release
 * page. Nothing is downloaded or installed here.
 */
final class Update {
    private static final String RELEASES = "https://api.github.com/repos/Levi-Gobin/classicvoices/releases?per_page=10";
    private static final long DAY = 24L * 3600 * 1000;

    private Update() {}

    static void check(final Activity a) {
        final SharedPreferences p = a.getSharedPreferences("update", 0);
        if (System.currentTimeMillis() - p.getLong("checked", 0) < DAY) return;
        final String mine;
        try {
            mine = a.getPackageManager().getPackageInfo(a.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                String[] found = newest(mine, p.getString("announced", "0"));
                SharedPreferences.Editor e = p.edit().putLong("checked", System.currentTimeMillis());
                if (found != null) e.putString("announced", found[0]);
                e.apply();
                if (found != null) offer(a, found[0], found[1]);
            }
        }, "update").start();
    }

    /** {version, page} of the newest release above both, or null (also offline). */
    private static String[] newest(String mine, String announced) {
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
            String[] best = null;
            for (int i = 0; i < rels.length(); i++) {  // pre-releases, so not .../releases/latest
                JSONObject r = rels.getJSONObject(i);
                String v = r.optString("tag_name").replaceFirst("^v", "");
                if (r.optBoolean("draft") || compare(v, mine) <= 0 || compare(v, announced) <= 0) continue;
                if (best == null || compare(v, best[0]) > 0)
                    best = new String[]{v, r.optString("html_url", "https://github.com/Levi-Gobin/classicvoices/releases")};
            }
            return best;
        } catch (Exception e) {
            return null;
        }
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

    private static void offer(final Activity a, final String version, final String url) {
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (a.isFinishing()) return;
                new AlertDialog.Builder(a)
                        .setMessage(a.getString(R.string.update_available, version))
                        .setPositiveButton(R.string.update_open, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface d, int w) {
                                a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
                            }
                        })
                        .setNegativeButton(R.string.update_later, null)
                        .show();
            }
        });
    }
}
