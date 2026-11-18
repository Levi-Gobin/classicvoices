package com.levigobin.classicvoices;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.Charset;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Publishing the person's dictionary changes through GitHub's own API (as the iPhone's GitHubPublishing.swift and
 * the NVDA add-on's ghpublish.py do): the Classic Voices GitHub App (Nico's), signed in to with GitHub's device flow
 * (a code typed at github.com/login/device; no password and no secret in the app). Its keys are kept in the app's
 * own files, encrypted with a key from the Android Keystore (Android 6 and later), never in the clear.
 *
 * Each changed file: the repository's latest copy, the person's changes made in it afresh, one commit per file with
 * git's messages (DictionaryStore.message); written meanwhile by someone else (409): again on the new copy. Someone
 * who can't write to the source is offered their own copy (a fork) and publishes there; pull requests are theirs to
 * open on GitHub. Everything here blocks: call it off the main thread.
 *
 * Test builds only (debuggable): the addresses can point at a mock GitHub (MainActivity's "github_api" and
 * "github_web" extras, as CV_GITHUB_API / CV_GITHUB_WEB elsewhere).
 */
final class GitHub {
    static final String CLIENT_ID = "Iv23liprOaNWuXD30i5d";  // the Classic Voices GitHub App (Nico's); not secret
    static final String APP_SLUG = "classic-voices";
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final String KEY_ALIAS = "classicvoices-github";
    private static final long REFRESH_BEFORE = 5 * 60 * 1000L;
    private static final Object LOCK = new Object();

    private GitHub() {}

    static final class Failure extends Exception {
        final int status;  // 0: no answer (the connection)

        Failure(int status, String message) {
            super(message);
            this.status = status;
        }
    }

    /** No keys, or GitHub wouldn't renew them. */
    static final class SignedOut extends Exception {
        SignedOut() { super("You're not signed in to GitHub any more. Sign in again to publish."); }
    }

    // ---------------------------------------------------------------- where

    private static boolean debuggable(Context c) {
        return (c.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    static SharedPreferences state(Context c) {
        return c.getApplicationContext().getSharedPreferences("publishing", Context.MODE_PRIVATE);
    }

    static String api(Context c) {
        String o = debuggable(c) ? state(c).getString("test_api", "") : "";
        return o.isEmpty() ? "https://api.github.com" : trimSlash(o);
    }

    static String web(Context c) {
        String o = debuggable(c) ? state(c).getString("test_web", "") : "";
        return o.isEmpty() ? "https://github.com" : trimSlash(o);
    }

    private static String trimSlash(String s) {
        while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
        return s;
    }

    /** Test builds: a mock GitHub (empty: the real one). */
    static void setTestAddresses(Context c, String api, String web) {
        if (!debuggable(c)) return;
        state(c).edit().putString("test_api", api == null ? "" : api).putString("test_web", web == null ? "" : web).commit();
    }

    /** Signing in needs the Keystore's AES keys (Android 6 and later). */
    static boolean supported() { return Build.VERSION.SDK_INT >= 23; }

    static String installUrl(Context c) { return web(c) + "/apps/" + APP_SLUG + "/installations/new"; }

    // ---------------------------------------------------------------- HTTP

    static final class Response {
        final int code;
        final byte[] body;

        Response(int code, byte[] body) {
            this.code = code;
            this.body = body;
        }

        JSONObject json() {
            try {
                return new JSONObject(new String(body, UTF8));
            } catch (Exception e) {
                return new JSONObject();
            }
        }

        String message() { return json().optString("message", ""); }
    }

    /** form goes as application/x-www-form-urlencoded, json as JSON. Failure(0) when GitHub couldn't be reached. */
    static Response request(String method, String url, Map<String, String> form, JSONObject json, String token,
                            String accept) throws Failure {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod(method);
            c.setConnectTimeout(30000);
            c.setReadTimeout(30000);
            c.setRequestProperty("Accept", accept != null ? accept : "application/vnd.github+json");
            c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
            c.setRequestProperty("User-Agent", "ClassicVoices");
            if (token != null) c.setRequestProperty("Authorization", "Bearer " + token);
            byte[] body = null;
            if (form != null) {
                StringBuilder b = new StringBuilder();
                for (Map.Entry<String, String> e : form.entrySet()) {
                    if (b.length() > 0) b.append('&');
                    b.append(URLEncoder.encode(e.getKey(), "UTF-8")).append('=').append(URLEncoder.encode(e.getValue(), "UTF-8"));
                }
                body = b.toString().getBytes(UTF8);
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
            } else if (json != null) {
                body = json.toString().getBytes(UTF8);
                c.setRequestProperty("Content-Type", "application/json");
            }
            if (body != null) {
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                OutputStream out = c.getOutputStream();
                try {
                    out.write(body);
                } finally {
                    out.close();
                }
            }
            int code = c.getResponseCode();
            InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            if (in != null) {
                try {
                    byte[] buf = new byte[16384];
                    int n;
                    while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
                } finally {
                    in.close();
                }
            }
            return new Response(code, b.toByteArray());
        } catch (IOException e) {
            throw new Failure(0, "GitHub couldn't be reached (" + (e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName()) + ").");
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static Map<String, String> form(String... kv) {
        Map<String, String> m = new LinkedHashMap<>();
        for (int i = 0; i + 1 < kv.length; i += 2) m.put(kv[i], kv[i + 1]);
        return m;
    }

    // ---------------------------------------------------------------- the keys, encrypted with the Keystore

    static final class Tokens {
        String access, refresh, login;
        long expires;  // ms
    }

    private static File tokenFile(Context c) { return new File(c.getApplicationContext().getFilesDir(), "github.dat"); }

    private static SecretKey key(boolean create) throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        KeyStore.Entry e = ks.getEntry(KEY_ALIAS, null);
        if (e instanceof KeyStore.SecretKeyEntry) return ((KeyStore.SecretKeyEntry) e).getSecretKey();
        if (!create) return null;
        KeyGenerator g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        g.init(new KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build());
        return g.generateKey();
    }

    static Tokens loadTokens(Context c) {
        if (!supported()) return null;
        synchronized (LOCK) {
            try {
                File f = tokenFile(c);
                if (!f.isFile()) return null;
                byte[] all = readAll(f);
                if (all.length < 4 + 12 || all[0] != 'C' || all[1] != 'V' || all[2] != 'K' || all[3] != '1') return null;
                SecretKey k = key(false);
                if (k == null) return null;
                Cipher ci = Cipher.getInstance("AES/GCM/NoPadding");
                ci.init(Cipher.DECRYPT_MODE, k, new GCMParameterSpec(128, all, 4, 12));
                byte[] plain = ci.doFinal(all, 16, all.length - 16);
                JSONObject j = new JSONObject(new String(plain, UTF8));
                Tokens t = new Tokens();
                t.access = j.getString("access");
                t.refresh = j.optString("refresh", "");
                t.login = j.optString("login", "");
                t.expires = j.optLong("expires", 0);
                return t;
            } catch (Exception e) {
                return null;  // not readable (the key is gone after a restore, say): signed out
            }
        }
    }

    static void saveTokens(Context c, Tokens t) {
        synchronized (LOCK) {
            File f = tokenFile(c);
            if (t == null) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
                return;
            }
            try {
                JSONObject j = new JSONObject();
                j.put("access", t.access);
                j.put("refresh", t.refresh == null ? "" : t.refresh);
                j.put("login", t.login == null ? "" : t.login);
                j.put("expires", t.expires);
                Cipher ci = Cipher.getInstance("AES/GCM/NoPadding");
                ci.init(Cipher.ENCRYPT_MODE, key(true));
                byte[] iv = ci.getIV(), sealed = ci.doFinal(j.toString().getBytes(UTF8));
                ByteArrayOutputStream b = new ByteArrayOutputStream();
                b.write(new byte[] {'C', 'V', 'K', '1'});
                b.write(iv, 0, 12);
                b.write(sealed);
                File part = new File(f.getPath() + ".part");
                OutputStream out = new java.io.FileOutputStream(part);
                try {
                    out.write(b.toByteArray());
                } finally {
                    out.close();
                }
                if (!part.renameTo(f)) throw new IOException("rename");
            } catch (Exception e) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
    }

    private static byte[] readAll(File f) throws IOException {
        java.io.FileInputStream in = new java.io.FileInputStream(f);
        try {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
            return b.toByteArray();
        } finally {
            in.close();
        }
    }

    /** The account signed in, or null. */
    static String login(Context c) {
        Tokens t = loadTokens(c);
        return t != null ? t.login : null;
    }

    /** Signs out; the copy (fork) belonged to that account, so it's forgotten too. */
    static void signOut(Context c) {
        saveTokens(c, null);
        state(c).edit().remove("fork").remove("fork_source").remove("fork_login").apply();
    }

    // ---------------------------------------------------------------- signing in (device flow)

    static final class DeviceCode {
        String deviceCode, userCode, verification;
        int interval;
        long expires;  // ms
    }

    static DeviceCode startSignIn(Context c) throws Failure {
        Response r = request("POST", web(c) + "/login/device/code", form("client_id", CLIENT_ID), null, null, "application/json");
        JSONObject o = r.json();
        if (r.code != 200 || o.optString("device_code", "").isEmpty() || o.optString("user_code", "").isEmpty()) {
            String m = o.optString("error_description", r.message());
            throw new Failure(r.code, "GitHub answered " + r.code + (m.isEmpty() ? "" : ": " + m));
        }
        DeviceCode d = new DeviceCode();
        d.deviceCode = o.optString("device_code");
        d.userCode = o.optString("user_code");
        d.verification = o.optString("verification_uri", web(c) + "/login/device");
        d.interval = o.optInt("interval", 5);
        d.expires = System.currentTimeMillis() + o.optInt("expires_in", 900) * 1000L;
        return d;
    }

    interface Cancelled {
        boolean cancelled();
    }

    /**
     * Waits for the code to be entered on GitHub, then keeps the keys: the account's name, or null when cancelled.
     * A request that fails on the way is just asked again: going to the browser to enter the code may leave this
     * app's connection dropped (Doze, a network change) while GitHub has the code all along.
     */
    static String finishSignIn(Context c, DeviceCode d, Cancelled cancel) throws Failure, InterruptedException {
        int interval = Math.max(1, d.interval);
        while (System.currentTimeMillis() < d.expires) {
            for (int i = 0; i < interval * 10; i++) {
                if (cancel.cancelled()) return null;
                Thread.sleep(100);
            }
            Response r;
            try {
                r = request("POST", web(c) + "/login/oauth/access_token", form("client_id", CLIENT_ID, "device_code",
                        d.deviceCode, "grant_type", "urn:ietf:params:oauth:grant-type:device_code"), null, null,
                        "application/json");
            } catch (Failure f) {
                if (f.status == 0) continue;
                throw f;
            }
            JSONObject o = r.json();
            if (o.has("access_token")) return keep(c, o, null, cancel).login;
            String err = o.optString("error", "");
            if (err.equals("authorization_pending")) continue;
            if (err.equals("slow_down")) {
                interval = o.optInt("interval", interval + 5);
                continue;
            }
            if (err.equals("expired_token")) throw new Failure(0, "The code expired before it was entered. Try signing in again.");
            if (err.equals("access_denied")) throw new Failure(0, "Signing in was cancelled on GitHub.");
            if (r.code != 200 && err.isEmpty()) {
                if (r.code >= 500) continue;  // GitHub having a moment: it still has the code
                throw new Failure(r.code, "GitHub answered " + r.code);
            }
            throw new Failure(0, o.optString("error_description", err));
        }
        throw new Failure(0, "The code expired before it was entered. Try signing in again.");
    }

    /** Stores a token answer (sign-in or refresh) with the account's name. */
    private static Tokens keep(Context c, JSONObject o, String knownLogin, Cancelled cancel) throws Failure {
        Tokens t = new Tokens();
        t.access = o.optString("access_token");
        t.refresh = o.optString("refresh_token", "");
        t.expires = System.currentTimeMillis() + o.optLong("expires_in", 8 * 3600) * 1000L;
        t.login = knownLogin != null ? knownLogin : "";
        for (int attempt = 0; attempt < 10 && t.login.isEmpty(); attempt++) {  // the key is good; a dropped connection mustn't lose it
            try {
                Response r = request("GET", api(c) + "/user", null, null, t.access, null);
                String l = r.json().optString("login", "");
                if (r.code != 200 || l.isEmpty()) throw new Failure(r.code, "GitHub answered " + r.code + ": " + r.message());
                t.login = l;
            } catch (Failure f) {
                if (f.status != 0 || attempt == 9) throw f;
                try {
                    Thread.sleep(2000);
                } catch (InterruptedException e) {
                    throw f;
                }
            }
        }
        saveTokens(c, t);
        return t;
    }

    /** Keys good for a while yet: renewed when within 5 minutes of running out (refresh keys change each time). */
    static Tokens tokens(Context c, boolean forceRefresh) throws Failure, SignedOut {
        Tokens t = loadTokens(c);
        if (t == null) throw new SignedOut();
        if (!forceRefresh && t.expires - System.currentTimeMillis() > REFRESH_BEFORE) return t;
        if (t.refresh == null || t.refresh.isEmpty()) {
            saveTokens(c, null);
            throw new SignedOut();
        }
        Response r = request("POST", web(c) + "/login/oauth/access_token",
                form("client_id", CLIENT_ID, "grant_type", "refresh_token", "refresh_token", t.refresh), null, null,
                "application/json");
        JSONObject o = r.json();
        if (!o.has("access_token")) {
            if (r.code >= 500) throw new Failure(r.code, "GitHub answered " + r.code);
            saveTokens(c, null);
            throw new SignedOut();
        }
        return keep(c, o, t.login, null);
    }

    // ---------------------------------------------------------------- publishing

    /** The person's copy of the source ("login/repo"), once made (for this source and account). */
    static String fork(Context c) {
        SharedPreferences s = state(c);
        String f = s.getString("fork", "");
        if (f.isEmpty()) return null;
        if (!Dictionaries.source(c)[0].equals(s.getString("fork_source", ""))) return null;
        String login = login(c);
        if (login == null || !login.equals(s.getString("fork_login", ""))) return null;
        return f;
    }

    static final int PUBLISHED = 0, NOTHING = 1, NEEDS_FORK = 2, NEEDS_INSTALL = 3, FAILED = 4;

    static final class Result {
        final int kind;
        final DictionaryStore.Summary summary;
        final String fork, message;

        Result(int kind, DictionaryStore.Summary summary, String fork, String message) {
            this.kind = kind;
            this.summary = summary;
            this.fork = fork;
            this.message = message;
        }
    }

    private static String path(String name) {
        try {
            return URLEncoder.encode(name, "UTF-8").replace("+", "%20");
        } catch (java.io.UnsupportedEncodingException e) {
            return name;
        }
    }

    /** The file's lines and sha in repo ({lines, sha}); sha null when it isn't there yet. Null: the key was refused. */
    private static Object[] fetch(Context c, String name, String repo, String branch, String token) throws Failure {
        Response r = request("GET", api(c) + "/repos/" + repo + "/contents/" + path(name) + "?ref=" + path(branch), null,
                null, token, null);
        if (r.code == 401) return null;
        if (r.code == 404) return new Object[] {new ArrayList<String>(), null};
        if (r.code != 200) throw new Failure(r.code, "GitHub answered " + r.code + ": " + r.message());
        JSONObject o = r.json();
        String sha = o.optString("sha", null);
        String content = o.optString("content", "");
        String text;
        if (!content.isEmpty()) {
            text = new String(Base64.decode(content.replace("\n", ""), Base64.DEFAULT), UTF8);
        } else {  // over 1 MB: the API leaves the content out
            Response raw = request("GET", o.optString("download_url"), null, null, token, "application/vnd.github.raw");
            if (raw.code != 200) throw new Failure(raw.code, "GitHub answered " + raw.code);
            text = new String(raw.body, UTF8);
        }
        return new Object[] {DictionaryStore.splitLines(text), sha};
    }

    private static final int DONE = 0, CONFLICT = 1, FORBIDDEN = 2, REFUSED = 3;

    private static int put(Context c, String name, String repo, String branch, String text, String sha, String message,
                           String token) throws Failure {
        JSONObject body = new JSONObject();
        try {
            body.put("message", message);
            body.put("content", Base64.encodeToString(text.getBytes(UTF8), Base64.NO_WRAP));
            body.put("branch", branch);
            if (sha != null) body.put("sha", sha);
        } catch (org.json.JSONException e) {
            throw new Failure(0, e.getMessage());
        }
        Response r = request("PUT", api(c) + "/repos/" + repo + "/contents/" + path(name), null, body, token, null);
        switch (r.code) {
            case 200: case 201: return DONE;
            case 409: return CONFLICT;
            case 401: return REFUSED;
            case 403: case 404: return FORBIDDEN;
            case 422: if (r.message().toLowerCase(java.util.Locale.ROOT).contains("sha")) return CONFLICT;
            // fall through
            default: throw new Failure(r.code, "GitHub answered " + r.code + ": " + r.message());
        }
    }

    /** Publishes every changed file to the source (or the person's copy of it). */
    static Result publish(Context c) {
        Map<String, DictionaryStore.Changes> todo = DictionaryStore.pending(c);
        if (todo.isEmpty()) return new Result(NOTHING, null, null, null);
        String[] source = Dictionaries.source(c);
        String fork = fork(c);
        String target = fork != null ? fork : source[0];
        DictionaryStore.Summary summary = new DictionaryStore.Summary();
        Map<String, String> written = new LinkedHashMap<>();
        try {
            Tokens t = tokens(c, false);
            boolean renewed = false;
            for (Map.Entry<String, DictionaryStore.Changes> e : todo.entrySet()) {
                String name = e.getKey();
                DictionaryStore.Changes ch = e.getValue();
                boolean finished = false;
                for (int attempt = 0; attempt < 3 && !finished; ) {  // someone else committed meanwhile: on their new copy
                    Object[] got = fetch(c, name, target, source[1], t.access);
                    int outcome;
                    String text = null;
                    if (got == null) {
                        outcome = REFUSED;
                    } else {
                        @SuppressWarnings("unchecked") List<String> lines = (List<String>) got[0];
                        List<String> now = DictionaryStore.applyChanges(lines, ch);
                        if (now.equals(lines)) {
                            finished = true;
                            continue;
                        }
                        StringBuilder b = new StringBuilder();
                        for (String l : now) b.append(l).append('\n');
                        text = b.toString();
                        outcome = put(c, name, target, source[1], text, (String) got[1], DictionaryStore.message(name, ch), t.access);
                    }
                    switch (outcome) {
                        case DONE:
                            written.put(name, text);
                            summary.add(name, ch);
                            finished = true;
                            break;
                        case CONFLICT:
                            attempt++;
                            break;
                        case REFUSED:  // the key was refused: once more with a new one
                            if (renewed) throw new SignedOut();
                            renewed = true;
                            t = tokens(c, true);
                            break;
                        default:  // FORBIDDEN
                            if (fork == null && summary.files.isEmpty()) return new Result(NEEDS_FORK, null, null, null);
                            throw new Failure(403, "GitHub doesn't let you write to " + target + ".");
                    }
                }
                if (!finished) throw new Failure(409, name + " kept changing on GitHub; try again.");
            }
        } catch (Failure | SignedOut e) {
            if (!summary.files.isEmpty()) finishSource(c, written, target.equals(source[0]));
            return new Result(FAILED, null, null, e.getMessage());
        }
        if (summary.files.isEmpty()) return new Result(NOTHING, null, null, null);
        finishSource(c, written, target.equals(source[0]));
        return new Result(PUBLISHED, summary, target.equals(source[0]) ? null : target, null);
    }

    /** What went to the source is now the fetched copy; the person's published changes drop out of Mine. */
    private static void finishSource(Context c, Map<String, String> written, boolean toSource) {
        if (!toSource) return;
        for (Map.Entry<String, String> e : written.entrySet()) {
            try {
                Dictionaries.writeText(new File(Dictionaries.dir(c), e.getKey()), e.getValue());
            } catch (IOException x) {
                // fetched again at the next check
            }
        }
        DictionaryStore.tidy(c);
    }

    /** Makes the person's copy of the source and remembers it (waits until GitHub has it ready). */
    static Result makeFork(Context c) {
        String[] source = Dictionaries.source(c);
        try {
            Tokens t = tokens(c, false);
            JSONObject body = new JSONObject();
            try {
                body.put("default_branch_only", true);
            } catch (org.json.JSONException e) {
                throw new Failure(0, e.getMessage());
            }
            Response r = request("POST", api(c) + "/repos/" + source[0] + "/forks", null, body, t.access, null);
            if (r.code == 401) {
                t = tokens(c, true);
                r = request("POST", api(c) + "/repos/" + source[0] + "/forks", null, body, t.access, null);
            }
            if (r.code == 403 || r.code == 404) return new Result(NEEDS_INSTALL, null, null, null);
            String full = r.json().optString("full_name", "");
            if ((r.code != 202 && r.code != 200) || full.isEmpty()) {
                throw new Failure(r.code, "GitHub answered " + r.code + ": " + r.message());
            }
            for (int i = 0; i < 30; i++) {  // forking happens in the background
                t = tokens(c, false);
                Response g = request("GET", api(c) + "/repos/" + full + "/contents/?ref=" + path(source[1]), null, null,
                        t.access, null);
                if (g.code == 200) {
                    state(c).edit().putString("fork", full).putString("fork_source", source[0])
                            .putString("fork_login", t.login == null ? "" : t.login).apply();
                    return new Result(NOTHING, null, full, null);
                }
                Thread.sleep(2000);
            }
            return new Result(FAILED, null, null, "GitHub is still making your copy. Try publishing again in a minute.");
        } catch (Failure | SignedOut e) {
            return new Result(FAILED, null, null, e.getMessage());
        } catch (InterruptedException e) {
            return new Result(FAILED, null, null, "Stopped.");
        }
    }
}
