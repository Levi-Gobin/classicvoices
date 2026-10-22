package com.levigobin.classicvoices;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;
import android.speech.tts.Voice;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import java.util.List;
import java.util.Locale;

/** Every voice, grouped by language: tap one to make it that language's default and hear it. */
public class VoicesActivity extends Activity {
    private LinearLayout list;
    private TextToSpeech tts;
    private boolean ready;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle(R.string.choose_voices);
        ScrollView scroll = new ScrollView(this);
        LinearLayout col = Ui.column(this);
        scroll.addView(col);
        setContentView(scroll);
        Ui.text(this, col, getString(R.string.voices_explained));
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        col.addView(list);
        tts = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int s) {
                ready = s == TextToSpeech.SUCCESS;
            }
        }, getPackageName());
        fill();
    }

    @Override
    protected void onDestroy() {
        tts.shutdown();
        super.onDestroy();
    }

    private void fill() {
        list.removeAllViews();
        List<VoiceInfo> voices = Data.refresh(this);
        if (voices.isEmpty()) {
            Ui.text(this, list, getString(R.string.summary_none));
            return;
        }
        String lastLang = null;
        for (final VoiceInfo v : voices) {
            String langName = v.locale.getDisplayName();
            if (!v.language.equals(lastLang)) {
                Ui.heading(this, list, langName);
                lastLang = v.language;
            }
            VoiceInfo def = Data.defaultFor(this, v.locale.getLanguage(), v.locale.getCountry());
            boolean isDefault = def != null && def.key.equals(v.key);
            String label = v.name + (isDefault ? ", " + getString(R.string.voice_default) : "");
            Ui.button(this, list, label, new View.OnClickListener() {
                @Override
                public void onClick(View b) {
                    Data.prefs(VoicesActivity.this).edit().putString("default_" + v.language, v.key).apply();
                    speak(v);
                    fill();
                }
            });
        }
    }

    private void speak(VoiceInfo v) {
        if (!ready) return;
        if (Build.VERSION.SDK_INT >= 21) {
            for (Voice x : tts.getVoices()) {
                if (x.getName().equals(v.androidName())) {
                    tts.setVoice(x);
                    break;
                }
            }
        } else {
            tts.setLanguage(v.locale);
        }
        tts.speak(Samples.forLanguage(v.locale) + " " + v.voice + ".", TextToSpeech.QUEUE_FLUSH, null);
    }

}
