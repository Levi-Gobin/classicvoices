package com.levigobin.classicvoices;

import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;
import android.speech.tts.UtteranceProgressListener;
import android.speech.tts.Voice;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Test hook (adb): speaks a sentence with each named voice through Android's
 * own TextToSpeech API into WAV files, and logs how long each took:
 *
 *   adb shell am start -n com.levigobin.classicvoices/.MainActivity --es selftest "ios6/en-US,ios3/en-US"
 *   adb logcat -s CVSELFTEST
 *
 * Files go to the app's cache folder (selftest-<n>.wav).
 */
final class SelfTest {
    private static final String TAG = "CVSELFTEST";

    static void run(final Context c, final String keys, final String text) {
        final List<String> want = new ArrayList<>();
        for (String k : keys.split(",")) if (!k.trim().isEmpty()) want.add(k.trim());
        final TextToSpeech[] holder = new TextToSpeech[1];
        holder[0] = new TextToSpeech(c, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int status) {
                if (status != TextToSpeech.SUCCESS) {
                    Log.e(TAG, "engine init failed " + status);
                    return;
                }
                final TextToSpeech tts = holder[0];
                final long[] started = new long[want.size()];
                final long[] first = new long[want.size()];
                tts.setOnUtteranceProgressListener(new UtteranceProgressListener() {
                    @Override
                    public void onStart(String id) {
                        started[Integer.parseInt(id)] = System.nanoTime();
                    }

                    @Override
                    public void onAudioAvailable(String id, byte[] audio) {
                        int i = Integer.parseInt(id);
                        if (first[i] == 0) first[i] = System.nanoTime();
                    }

                    @Override
                    public void onDone(String id) {
                        int i = Integer.parseInt(id);
                        File f = new File(c.getCacheDir(), "selftest-" + i + ".wav");
                        double ms = (System.nanoTime() - started[i]) / 1e6;
                        double audioMs = Math.max(0, f.length() - 44) / 2.0 / Native.SAMPLE_RATE * 1000;
                        Log.i(TAG, String.format(java.util.Locale.ROOT,
                                "%s: ok %.0f ms of audio in %.0f ms (%.1fx real time), first audio %.0f ms, file %s",
                                want.get(i), audioMs, ms, ms > 0 ? audioMs / ms : 0,
                                first[i] > 0 ? (first[i] - started[i]) / 1e6 : -1, f.getName()));
                        if (i == want.size() - 1) {
                            Log.i(TAG, "done");
                            tts.shutdown();
                        }
                    }

                    @Override
                    public void onError(String id) {
                        Log.e(TAG, want.get(Integer.parseInt(id)) + ": error " + Native.lastError());
                        if (Integer.parseInt(id) == want.size() - 1) tts.shutdown();
                    }
                });
                for (int i = 0; i < want.size(); i++) {
                    VoiceInfo v = Data.byAndroidName(c, want.get(i));
                    if (v == null) {
                        Log.e(TAG, want.get(i) + ": no such voice");
                        continue;
                    }
                    if (Build.VERSION.SDK_INT >= 21) {
                        for (Voice x : tts.getVoices()) if (x.getName().equals(v.androidName())) tts.setVoice(x);
                        tts.synthesizeToFile(text, new Bundle(), new File(c.getCacheDir(), "selftest-" + i + ".wav"),
                                String.valueOf(i));
                    }
                }
            }
        }, c.getPackageName());
    }
}
