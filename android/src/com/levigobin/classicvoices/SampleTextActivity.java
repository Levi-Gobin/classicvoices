package com.levigobin.classicvoices;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;

import java.util.Locale;

/** The sentence Android's settings play to demonstrate a language (ACTION_GET_SAMPLE_TEXT). */
public class SampleTextActivity extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        String lang = getIntent().getStringExtra("language");
        Locale l = lang != null ? new Locale(lang.length() == 3 ? iso2(lang) : lang) : Locale.getDefault();
        Intent out = new Intent();
        out.putExtra(TextToSpeech.Engine.EXTRA_SAMPLE_TEXT, Samples.forLanguage(l));
        setResult(TextToSpeech.LANG_AVAILABLE, out);
        finish();
    }

    private static String iso2(String iso3) {
        for (String l : Locale.getISOLanguages()) {
            try {
                if (new Locale(l).getISO3Language().equals(iso3)) return l;
            } catch (Exception e) {
                // no ISO 639-2 code
            }
        }
        return iso3;
    }
}
