package com.levigobin.classicvoices;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Runs imports and downloads one after another in the foreground (with a
 * notification), so they carry on when the app is in the background or the
 * screen is off. The open screen follows along through Listener.
 */
public class ImportService extends Service {
    interface Listener {
        /** On the main thread. busy == false when the queue has emptied. */
        void onImportStatus(boolean busy, String message, double fraction, String finished, String error);
    }

    static final class Job {
        final String kind;   // "firmware", "ipsw", "download", "pack"
        final String arg;    // build key, download id, or content Uri
        final String label;

        Job(String kind, String arg, String label) {
            this.kind = kind;
            this.arg = arg;
            this.label = label;
        }
    }

    private static final String CHANNEL = "imports";
    private static final Object LOCK = new Object();
    private static final LinkedList<Job> QUEUE = new LinkedList<>();
    private static final List<Listener> LISTENERS = new ArrayList<>();
    private static volatile boolean running, cancelAll;
    private static volatile String lastMessage = "";
    private static volatile double lastFraction = -1;
    private static volatile Job currentJob;

    private final Handler main = new Handler(Looper.getMainLooper());
    private PowerManager.WakeLock wake;

    static void enqueue(Context c, Job j) {
        synchronized (LOCK) {
            for (Job q : QUEUE) if (q.kind.equals(j.kind) && q.arg.equals(j.arg)) return;
            if (currentJob != null && currentJob.kind.equals(j.kind) && currentJob.arg.equals(j.arg)) return;
            QUEUE.add(j);
        }
        Intent i = new Intent(c, ImportService.class);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
        else c.startService(i);
    }

    static void cancel() {
        synchronized (LOCK) {
            QUEUE.clear();
        }
        cancelAll = true;
    }

    static boolean busy() { return running; }

    static int queued() {
        synchronized (LOCK) {
            return QUEUE.size() + (currentJob != null ? 1 : 0);
        }
    }

    static String status() { return lastMessage; }

    static void addListener(Listener l) {
        synchronized (LISTENERS) {
            LISTENERS.add(l);
        }
    }

    static void removeListener(Listener l) {
        synchronized (LISTENERS) {
            LISTENERS.remove(l);
        }
    }

    private void tell(final boolean busy, final String msg, final double f, final String finished, final String error) {
        if (msg != null) lastMessage = msg;
        lastFraction = f;
        main.post(new Runnable() {
            @Override
            public void run() {
                List<Listener> ls;
                synchronized (LISTENERS) {
                    ls = new ArrayList<>(LISTENERS);
                }
                for (Listener l : ls) l.onImportStatus(busy, msg, f, finished, error);
                updateNotification(busy, msg != null ? msg : lastMessage, f);
            }
        });
    }

    // ---------------------------------------------------------------- service

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = notification(getString(R.string.import_preparing), -1);
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        else startForeground(1, n);
        synchronized (LOCK) {
            if (running) return START_NOT_STICKY;
            running = true;
        }
        cancelAll = false;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ClassicVoices:import");
        wake.acquire(6 * 60 * 60 * 1000L);
        new Thread(new Runnable() {
            @Override
            public void run() {
                work();
            }
        }, "ClassicVoices import").start();
        return START_NOT_STICKY;
    }

    private void work() {
        while (true) {
            Job j;
            synchronized (LOCK) {
                j = QUEUE.poll();
                currentJob = j;
                if (j == null) {
                    running = false;
                    break;
                }
            }
            String error = null;
            try {
                run(j);
            } catch (Progress.Cancelled e) {
                error = getString(R.string.import_cancelled);
            } catch (Exception e) {
                error = j.label + ": " + (e.getMessage() != null ? e.getMessage() : e.toString());
            }
            Data.refresh(this);
            tell(true, error != null ? error : j.label + " " + getString(R.string.import_done_suffix), -1,
                    error == null ? j.label : null, error);
            if (cancelAll) {
                synchronized (LOCK) {
                    QUEUE.clear();
                }
            }
        }
        currentJob = null;
        tell(false, null, -1, null, null);
        if (wake != null && wake.isHeld()) wake.release();
        main.post(new Runnable() {
            @Override
            public void run() {
                if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_DETACH);
                else stopForeground(false);
                notifyFinished();
                stopSelf();
            }
        });
    }

    private void run(Job j) throws IOException {
        Progress p = new Progress() {
            String step = "";

            @Override
            public void step(String message) {
                step = message;
                tell(true, message, -1, null, null);
            }

            @Override
            public void fraction(double f) {
                tell(true, step, f, null, null);
            }

            @Override
            public boolean cancelled() {
                return cancelAll;
            }
        };
        switch (j.kind) {
            case "firmware":
                Firmware.importBuild(this, Firmware.byKey(j.arg), null, p);
                break;
            case "ipsw":
                importIpsw(Uri.parse(j.arg), p);
                break;
            case "download":
                for (Downloads.Item it : Downloads.available(this)) {
                    if (!it.id().equals(j.arg)) continue;
                    if (it.kind.equals("enhanced")) Downloads.downloadEnhanced(this, it, p);
                    else Downloads.downloadMac(this, it, p);
                    return;
                }
                throw new IOException(getString(R.string.import_not_available));
            case "pack":
                importPack(Uri.parse(j.arg), p);
                break;
            default:
                throw new IOException("unknown job " + j.kind);
        }
    }

    private void importIpsw(Uri uri, Progress p) throws IOException {
        ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r");
        if (fd == null) throw new IOException(getString(R.string.import_cant_open));
        FileInputStream in = new FileInputStream(fd.getFileDescriptor());
        Source s = new Source.FileSrc(in);
        try {
            String build = Firmware.identify(s);
            Firmware.Build b = build != null ? Firmware.byBuild(build) : null;
            if (b == null) {
                throw new IOException(build == null ? getString(R.string.import_not_ipsw)
                        : getString(R.string.import_wrong_build, build));
            }
            Firmware.importBuild(this, b, s, p);
        } finally {
            in.close();
            fd.close();
        }
    }

    /**
     * A zip with Voices/, Downloads/ and Native/ folders (the same layout as
     * the app's data folder): from the Linux importer, from another phone, or
     * from your own GitHub build of the fast engine code.
     */
    private void importPack(Uri uri, Progress p) throws IOException {
        InputStream raw = getContentResolver().openInputStream(uri);
        if (raw == null) throw new IOException(getString(R.string.import_cant_open));
        File root = Data.root(this);
        int files = 0;
        p.step(getString(R.string.import_unpacking));
        try {
            ZipInputStream z = new ZipInputStream(new java.io.BufferedInputStream(raw, 1 << 16));
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                if (cancelAll) throw new Progress.Cancelled();
                String name = e.getName().replace('\\', '/');
                // accept "Voices/..." anywhere in the path (a zip of the folder, or of its parent)
                int at = -1;
                for (String top : new String[] {"Voices/", "Downloads/", "Native/"}) {
                    int slash = name.indexOf("/" + top);
                    int k = name.startsWith(top) ? 0 : slash >= 0 ? slash + 1 : -1;
                    if (k >= 0 && (at < 0 || k < at)) at = k;
                }
                if (at < 0 || e.isDirectory() || name.contains("/../") || name.contains("__MACOSX")) continue;
                String rel = name.substring(at);
                if (rel.contains("/.") || rel.endsWith(".DS_Store")) continue;
                File dest = new File(root, rel);
                //noinspection ResultOfMethodCallIgnored
                dest.getParentFile().mkdirs();
                OutputStream out = new FileOutputStream(dest);
                try {
                    int n;
                    while ((n = z.read(buf)) > 0) out.write(buf, 0, n);
                } finally {
                    out.close();
                }
                if (rel.startsWith("Native/")) {
                    //noinspection ResultOfMethodCallIgnored
                    dest.setWritable(false, false);  // Android 14+ loads code only from read-only files
                }
                files++;
                if (files % 20 == 0) p.step(getString(R.string.import_unpacked_n, files));
            }
        } finally {
            raw.close();
        }
        if (files == 0) throw new IOException(getString(R.string.import_pack_empty));
        Firmware.assembleMacCommon(Data.voicesDir(this));
    }

    // ---------------------------------------------------------------- notification

    private Notification notification(String text, double f) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            if (nm.getNotificationChannel(CHANNEL) == null) {
                nm.createNotificationChannel(new NotificationChannel(CHANNEL, getString(R.string.import_channel),
                        NotificationManager.IMPORTANCE_LOW));
            }
            b = new Notification.Builder(this, CHANNEL);
        } else {
            b = new Notification.Builder(this);
        }
        int flags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), flags);
        b.setSmallIcon(R.drawable.ic_notify).setContentTitle(getString(R.string.app_name)).setContentText(text)
                .setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true);
        if (f >= 0) b.setProgress(1000, (int) (f * 1000), false);
        else b.setProgress(0, 0, true);
        return b.build();
    }

    private long lastNotify;

    private void updateNotification(boolean busy, String text, double f) {
        if (!busy) return;
        long now = System.currentTimeMillis();
        if (f >= 0 && now - lastNotify < 1000) return;  // the system rate-limits updates
        lastNotify = now;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        nm.notify(1, notification(text, f));
    }

    private void notifyFinished() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        int flags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        b.setSmallIcon(R.drawable.ic_notify).setContentTitle(getString(R.string.app_name)).setContentText(lastMessage)
                .setAutoCancel(true)
                .setContentIntent(PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), flags));
        nm.notify(1, b.build());
    }
}
