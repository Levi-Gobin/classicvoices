package com.levigobin.classicvoices;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.speech.tts.TextToSpeech;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.List;
import java.util.Locale;

/** The app's one main screen: status, imports, downloads, settings. */
public class MainActivity extends Activity implements ImportService.Listener {
    private static final int PICK_IPSW = 1, PICK_PACK = 2;

    private TextView summary, status, nativeInfo;
    private ProgressBar bar;
    private Button cancel;
    private LinearLayout firmwareList;
    private TextToSpeech tts;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle(R.string.app_name);
        ScrollView scroll = new ScrollView(this);
        LinearLayout col = Ui.column(this);
        scroll.addView(col);
        setContentView(scroll);

        summary = Ui.text(this, col, "");
        Ui.button(this, col, getString(R.string.open_tts_settings), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openTtsSettings();
            }
        });

        // progress of imports, announced as it changes
        status = Ui.text(this, col, "");
        if (Build.VERSION.SDK_INT >= 19) status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        col.addView(bar);
        cancel = Ui.button(this, col, getString(R.string.cancel_imports), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ImportService.cancel();
            }
        });

        Ui.heading(this, col, getString(R.string.heading_firmware));
        Ui.text(this, col, getString(R.string.firmware_explained));
        firmwareList = new LinearLayout(this);
        firmwareList.setOrientation(LinearLayout.VERTICAL);
        col.addView(firmwareList);
        Ui.button(this, col, getString(R.string.import_all_firmware), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                for (Firmware.Build b : Firmware.BUILDS) {
                    if (!Data.hasEngine(MainActivity.this, b.key)) enqueueFirmware(b);
                }
            }
        });
        Ui.button(this, col, getString(R.string.import_ipsw_file), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_IPSW, "*/*");
            }
        });

        Ui.heading(this, col, getString(R.string.heading_more_voices));
        Ui.text(this, col, getString(R.string.more_voices_explained));
        Ui.button(this, col, getString(R.string.download_voices), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(MainActivity.this, DownloadsActivity.class));
            }
        });

        Ui.heading(this, col, getString(R.string.heading_your_voices));
        Ui.button(this, col, getString(R.string.choose_voices), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(MainActivity.this, VoicesActivity.class));
            }
        });
        Ui.button(this, col, getString(R.string.speak_sample), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                speakSample();
            }
        });

        Ui.heading(this, col, getString(R.string.heading_settings));
        checkbox(col, R.string.setting_rules, "rules");
        checkbox(col, R.string.setting_fallback, "pitch_fallback");

        Ui.heading(this, col, getString(R.string.heading_fast));
        nativeInfo = Ui.text(this, col, "");
        Ui.button(this, col, getString(R.string.import_pack), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_PACK, "application/zip");
            }
        });

        Ui.heading(this, col, getString(R.string.heading_macintalk));
        Ui.text(this, col, getString(R.string.macintalk_explained));

        // test hooks (adb shell am start ... --es import ios6,ios3 / --es selftest ios6/en-US), in
        // debuggable builds only: the activity is exported, and a pack can bring native code
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) testHooks();

        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 0);
        }
    }

    private void testHooks() {
        String imports = getIntent().getStringExtra("import");
        if (imports != null) {
            for (String k : imports.split(",")) {
                Firmware.Build b = Firmware.byKey(k.trim());
                if (b != null) enqueueFirmware(b);
            }
        }
        String download = getIntent().getStringExtra("download");
        if (download != null) {
            ImportService.enqueue(this, new ImportService.Job("download", download, download));
        }
        String pack = getIntent().getStringExtra("pack");  // a zip the app can read, e.g. in its cache
        if (pack != null) {
            ImportService.enqueue(this, new ImportService.Job("pack", android.net.Uri.fromFile(new File(pack)).toString(),
                    getString(R.string.the_pack)));
        }
        String self = getIntent().getStringExtra("selftest");
        if (self != null) {
            String text = getIntent().getStringExtra("text");
            SelfTest.run(this, self, text != null ? text : "Hello from Android. This is a test of the Classic Voices engine.");
        }
    }

    private void checkbox(LinearLayout col, int label, final String key) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setTextSize(16);
        cb.setChecked(Data.prefs(this).getBoolean(key, true));
        cb.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                Data.prefs(MainActivity.this).edit().putBoolean(key, on).apply();
            }
        });
        col.addView(cb);
    }

    @Override
    protected void onResume() {
        super.onResume();
        ImportService.addListener(this);
        refresh();
    }

    @Override
    protected void onPause() {
        ImportService.removeListener(this);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (tts != null) tts.shutdown();
        super.onDestroy();
    }

    private void refresh() {
        List<VoiceInfo> voices = Data.refresh(this);
        String engine = Settings.Secure.getString(getContentResolver(), "tts_default_synth");
        boolean isDefault = getPackageName().equals(engine);
        summary.setText(voices.isEmpty() ? getString(R.string.summary_none)
                : getString(isDefault ? R.string.summary_default : R.string.summary_not_default, voices.size()));

        firmwareList.removeAllViews();
        for (final Firmware.Build b : Firmware.BUILDS) {
            boolean have = Data.hasEngine(this, b.key);
            String label;
            if (have) label = getString(R.string.firmware_imported, b.nice);
            else label = getString(R.string.firmware_import, b.nice, Ui.mb(b.size));
            final boolean again = have;
            Ui.button(this, firmwareList, label + "\n" + b.purpose, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    if (!again) {
                        enqueueFirmware(b);
                        return;
                    }
                    new android.app.AlertDialog.Builder(MainActivity.this)
                            .setMessage(getString(R.string.reimport_question, b.nice, Ui.mb(b.size)))
                            .setPositiveButton(R.string.reimport_yes, new android.content.DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(android.content.DialogInterface d, int w) {
                                    enqueueFirmware(b);
                                }
                            })
                            .setNegativeButton(android.R.string.cancel, null)
                            .show();
                }
            });
        }

        StringBuilder n = new StringBuilder();
        File[] libs = Data.nativeDir(this).listFiles();
        if (libs != null) {
            for (File f : libs) {
                String name = f.getName();
                if (name.startsWith("cvnative_") && name.endsWith(".so")) {
                    if (n.length() > 0) n.append(", ");
                    n.append(name.substring(9, name.length() - 3));
                }
            }
        }
        nativeInfo.setText(n.length() > 0 ? getString(R.string.fast_present, n.toString(), Data.abi())
                : getString(R.string.fast_none, Data.abi()));
        showStatus(ImportService.busy(), ImportService.status(), -1);
    }

    private void enqueueFirmware(Firmware.Build b) {
        ImportService.enqueue(this, new ImportService.Job("firmware", b.key, b.nice));
        showStatus(true, getString(R.string.import_queued, b.nice), -1);
    }

    private void pick(int code, String type) {
        Intent i = new Intent(Build.VERSION.SDK_INT >= 19 ? Intent.ACTION_OPEN_DOCUMENT : Intent.ACTION_GET_CONTENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(type);
        try {
            startActivityForResult(i, code);
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, R.string.no_file_picker, Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        String uri = data.getData().toString();
        if (code == PICK_IPSW) ImportService.enqueue(this, new ImportService.Job("ipsw", uri, getString(R.string.the_ipsw)));
        else if (code == PICK_PACK) ImportService.enqueue(this, new ImportService.Job("pack", uri, getString(R.string.the_pack)));
        showStatus(true, getString(R.string.import_starting), -1);
    }

    private void openTtsSettings() {
        try {
            startActivity(new Intent("com.android.settings.TTS_SETTINGS"));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_SETTINGS));
        }
    }

    private void speakSample() {
        if (Data.voices(this).isEmpty()) {
            Toast.makeText(this, R.string.summary_none, Toast.LENGTH_LONG).show();
            return;
        }
        if (tts != null) {
            tts.speak(getString(R.string.sample_text), TextToSpeech.QUEUE_FLUSH, null);
            return;
        }
        tts = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int s) {
                if (s == TextToSpeech.SUCCESS) {
                    tts.setLanguage(Locale.getDefault());
                    tts.speak(getString(R.string.sample_text), TextToSpeech.QUEUE_FLUSH, null);
                }
            }
        }, getPackageName());
    }

    @Override
    public void onImportStatus(boolean busy, String message, double fraction, String finished, String error) {
        if (!busy || finished != null || error != null) refresh();
        showStatus(busy, message, fraction);
    }

    private void showStatus(boolean busy, String message, double fraction) {
        if (message != null && !message.isEmpty()) status.setText(message);
        bar.setVisibility(busy ? View.VISIBLE : View.GONE);
        cancel.setVisibility(busy ? View.VISIBLE : View.GONE);
        bar.setIndeterminate(fraction < 0);
        if (fraction >= 0) bar.setProgress((int) (fraction * 1000));
        if (busy && fraction >= 0) {
            bar.setContentDescription(getString(R.string.percent, (int) (fraction * 100)));
        }
    }
}
