package com.levigobin.classicvoices;

import android.media.AudioFormat;
import android.os.Build;
import android.speech.tts.SynthesisCallback;
import android.speech.tts.SynthesisRequest;
import android.speech.tts.TextToSpeech;
import android.speech.tts.TextToSpeechService;
import android.speech.tts.Voice;
import android.util.Log;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Classic Voices as an Android text-to-speech engine: TalkBack and every app
 * that speaks can use the iOS 3-6 and Mac OS X Vocalizer voices.
 */
public class TtsService extends TextToSpeechService {
    private static final String TAG = "ClassicVoices";

    private volatile boolean stopped;
    private volatile VoiceInfo current;

    @Override
    public void onCreate() {
        super.onCreate();
        Data.refresh(this);
    }

    // ---------------------------------------------------------------- languages

    @Override
    protected int onIsLanguageAvailable(String lang, String country, String variant) {
        int best = TextToSpeech.LANG_NOT_SUPPORTED;
        for (VoiceInfo v : Data.voices(this)) {
            int m = Data.match(v, lang, country);
            if (m == 3) return TextToSpeech.LANG_COUNTRY_AVAILABLE;
            if (m == 2) best = TextToSpeech.LANG_AVAILABLE;
        }
        return best;
    }

    @Override
    protected String[] onGetLanguage() {
        VoiceInfo v = current;
        if (v == null) v = Data.defaultFor(this, "eng", "USA");
        if (v == null) return new String[] {"", "", ""};
        return new String[] {Data.iso3(v.locale, true), Data.iso3(v.locale, false), ""};
    }

    @Override
    protected int onLoadLanguage(String lang, String country, String variant) {
        int r = onIsLanguageAvailable(lang, country, variant);
        if (r >= TextToSpeech.LANG_AVAILABLE) {
            VoiceInfo v = Data.defaultFor(this, lang, country);
            if (v != null) {
                current = v;
                Native.preload(v.key);
            }
        }
        return r;
    }

    // ---------------------------------------------------------------- voices (Android 5+)

    @Override
    public List<Voice> onGetVoices() {
        List<Voice> out = new ArrayList<>();
        for (VoiceInfo v : Data.voices(this)) out.add(toVoice(v));
        return out;
    }

    private static Voice toVoice(VoiceInfo v) {
        Set<String> features = new HashSet<>();
        features.add(TextToSpeech.Engine.KEY_FEATURE_EMBEDDED_SYNTHESIS);
        return new Voice(v.androidName(), v.locale,
                v.quality >= 2 ? Voice.QUALITY_HIGH : Voice.QUALITY_NORMAL,
                Voice.LATENCY_LOW, false, features);
    }

    @Override
    public int onIsValidVoiceName(String name) {
        return Data.byAndroidName(this, name) != null ? TextToSpeech.SUCCESS : TextToSpeech.ERROR;
    }

    @Override
    public int onLoadVoice(String name) {
        VoiceInfo v = Data.byAndroidName(this, name);
        if (v == null) return TextToSpeech.ERROR;
        current = v;
        Native.preload(v.key);
        return TextToSpeech.SUCCESS;
    }

    @Override
    public String onGetDefaultVoiceNameFor(String lang, String country, String variant) {
        VoiceInfo v = Data.defaultFor(this, lang, country);
        return v != null ? v.androidName() : null;
    }

    // ---------------------------------------------------------------- speaking

    @Override
    protected void onStop() {
        stopped = true;
        Native.cancel();
    }

    @Override
    protected void onSynthesizeText(SynthesisRequest request, SynthesisCallback cb) {
        stopped = false;
        VoiceInfo v = null;
        if (Build.VERSION.SDK_INT >= 21) v = Data.byAndroidName(this, request.getVoiceName());
        if (v == null) v = Data.defaultFor(this, request.getLanguage(), request.getCountry());
        if (v == null) {
            Log.w(TAG, "no voice for " + request.getLanguage() + "-" + request.getCountry());
            cb.error();
            return;
        }
        current = v;
        CharSequence cs = Build.VERSION.SDK_INT >= 21 ? request.getCharSequenceText() : null;
        String text = cs != null ? cs.toString() : request.getText();
        if (text == null) text = "";
        speak(this, v, text, request.getSpeechRate(), request.getPitch(), cb);
    }

    private void speak(TtsService s, VoiceInfo v, String text, int rate, int pitch, SynthesisCallback cb) {
        if (Data.rulesEnabled(s)) {
            Rules r = Rules.forVoice(s, v);
            if (r != null) text = r.apply(text);
        }
        String fallback = Data.fallbackEnabled(s) ? v.fallback : "";
        byte[] utf8;
        try {
            utf8 = text.getBytes("UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            cb.error();
            return;
        }
        // Android: 100 = normal for both; TalkBack goes well past 400% rate.
        int r = Math.max(50, Math.min(400, rate));
        int p = Math.max(50, Math.min(200, pitch));
        if (cb.start(Native.SAMPLE_RATE, AudioFormat.ENCODING_PCM_16BIT, 1) != TextToSpeech.SUCCESS) return;
        if (Native.begin(v.key, fallback, utf8, false, r, p, -1) < 0) {
            Log.w(TAG, "engine not ready: " + Native.lastError());
            cb.error();
            return;
        }
        int max = cb.getMaxBufferSize();
        byte[] buf = new byte[Math.max(2, Math.min(max, 4410) & ~1)];
        while (!stopped) {
            int n = Native.read(buf);
            if (n < 0) break;
            if (n == 0) continue;
            if (cb.audioAvailable(buf, 0, n) != TextToSpeech.SUCCESS) {
                Native.cancel();
                break;
            }
        }
        if (stopped) return;  // the framework reports the stop itself
        cb.done();
    }
}
