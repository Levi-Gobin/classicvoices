package com.levigobin.classicvoices;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.Set;

/** Answers the system's "which languages does this engine have?" (ACTION_CHECK_TTS_DATA). */
public class CheckDataActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Set<String> langs = new LinkedHashSet<>();
        for (VoiceInfo v : Data.refresh(this)) {
            String l = Data.iso3(v.locale, true), c = Data.iso3(v.locale, false);
            langs.add(c.isEmpty() ? l : l + "-" + c);
        }
        Intent out = new Intent();
        out.putStringArrayListExtra(TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, new ArrayList<>(langs));
        out.putStringArrayListExtra(TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES, new ArrayList<String>());
        setResult(langs.isEmpty() ? TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL : TextToSpeech.Engine.CHECK_VOICE_DATA_PASS, out);
        finish();
    }
}
