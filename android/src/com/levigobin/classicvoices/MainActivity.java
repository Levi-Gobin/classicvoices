package com.levigobin.classicvoices;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.speech.tts.TextToSpeech;
import android.speech.tts.Voice;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * The app, laid out as on the iPhone, the Mac, Windows and Linux: an iOS tab,
 * a macOS tab and Settings. A tab goes from a version and a language down to
 * each voice, one page at a time under a Back button and the path walked.
 * Every row is a button that says what pressing it does before you press it;
 * moving TalkBack onto one never opens it.
 */
public class MainActivity extends Activity implements ImportService.Listener {
    private static final int PICK_IPSW = 1, PICK_PACK = 2;
    private static final int[] RATES = {50, 75, 100, 125, 150, 175, 200, 250, 300, 400};

    /** Something a page shows that changes while a job runs: a progress bar, a Stop button's step. */
    private interface Live {
        void update();
    }

    private final List<List<String[]>> stacks = new ArrayList<>();   // per browsing tab: routes
    private final List<List<String>> cameFrom = new ArrayList<>();   // the control that opened each page
    private final String[] query = {"", ""};
    private int tab;
    private Button[] tabButtons;
    private Button back;
    private TextView where;
    private LinearLayout header, body, rootBody;
    private ScrollView scroll;
    private final List<Live> live = new ArrayList<>();
    private List<Library.Item> items = new ArrayList<>();
    private String focusedTag;  // the control TalkBack is on, to come back to after a page is rebuilt
    private int autoTag;
    private String jobState = "";
    private String sampleFor, sampleText;
    private int rate = 100;
    private TextToSpeech tts;
    private boolean ttsReady;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle(R.string.app_name);
        for (int i = 0; i < 2; i++) {
            stacks.add(new ArrayList<String[]>());
            cameFrom.add(new ArrayList<String>());
        }
        LinearLayout outer = new LinearLayout(this);
        outer.setOrientation(LinearLayout.VERTICAL);

        // the tabs, as on the iPhone (there at the bottom; here at the top, as Android's own apps do)
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        int pad = Ui.dp(this, 8);
        bar.setPadding(pad, pad, pad, 0);
        String[] names = {getString(R.string.tab_ios), getString(R.string.tab_mac), getString(R.string.tab_settings)};
        tabButtons = new Button[names.length];
        for (int i = 0; i < names.length; i++) {
            final int which = i;
            Button b = new Button(this);
            b.setText(names[i]);
            b.setAllCaps(false);
            b.setTextSize(16);
            b.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    selectTab(which);
                }
            });
            b.setAccessibilityDelegate(new Delegate(getString(R.string.role_tab), i, names.length));
            bar.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            tabButtons[i] = b;
        }
        outer.addView(bar);

        // Back and where you are
        header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setPadding(pad, 0, pad, 0);
        back = new Button(this);
        back.setText(R.string.back);
        back.setAllCaps(false);
        back.setTextSize(16);
        back.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                goBack();
            }
        });
        header.addView(back);
        where = new TextView(this);
        where.setTextSize(16);
        where.setPadding(pad, 0, 0, 0);
        where.setGravity(android.view.Gravity.CENTER_VERTICAL);
        where.setTag("where");
        if (Build.VERSION.SDK_INT >= 28) where.setAccessibilityHeading(true);
        header.addView(where, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
        outer.addView(header);

        scroll = new ScrollView(this);
        body = Ui.column(this);
        scroll.addView(body);
        outer.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(outer);

        tts = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int s) {
                ttsReady = s == TextToSpeech.SUCCESS;
            }
        }, getPackageName());

        // test hooks (adb shell am start ... --es import ios6,ios3 / --es selftest ios6/en-US), in
        // debuggable builds only: the activity is exported, and a pack can bring native code
        if ((getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0) testHooks();
        // fast code for firmware imported before it could be built here, or by an older version of the app
        if (!FastCode.autoOff(this)) FastCode.startIfNeeded(this);

        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 0);
        }
        tab = Data.prefs(this).getInt("tab", 0);
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

    @Override
    protected void onResume() {
        super.onResume();
        ImportService.addListener(this);
        reload();
        render(null, false);
    }

    @Override
    protected void onPause() {
        ImportService.removeListener(this);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        tts.shutdown();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (tab < 2 && !stacks.get(tab).isEmpty()) goBack();
        else super.onBackPressed();
    }

    private void reload() {
        Data.refresh(this);
        items = Library.items(this);
        jobState = jobState();
    }

    // ---------------------------------------------------------------- navigation

    private void selectTab(int which) {
        tab = which;
        Data.prefs(this).edit().putInt("tab", which).apply();
        render(null, false);
    }

    private void open(String... route) {
        stacks.get(tab).add(route);
        cameFrom.get(tab).add(focusedTag);
        render("where", true);
    }

    private void goBack() {
        List<String[]> s = stacks.get(tab);
        if (s.isEmpty()) return;
        s.remove(s.size() - 1);
        List<String> c = cameFrom.get(tab);
        String from = c.isEmpty() ? null : c.remove(c.size() - 1);
        render(from, true);
    }

    private String[] route() {
        if (tab == 2) return null;
        List<String[]> s = stacks.get(tab);
        return s.isEmpty() ? null : s.get(s.size() - 1);
    }

    private boolean mac() { return tab == 1; }

    private String versionId() { return Data.prefs(this).getString(mac() ? "version.mac" : "version.ios", "all"); }

    private String title(String[] r) {
        switch (r[0]) {
            case "language": return Library.languageName(r[1]);
            case "voice": {
                Library.Item it = item(r[1]);
                return it != null ? it.name : getString(R.string.voice);
            }
            case "firmware": return getString(R.string.page_firmware);
            case "fast": return getString(R.string.page_fast);
            default: return getString(R.string.page_diagnostics);
        }
    }

    // ---------------------------------------------------------------- pages

    /**
     * Rebuilds the page on screen. focusTag: put TalkBack on that control
     * (the page title when opening a page, the row that opened it when going
     * back); announce: say so even if it's the same control.
     */
    private void render(String focusTag, boolean announce) {
        for (int i = 0; i < tabButtons.length; i++) tabButtons[i].setSelected(i == tab);
        body.removeAllViews();
        live.clear();
        autoTag = 0;
        rootBody = null;
        String[] r = route();
        if (tab == 2) {
            header.setVisibility(View.GONE);
            settingsPage();
        } else {
            header.setVisibility(View.VISIBLE);
            back.setEnabled(r != null);
            List<String> crumbs = new ArrayList<>();
            crumbs.add(getString(mac() ? R.string.tab_mac : R.string.tab_ios));
            Library.Version v = Library.version(versionId());
            crumbs.add(v != null ? v.menuName() : getString(R.string.all_versions));
            for (String[] x : stacks.get(tab)) crumbs.add(title(x));
            where.setText(join(crumbs, " › "));
            where.setContentDescription(getString(R.string.current_location, join(crumbs, ", ")));
            if (r == null) rootPage();
            else if (r[0].equals("language")) languagePage(r[1]);
            else if (r[0].equals("voice")) voicePage(r[1]);
            else if (r[0].equals("firmware")) firmwarePage();
            else if (r[0].equals("fast")) fastPage();
            else diagnosticsPage();
        }
        if (focusTag == null && !announce) {
            scroll.scrollTo(0, 0);
            return;
        }
        final String target = focusTag;
        body.postDelayed(new Runnable() {
            @Override
            public void run() {
                View v = target == null ? null : findTagged(target);
                if (v == null && "where".equals(target)) v = where;
                if (v == null && announce()) v = where;
                if (v != null) {
                    v.requestRectangleOnScreen(new android.graphics.Rect(0, 0, v.getWidth(), v.getHeight()), true);
                    v.performAccessibilityAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS, null);
                }
            }

            private boolean announce() { return header.getVisibility() == View.VISIBLE; }
        }, 80);
    }

    private View findTagged(String tag) {
        if (tag.equals("where")) return where;
        return body.findViewWithTag(tag);
    }

    /** Rebuild because something changed under the page: stay on the same control. */
    private void refreshPage() {
        reload();
        render(focusedTag, false);
    }

    private void rootPage() {
        final EditText search = edit("search", getString(R.string.search_all), query[tab], false);
        search.setInputType(InputType.TYPE_CLASS_TEXT);
        rootBody = new LinearLayout(this);
        rootBody.setOrientation(LinearLayout.VERTICAL);
        body.addView(rootBody);
        search.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}

            @Override
            public void onTextChanged(CharSequence s, int a, int b, int c) {}

            @Override
            public void afterTextChanged(Editable s) {
                query[tab] = s.toString();
                LinearLayout keep = body;
                body = rootBody;
                rootBody.removeAllViews();
                live.clear();
                rootContent();
                body = keep;
            }
        });
        LinearLayout keep = body;
        body = rootBody;
        rootContent();
        body = keep;
    }

    private void rootContent() {
        String q = query[tab].trim();
        if (!q.isEmpty()) {
            // across everything, both tabs and all versions
            List<Library.Item> found = Library.search(items, q);
            if (found.isEmpty()) text(getString(R.string.no_match, q));
            else voiceButtons(found, true, getString(R.string.found_n, found.size()));
            return;
        }
        final List<Library.Version> versions = Library.versions(mac());
        List<String> options = new ArrayList<>();
        options.add(getString(R.string.all_versions));
        int selected = 0;
        for (int i = 0; i < versions.size(); i++) {
            options.add(versions.get(i).menuName());
            if (versions.get(i).id.equals(versionId())) selected = i + 1;
        }
        choice("version", getString(R.string.version), options, selected, new Choice() {
            @Override
            public void chose(int i) {
                String id = i == 0 ? "all" : versions.get(i - 1).id;
                if (id.equals(versionId())) return;
                Data.prefs(MainActivity.this).edit().putString(mac() ? "version.mac" : "version.ios", id).apply();
                render("version", false);
            }
        });
        List<Library.Version> missing = new ArrayList<>();
        for (Library.Version v : versions) {
            if ((versionId().equals("all") || v.id.equals(versionId())) && !Library.ready(this, v)) missing.add(v);
        }
        if (!missing.isEmpty()) {
            section(getString(R.string.get_the_voices));
            for (Library.Version v : missing) {
                List<String> need = new ArrayList<>();
                for (String f : v.firmware) if (!Library.firmwareImported(this, f)) need.add(Firmware.byKey(f).nice);
                text(need.isEmpty() ? getString(R.string.needs_reimport, v.name)
                        : getString(R.string.needs_firmware, v.name, join(need, getString(R.string.and))));
            }
            button("firmware", getString(R.string.get_firmware), new View.OnClickListener() {
                @Override
                public void onClick(View b) {
                    open("firmware");
                }
            });
        }
        List<String> errors = ImportService.errors();
        if (!errors.isEmpty()) {
            section(getString(R.string.problems));
            for (String e : errors) text(e);
            button("dismiss", getString(R.string.dismiss), new View.OnClickListener() {
                @Override
                public void onClick(View b) {
                    ImportService.dismissErrors();
                    render(null, false);
                }
            });
        }
        List<Library.Item> shown = Library.filter(items, mac(), versionId(), null);
        Set<String> langs = new LinkedHashSet<>();
        for (Library.Item i : shown) langs.add(Library.languageKey(i.language));
        List<String> sorted = new ArrayList<>(langs);
        Collections.sort(sorted, new Comparator<String>() {
            @Override
            public int compare(String a, String b) {
                return Library.languageName(a).compareToIgnoreCase(Library.languageName(b));
            }
        });
        if (sorted.size() == 1) {
            voiceButtons(shown, versionId().equals("all"), null);
        } else if (!sorted.isEmpty()) {
            section(getString(R.string.languages));
            for (final String lang : sorted) {
                int n = Library.filter(shown, mac(), null, lang).size();
                button("lang:" + lang, getString(n == 1 ? R.string.language_one : R.string.language_n,
                        Library.languageName(lang), n), new View.OnClickListener() {
                    @Override
                    public void onClick(View b) {
                        open("language", lang);
                    }
                });
            }
        }
        section(getString(R.string.more));
        if (missing.isEmpty()) {
            button("firmware", getString(R.string.get_firmware), new View.OnClickListener() {
                @Override
                public void onClick(View b) {
                    open("firmware");
                }
            });
        }
        button("fast", getString(R.string.page_fast), new View.OnClickListener() {
            @Override
            public void onClick(View b) {
                open("fast");
            }
        });
        button("diagnostics", getString(R.string.diagnostics_row), new View.OnClickListener() {
            @Override
            public void onClick(View b) {
                open("diagnostics");
            }
        });
    }

    private void languagePage(String lang) {
        voiceButtons(Library.filter(items, mac(), versionId(), lang), versionId().equals("all"), Library.languageName(lang));
    }

    /** where: the page's subject, leading each group's name. */
    private void voiceButtons(List<Library.Item> list, boolean byVersion, String where) {
        String lead = where != null ? where + ", " : "";
        if (byVersion) {
            for (Library.Version v : Library.VERSIONS) {
                List<Library.Item> vs = new ArrayList<>();
                for (Library.Item i : list) if (i.version == v) vs.add(i);
                if (vs.isEmpty()) continue;
                section(lead + (v.mac ? "Mac OS X " : "") + v.name);
                rows(vs);
            }
        } else {
            section(where != null ? getString(R.string.voices_of, where) : getString(R.string.voices));
            rows(list);
        }
    }

    private void rows(List<Library.Item> list) {
        List<Library.Item> sorted = new ArrayList<>(list);
        Collections.sort(sorted, new Comparator<Library.Item>() {
            @Override
            public int compare(Library.Item a, Library.Item b) {
                int c = a.name.compareToIgnoreCase(b.name);
                return c != 0 ? c : a.kind.compareTo(b.kind);
            }
        });
        Set<String> hidden = Data.hidden(this);
        for (final Library.Item it : sorted) {
            final String arg = it.jobArg();
            boolean pending = arg != null && ImportService.pending("download", arg);
            String label;
            if (pending) label = getString(R.string.stop_downloading, it.title(), Library.size(it.size));
            else if (!it.here()) label = getString(R.string.download_voice, it.title(), Library.size(it.size));
            else {
                label = it.title();
                if (it.installed && !it.builtIn) label += ", " + getString(R.string.using, Library.size(it.size));
                if (hidden.contains(it.id)) label += ", " + getString(R.string.not_system_voice);
            }
            button("voice:" + it.id, label, new View.OnClickListener() {
                @Override
                public void onClick(View b) {
                    if (it.here()) open("voice", it.id);
                    else if (ImportService.pending("download", arg)) ImportService.cancel("download", arg);
                    else ImportService.enqueue(MainActivity.this, new ImportService.Job("download", arg, it.title()));
                    if (!it.here()) refreshPage();
                }
            });
            if (pending) jobBar("download", arg, getString(R.string.downloading, it.title()));
        }
    }

    private Library.Item item(String id) {
        for (Library.Item i : items) if (i.id.equals(id)) return i;
        return null;
    }

    private void voicePage(String id) {
        final Library.Item it = item(id);
        if (it == null || !it.here()) {
            text(getString(R.string.voice_gone));
            return;
        }
        section(it.title() + ", " + (it.version.mac ? "Mac OS X " : "") + it.version.name);
        text(getString(R.string.voice_version, (it.version.mac ? "Mac OS X " : "") + it.version.menuName()));
        text(getString(R.string.voice_language, Library.languageName(it.language)));
        text(getString(R.string.voice_type, it.kind));
        if (!it.id.equals(sampleFor)) {
            sampleFor = it.id;
            sampleText = Samples.forLanguage(VoiceInfo.toLocale(it.language));
        }
        section(getString(R.string.try_it));
        EditText t = edit("sample", getString(R.string.text_to_speak), sampleText, true);
        t.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}

            @Override
            public void onTextChanged(CharSequence s, int a, int b, int c) {}

            @Override
            public void afterTextChanged(Editable s) {
                sampleText = s.toString();
            }
        });
        List<String> rates = new ArrayList<>();
        int sel = 2;
        for (int i = 0; i < RATES.length; i++) {
            rates.add(RATES[i] + "%");
            if (RATES[i] == rate) sel = i;
        }
        choice("rate", getString(R.string.speaking_rate), rates, sel, new Choice() {
            @Override
            public void chose(int i) {
                rate = RATES[i];
            }
        });
        button("speak", getString(R.string.speak), new View.OnClickListener() {
            @Override
            public void onClick(View b) {
                speak(it);
            }
        });
        button("stop", getString(R.string.stop), new View.OnClickListener() {
            @Override
            public void onClick(View b) {
                if (ttsReady) tts.stop();
            }
        });
        text(getString(R.string.try_it_explained));

        section(getString(R.string.system));
        check("system", getString(R.string.use_as_system_voice), !Data.hidden(this).contains(it.id),
                new CompoundButton.OnCheckedChangeListener() {
                    @Override
                    public void onCheckedChanged(CompoundButton b, boolean on) {
                        Data.setHidden(MainActivity.this, it.id, !on);
                        refreshPage();
                    }
                });
        final VoiceInfo def = Data.defaultFor(this, VoiceInfo.toLocale(it.language).getLanguage(),
                VoiceInfo.toLocale(it.language).getCountry());
        final String langPref = "default_" + it.language;
        check("default", getString(R.string.default_for, Library.languageName(it.language)),
                def != null && def.key.equals(it.id), new CompoundButton.OnCheckedChangeListener() {
                    @Override
                    public void onCheckedChanged(CompoundButton b, boolean on) {
                        if (on) Data.prefs(MainActivity.this).edit().putString(langPref, it.id).apply();
                        else Data.prefs(MainActivity.this).edit().remove(langPref).apply();
                        refreshPage();
                    }
                });
        text(getString(R.string.system_explained));

        if (it.installed && !it.builtIn) {
            section(getString(R.string.remove_heading));
            button("remove", getString(R.string.remove_voice, Library.size(it.size)), new View.OnClickListener() {
                @Override
                public void onClick(View b) {
                    new AlertDialog.Builder(MainActivity.this)
                            .setMessage(getString(R.string.remove_question, it.title()))
                            .setPositiveButton(R.string.remove, new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    if (ttsReady) tts.stop();
                                    Library.remove(MainActivity.this, it);
                                    reload();
                                    goBack();
                                }
                            })
                            .setNegativeButton(android.R.string.cancel, null)
                            .show();
                }
            });
        }
    }

    private void speak(Library.Item it) {
        if (!ttsReady) {
            Toast.makeText(this, R.string.not_ready, Toast.LENGTH_SHORT).show();
            return;
        }
        VoiceInfo v = Data.byAndroidName(this, it.id);
        if (v == null) return;
        Voice voice = null;
        Set<Voice> all = tts.getVoices();
        if (all != null) for (Voice x : all) if (x.getName().equals(v.androidName())) voice = x;
        // a voice taken out of the system's list still plays here, by name
        if (voice == null) voice = new Voice(v.androidName(), v.locale, Voice.QUALITY_NORMAL, Voice.LATENCY_LOW, false, null);
        tts.setVoice(voice);
        tts.setSpeechRate(rate / 100f);
        tts.speak(sampleText, TextToSpeech.QUEUE_FLUSH, null, "sample");
    }

    private void firmwarePage() {
        text(getString(R.string.firmware_explained));
        section(getString(R.string.from_apple));
        List<String> needed = new ArrayList<>();
        for (Library.Version v : Library.versions(mac())) {
            for (String f : v.firmware) if (!needed.contains(f)) needed.add(f);
        }
        for (String key : needed) {
            final Firmware.Build b = Firmware.byKey(key);
            final ImportService.Job job = currentOrQueued("firmware", key);
            String label;
            if (job != null) label = getString(R.string.stop_getting, b.nice, step(job));
            else if (Library.firmwareImported(this, key)) label = getString(R.string.firmware_imported, b.nice);
            else label = getString(R.string.firmware_get, b.nice, Library.size(b.size), b.purpose);
            final Button btn = button("fw:" + key, label, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    if (ImportService.pending("firmware", b.key)) {
                        ImportService.cancel("firmware", b.key);
                        refreshPage();
                    } else if (!Library.firmwareImported(MainActivity.this, b.key)) {
                        enqueueFirmware(b);
                    } else {
                        new AlertDialog.Builder(MainActivity.this)
                                .setMessage(getString(R.string.reimport_question, b.nice, Library.size(b.size)))
                                .setPositiveButton(R.string.reimport_yes, new DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(DialogInterface d, int w) {
                                        enqueueFirmware(b);
                                    }
                                })
                                .setNegativeButton(android.R.string.cancel, null)
                                .show();
                    }
                }
            });
            if (job != null) {
                stepLabel(btn, job, R.string.stop_getting, b.nice);
                jobBar("firmware", key, getString(R.string.getting, b.nice));
            }
        }
        section(getString(R.string.from_a_file));
        button("ipsw", getString(R.string.import_ipsw_file), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_IPSW, "*/*");
            }
        });
        button("pack", getString(R.string.import_pack), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_PACK, "application/zip");
            }
        });
        fileJobs();
    }

    /** The firmware files and data packs being imported, with their Stop buttons. */
    private void fileJobs() {
        ImportService.Job c = ImportService.current();
        if (c == null || !(c.kind.equals("ipsw") || c.kind.equals("pack"))) return;
        final ImportService.Job job = c;
        Button b = button("job:" + job.arg, getString(R.string.stop_importing, job.label, step(job)),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        ImportService.cancel(job.kind, job.arg);
                        refreshPage();
                    }
                });
        stepLabel(b, job, R.string.stop_importing, job.label);
        jobBar(job.kind, job.arg, getString(R.string.importing, job.label));
    }

    private void fastPage() {
        text(getString(R.string.fast_explained));
        section(getString(R.string.fast_heading, Data.abi()));
        boolean any = false;
        for (Firmware.Build b : Firmware.BUILDS) {
            if (!Library.firmwareImported(this, b.key)) continue;
            any = true;
            text(getString(Library.fastCode(this, b.key) ? R.string.fast_ready : R.string.fast_missing, b.nice));
        }
        if (!any) text(getString(R.string.fast_import_first));
        ImportService.Job j = currentOrQueued(FastCode.JOB, FastCode.JOB);
        final ImportService.Job job = j != null ? j : currentOrQueued(FastCode.JOB, FastCode.AGAIN);
        if (job != null) {
            Button stop = button("fast:stop", getString(R.string.fast_stop, "", step(job)), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    FastCode.autoOff(MainActivity.this, true);  // stopped on purpose: don't restart it at launch
                    ImportService.cancel(FastCode.JOB, job.arg);
                    refreshPage();
                }
            });
            stepLabel(stop, job, R.string.fast_stop, "");
            jobBar(FastCode.JOB, job.arg, getString(R.string.fast_bar));
        } else if (!FastCode.supported(this)) {
            text(getString(R.string.fast_not_here, Data.abi()));
        } else if (!FastCode.missing(this).isEmpty()) {
            button("fast:build", getString(R.string.fast_build_now), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    FastCode.autoOff(MainActivity.this, false);
                    FastCode.startIfNeeded(MainActivity.this);
                    refreshPage();
                }
            });
        } else if (any) {
            button("fast:again", getString(R.string.fast_again), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    FastCode.autoOff(MainActivity.this, false);
                    ImportService.enqueue(MainActivity.this,
                            new ImportService.Job(FastCode.JOB, FastCode.AGAIN, getString(R.string.fast_job)));
                    refreshPage();
                }
            });
        }
        button("pack", getString(R.string.import_pack), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_PACK, "application/zip");
            }
        });
        fileJobs();
    }

    private void diagnosticsPage() {
        final String report = diagnostics();
        TextView t = text(report);
        t.setTextIsSelectable(true);
        button("copy", getString(R.string.copy_report), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                cm.setPrimaryClip(ClipData.newPlainText(getString(R.string.page_diagnostics), report));
                Toast.makeText(MainActivity.this, R.string.copied, Toast.LENGTH_SHORT).show();
            }
        });
    }

    private String diagnostics() {
        StringBuilder s = new StringBuilder("Classic Voices diagnostics\n");
        s.append("Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append("), ")
                .append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(", ").append(Data.abi()).append('\n');
        for (Firmware.Build b : Firmware.BUILDS) {
            boolean have = Library.firmwareImported(this, b.key);
            s.append("  ").append(b.nice).append(" (").append(b.key).append("): ")
                    .append(have ? (Library.fastCode(this, b.key) ? "imported, fast code" : "imported, no fast code")
                            : "not imported").append('\n');
        }
        int here = 0;
        for (Library.Item i : items) if (i.here()) here++;
        s.append("Voices: ").append(here).append(" (").append(Data.hidden(this).size()).append(" hidden from the system)\n");
        String engine = Settings.Secure.getString(getContentResolver(), "tts_default_synth");
        s.append("Preferred engine: ").append(engine).append('\n');
        s.append("Settings: rules ").append(Data.rulesEnabled(this) ? "on" : "off").append(", pitch fallback ")
                .append(Data.fallbackEnabled(this) ? "on" : "off").append('\n');
        for (String e : ImportService.errors()) s.append("Error: ").append(e).append('\n');
        return s.toString().trim();
    }

    private void settingsPage() {
        section(getString(R.string.speech));
        check("rules", getString(R.string.setting_rules), Data.rulesEnabled(this), pref("rules"));
        text(getString(R.string.setting_rules_explained));
        check("pitch_fallback", getString(R.string.setting_fallback), Data.fallbackEnabled(this), pref("pitch_fallback"));
        text(getString(R.string.setting_fallback_explained));

        section(getString(R.string.tts_heading));
        String engine = Settings.Secure.getString(getContentResolver(), "tts_default_synth");
        int n = Data.voices(this).size();
        text(n == 0 ? getString(R.string.summary_none)
                : getString(getPackageName().equals(engine) ? R.string.summary_default : R.string.summary_not_default, n));
        button("tts", getString(R.string.open_tts_settings), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                try {
                    startActivity(new Intent("com.android.settings.TTS_SETTINGS"));
                } catch (ActivityNotFoundException e) {
                    startActivity(new Intent(Settings.ACTION_SETTINGS));
                }
            }
        });

        section(getString(R.string.heading_macintalk));
        text(getString(R.string.macintalk_explained));
    }

    private CompoundButton.OnCheckedChangeListener pref(final String key) {
        return new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                Data.prefs(MainActivity.this).edit().putBoolean(key, on).apply();
            }
        };
    }

    // ---------------------------------------------------------------- jobs

    private void enqueueFirmware(Firmware.Build b) {
        ImportService.enqueue(this, new ImportService.Job("firmware", b.key, b.nice));
        refreshPage();
    }

    private ImportService.Job currentOrQueued(String kind, String arg) {
        ImportService.Job c = ImportService.current();
        if (c != null && c.is(kind, arg)) return c;
        return ImportService.pending(kind, arg) ? new ImportService.Job(kind, arg, arg) : null;
    }

    /** What a job is doing: its step while it runs, "waiting" before. */
    private String step(ImportService.Job job) {
        ImportService.Job c = ImportService.current();
        if (c != null && c.is(job.kind, job.arg)) {
            String s = ImportService.status();
            return s != null && !s.isEmpty() ? s : getString(R.string.import_starting);
        }
        return getString(R.string.waiting);
    }

    /**
     * A job's progress bar, under its Stop button. The labels stay put while
     * it runs; the bar moves by 1% and TalkBack reports it its own way.
     */
    private void jobBar(final String kind, final String arg, String name) {
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        bar.setContentDescription(name);
        bar.setTag("bar:" + kind + ":" + arg);
        body.addView(bar);
        Live l = new Live() {
            @Override
            public void update() {
                ImportService.Job c = ImportService.current();
                boolean running = c != null && c.is(kind, arg);
                double f = running ? ImportService.fraction() : 0;
                bar.setIndeterminate(running && f < 0);
                int p = (int) (Math.max(0, f) * 100);
                if (bar.getProgress() != p) bar.setProgress(p);
            }
        };
        l.update();
        live.add(l);
    }

    /** A Stop button whose label follows the job's step (a few times per job, not every percent). */
    private void stepLabel(final Button b, final ImportService.Job job, final int format, final String name) {
        live.add(new Live() {
            @Override
            public void update() {
                String label = getString(format, name, step(job));
                if (!label.contentEquals(b.getText())) b.setText(label);
            }
        });
    }

    private String jobState() {
        ImportService.Job c = ImportService.current();
        return (c != null ? c.kind + ":" + c.arg : "") + "/" + ImportService.queued() + "/" + ImportService.errors().size();
    }

    @Override
    public void onImportStatus(boolean busy, String message, double fraction, String finished, String error) {
        if (finished != null || error != null || !jobState().equals(jobState)) {
            refreshPage();
            return;
        }
        for (Live l : live) l.update();
    }

    private void pick(int code, String type) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
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
    }

    // ---------------------------------------------------------------- controls

    private interface Choice {
        void chose(int index);
    }

    /** Remembers which control TalkBack is on; gives tabs their role and position. */
    private final class Delegate extends View.AccessibilityDelegate {
        private final String role;
        private final int index, count;

        Delegate() {
            this(null, 0, 0);
        }

        Delegate(String role, int index, int count) {
            this.role = role;
            this.index = index;
            this.count = count;
        }

        @Override
        public void sendAccessibilityEvent(View host, int type) {
            if (type == AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED && host.getTag() instanceof String) {
                focusedTag = (String) host.getTag();
            }
            super.sendAccessibilityEvent(host, type);
        }

        @Override
        public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {
            super.onInitializeAccessibilityNodeInfo(host, info);
            if (role == null) return;
            // the extra TalkBack reads as the role (what AndroidX's setRoleDescription sets)
            info.getExtras().putCharSequence("AccessibilityNodeInfo.roleDescription", role);
            info.setCollectionItemInfo(AccessibilityNodeInfo.CollectionItemInfo.obtain(0, 1, index, 1, false,
                    host.isSelected()));
            info.setSelected(host.isSelected());
        }
    }

    private <T extends View> T add(T v, String tag) {
        v.setTag(tag != null ? tag : "auto:" + autoTag++);
        v.setAccessibilityDelegate(new Delegate());
        body.addView(v);
        return v;
    }

    private void section(String title) {
        TextView t = Ui.heading(this, body, title);
        body.removeView(t);
        add(t, "h:" + title);
    }

    private TextView text(String s) {
        TextView t = Ui.text(this, body, s);
        body.removeView(t);
        return add(t, null);
    }

    private Button button(String tag, String label, View.OnClickListener l) {
        Button b = Ui.button(this, body, label, l);
        body.removeView(b);
        b.setTag(tag);
        b.setAccessibilityDelegate(new Delegate());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = Ui.dp(this, 4);
        b.setGravity(android.view.Gravity.START | android.view.Gravity.CENTER_VERTICAL);
        body.addView(b, lp);
        return b;
    }

    private void check(String tag, String label, boolean on, CompoundButton.OnCheckedChangeListener l) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setTextSize(16);
        cb.setChecked(on);
        cb.setOnCheckedChangeListener(l);
        add(cb, tag);
    }

    private EditText edit(String tag, String label, String value, boolean multiline) {
        TextView l = new TextView(this);
        l.setText(label);
        l.setTextSize(16);
        l.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        body.addView(l);
        EditText e = new EditText(this);
        e.setText(value);
        e.setTextSize(16);
        e.setHint(label);
        if (Build.VERSION.SDK_INT >= 26) e.setTooltipText(label);
        e.setContentDescription(null);
        if (multiline) e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        e.setId(View.generateViewId());
        l.setLabelFor(e.getId());
        return add(e, tag);
    }

    private void choice(String tag, String label, List<String> options, int selected, final Choice c) {
        TextView l = new TextView(this);
        l.setText(label);
        l.setTextSize(16);
        l.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        body.addView(l);
        Spinner s = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, options);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a);
        s.setSelection(selected, false);
        s.setId(View.generateViewId());
        l.setLabelFor(s.getId());
        s.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int i, long id) {
                c.chose(i);
            }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        add(s, tag);
    }

    private static String join(List<String> parts, String sep) {
        StringBuilder b = new StringBuilder();
        for (String p : parts) {
            if (b.length() > 0) b.append(sep);
            b.append(p);
        }
        return b.toString();
    }
}
