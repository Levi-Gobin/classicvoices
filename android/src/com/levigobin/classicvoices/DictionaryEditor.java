package com.levigobin.classicvoices;

import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.LinearLayout;

import java.nio.charset.Charset;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The Dictionaries tab (Nico's design for the iPhone, 2026-11-14, in this app's form): a file choice, a searchable
 * list of entries (activating a row edits it; Preview, Preview with Earlier Entries, Preview with All Entries,
 * Remove and Restore the Community's are TalkBack actions, and a long press offers them too), Add Entry, and More:
 * Download the Latest Dictionaries, Source and Publishing. The entry page has the NVDA dialog's order: pattern,
 * replacement with Insert Phoneme and Convert to Phonemes, case, type, then a test text and Preview; Cancel and
 * Save. While the tab is on screen the voices use no dictionary (Dictionaries.editing), so plain reading is the
 * "without" preview; previews speak through the app's own engine with the entries they try (TtsService).
 *
 * Publishing (GitHub): signing in with GitHub's device flow on a page of its own, Publish My Changes Now, once a day
 * when turned on; Publish with no sign-in opens the sign-in, then publishes. Leaving the tab with unpublished changes
 * and no sign-in asks "Sign in to GitHub to publish your changes?" Yes / No / Don't Show Again.
 */
final class DictionaryEditor {
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final long EVERY = 24 * 3600 * 1000L;
    private static final String[] GENERIC = ("i y I Y e 2 E 9 { a A Q O o U u V @ 3 6 1 } M 7 8 & a~ E~ O~ 9~ o~ "
            + "p b t d k g q G\\ ? c J\\ m n N J F f v T D s z S Z x G h X R C j\\ s\\ z\\ tS dZ ts dz "
            + "l L 5 r r\\ 4 R j w H").split(" ");
    private static final int[] TYPES = {2, 0, 1};  // the Type choice's order: whole word, anywhere, regular expression

    private final MainActivity a;
    private String query = "";
    private LinearLayout list;

    // the entry being added or edited
    private String file, oldKey, pattern = "", replacement = "", test = "", voiceKey = "";
    private boolean caseSensitive, isNew;
    private int type = 2;
    private String problem;

    // the phoneme list of a voice, worked out once
    private String soundsFor;
    private List<String[]> sounds = new ArrayList<>();  // {X-SAMPA, example, what Preview says (X-SAMPA)}

    private boolean busy;  // publishing
    private String sourceRepo, sourceBranch, sourceProblem;

    DictionaryEditor(MainActivity a) {
        this.a = a;
    }

    // ---------------------------------------------------------------- files and voices

    private Context c() { return a; }

    /** The files a voice here could use (the fetched and the person's first), as on the iPhone. */
    List<String> files() {
        List<String> existing = DictionaryStore.fileNames(a);
        Set<String> langs = new java.util.TreeSet<>();
        boolean mt = false;
        for (VoiceInfo v : Data.allVoices(a)) {
            if (v.key.startsWith("mt")) mt = true;
            else langs.add(v.language.replace('_', '-').toLowerCase(Locale.ROOT));
        }
        List<String> out = new ArrayList<>(existing);
        for (String l : langs) {
            int dash = l.indexOf('-');
            for (String f : new String[] {(dash > 0 ? l.substring(0, dash) : l) + ".dic", l + ".dic"}) {
                if (!out.contains(f)) out.add(f);
            }
        }
        if (mt && !out.contains("macintalk.dic")) out.add("macintalk.dic");
        if (out.isEmpty()) out.add("en.dic");
        return out;
    }

    private String file() {
        List<String> files = files();
        String chosen = Data.prefs(a).getString("dictionaries.file", "");
        return files.contains(chosen) ? chosen : files.get(0);
    }

    static String fileLanguage(String file) {
        String base = file.endsWith(".dic") ? file.substring(0, file.length() - 4) : file;
        String[] p = base.split("-");
        return p.length == 2 ? p[0] + "-" + p[1].toUpperCase(Locale.ROOT) : base;
    }

    private String fileTitle(String file) {
        if (file.equals("macintalk.dic")) return a.getString(R.string.dict_file_macintalk);
        return a.getString(R.string.dict_file, Library.languageName(fileLanguage(file)), file);
    }

    /** The voices that use a file. */
    private List<VoiceInfo> voices(String file) {
        String lang = fileLanguage(file).toLowerCase(Locale.ROOT);
        List<VoiceInfo> out = new ArrayList<>();
        for (VoiceInfo v : Data.allVoices(a)) {
            if (file.equals("macintalk.dic")) {
                if (v.key.startsWith("mt")) out.add(v);
                continue;
            }
            String l = v.language.replace('_', '-').toLowerCase(Locale.ROOT);
            if (!v.key.startsWith("mt") && (l.equals(lang) || l.startsWith(lang + "-"))) out.add(v);
        }
        // the system's choice for the language first, then by name
        String[] lc = fileLanguage(file).split("-");
        final VoiceInfo first = out.isEmpty() || file.equals("macintalk.dic") ? null
                : Data.defaultFor(a, lc[0], lc.length > 1 ? lc[1] : "");
        Collections.sort(out, new java.util.Comparator<VoiceInfo>() {
            @Override
            public int compare(VoiceInfo x, VoiceInfo y) {
                if (first != null && x.key.equals(first.key) != y.key.equals(first.key)) return x.key.equals(first.key) ? -1 : 1;
                return x.name.compareToIgnoreCase(y.name);
            }
        });
        return out;
    }

    private VoiceInfo voice(List<VoiceInfo> vs) {
        for (VoiceInfo v : vs) if (v.key.equals(voiceKey)) return v;
        return vs.isEmpty() ? null : vs.get(0);
    }

    private String whose(DictionaryStore.Origin o) {
        switch (o) {
            case ADDED: return a.getString(R.string.dict_whose_added);
            case CHANGED: return a.getString(R.string.dict_whose_changed);
            case REMOVED: return a.getString(R.string.dict_whose_removed);
            default: return a.getString(R.string.dict_whose_community);
        }
    }

    private String spokenReplacement(String r) {
        if (r.isEmpty()) return a.getString(R.string.dict_nothing);
        return r.contains("~~") ? a.getString(R.string.dict_phonemes, r.replace("~~", "")) : r;
    }

    // ---------------------------------------------------------------- navigation

    String title(String[] r) {
        switch (r[0]) {
            case "entry": return a.getString(isNew ? R.string.dict_add_entry : R.string.dict_edit_entry);
            case "phonemes": return a.getString(R.string.dict_insert_phoneme_title);
            case "source": return a.getString(R.string.dict_source);
            case "publishing": return a.getString(R.string.dict_publishing);
            default: return a.getString(R.string.dict_sign_in_title);
        }
    }

    void page(String[] r) {
        if (r == null) rootPage();
        else if (r[0].equals("entry")) entryPage();
        else if (r[0].equals("phonemes")) phonemePage();
        else if (r[0].equals("source")) sourcePage();
        else if (r[0].equals("publishing")) publishingPage();
        else signInPage(r.length > 1 && r[1].equals("1"));
    }

    /** Back from a page: a sign-in under way stops. */
    void leaving(String[] r) {
        if (r != null && r[0].equals("signin")) SignIn.cancel();
    }

    private void say(String s) {
        View v = a.getWindow().getDecorView();
        v.announceForAccessibility(s);
    }

    // ---------------------------------------------------------------- the list

    private void rootPage() {
        final List<String> files = files();
        final String f = file();
        List<String> titles = new ArrayList<>();
        for (String x : files) titles.add(fileTitle(x));
        a.choice("dict:file", a.getString(R.string.dict_dictionary), titles, files.indexOf(f), new MainActivity.Choice() {
            @Override
            public void chose(int i) {
                if (files.get(i).equals(file())) return;
                Data.prefs(a).edit().putString("dictionaries.file", files.get(i)).apply();
                query = "";
                a.render("dict:file", false);
            }
        });
        a.text(a.getString(R.string.dict_explained));
        a.button("dict:add", a.getString(R.string.dict_add_entry), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startEntry(f, null);
            }
        });
        final EditText search = a.edit("dict:search", a.getString(R.string.dict_search), query, false);
        search.setInputType(InputType.TYPE_CLASS_TEXT);
        list = new LinearLayout(a);
        list.setOrientation(LinearLayout.VERTICAL);
        a.body().addView(list);
        search.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int x, int y, int z) {}

            @Override
            public void onTextChanged(CharSequence s, int x, int y, int z) {}

            @Override
            public void afterTextChanged(Editable s) {
                query = s.toString();
                list.removeAllViews();
                a.into(list, new Runnable() {
                    @Override
                    public void run() {
                        entries(f);
                    }
                });
            }
        });
        a.into(list, new Runnable() {
            @Override
            public void run() {
                entries(f);
            }
        });
        a.section(a.getString(R.string.more));
        a.button("dict:fetch", a.getString(R.string.dict_download), new View.OnClickListener() {
            @Override
            public void onClick(final View v) {
                fetch((Button) v);
            }
        });
        String[] src = Dictionaries.source(a);
        a.button("dict:source", a.getString(R.string.dict_source_row, src[0], src[1]), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String[] s = Dictionaries.source(a);
                sourceRepo = s[0];
                sourceBranch = s[1];
                sourceProblem = null;
                a.open("source");
            }
        });
        a.button("dict:publishing", a.getString(R.string.dict_publishing_row, publishingDetail()), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                a.open("publishing");
            }
        });
    }

    private void entries(final String f) {
        String q = query.trim().toLowerCase(Locale.getDefault());
        List<DictionaryStore.Row> shown = new ArrayList<>();
        for (DictionaryStore.Row r : DictionaryStore.rows(a, f)) {
            if (q.isEmpty() || r.entry.pattern.toLowerCase(Locale.getDefault()).contains(q)
                    || r.entry.replacement.toLowerCase(Locale.getDefault()).contains(q)) shown.add(r);
        }
        a.section(shown.isEmpty() ? a.getString(q.isEmpty() ? R.string.dict_no_entries : R.string.dict_no_matches)
                : a.getString(R.string.dict_entries_n, shown.size()));
        boolean canPreview = !voices(f).isEmpty();
        for (final DictionaryStore.Row r : shown) row(f, r, canPreview && r.entry.type != 1);
    }

    /** Custom TalkBack actions on a control, also offered by a long press. */
    private final class Actions extends View.AccessibilityDelegate {
        final int[] ids;
        final String[] labels;
        final Runnable[] runs;

        Actions(List<Integer> ids, List<String> labels, List<Runnable> runs) {
            this.ids = new int[ids.size()];
            for (int i = 0; i < ids.size(); i++) this.ids[i] = ids.get(i);
            this.labels = labels.toArray(new String[0]);
            this.runs = runs.toArray(new Runnable[0]);
        }

        @Override
        public void sendAccessibilityEvent(View host, int type) {
            if (type == AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED && host.getTag() instanceof String) {
                a.noteFocus((String) host.getTag());
            }
            super.sendAccessibilityEvent(host, type);
        }

        @Override
        public void onInitializeAccessibilityNodeInfo(View host, AccessibilityNodeInfo info) {
            super.onInitializeAccessibilityNodeInfo(host, info);
            for (int i = 0; i < ids.length; i++) info.addAction(new AccessibilityNodeInfo.AccessibilityAction(ids[i], labels[i]));
        }

        @Override
        public boolean performAccessibilityAction(View host, int action, Bundle args) {
            for (int i = 0; i < ids.length; i++) {
                if (ids[i] == action) {
                    runs[i].run();
                    return true;
                }
            }
            return super.performAccessibilityAction(host, action, args);
        }

        void menu(String title) {
            new AlertDialog.Builder(a).setTitle(title).setItems(labels, new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int which) {
                    runs[which].run();
                }
            }).setNegativeButton(android.R.string.cancel, null).show();
        }
    }

    private void attach(final View v, final String title, Actions act) {
        v.setAccessibilityDelegate(act);
        final Actions x = act;
        v.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View view) {
                x.menu(title);
                return true;
            }
        });
    }

    private void row(final String f, final DictionaryStore.Row r, boolean canPreview) {
        final DictionaryStore.Entry e = r.entry;
        Button b = a.button("dict:row:" + e.key(), e.pattern + "\n" + (e.replacement.isEmpty() ? "—" : e.replacement)
                + " · " + whose(r.origin), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startEntry(f, r);
            }
        });
        b.setContentDescription(a.getString(R.string.dict_row, e.pattern, spokenReplacement(e.replacement), whose(r.origin)));
        List<Integer> ids = new ArrayList<>();
        List<String> labels = new ArrayList<>();
        List<Runnable> runs = new ArrayList<>();
        if (canPreview) {
            int[] scopes = {DictionaryStore.ALONE, DictionaryStore.EARLIER, DictionaryStore.ALL};
            int[] actionIds = {R.id.dict_action_preview, R.id.dict_action_preview_earlier, R.id.dict_action_preview_all};
            int[] names = {R.string.dict_preview, R.string.dict_preview_earlier, R.string.dict_preview_all};
            for (int i = 0; i < 3; i++) {
                final int scope = scopes[i];
                ids.add(actionIds[i]);
                labels.add(a.getString(names[i]));
                runs.add(new Runnable() {
                    @Override
                    public void run() {
                        previewRow(f, e, scope);
                    }
                });
            }
        }
        if (r.origin != DictionaryStore.Origin.REMOVED) {
            ids.add(R.id.dict_action_remove);
            labels.add(a.getString(R.string.dict_remove));
            runs.add(new Runnable() {
                @Override
                public void run() {
                    DictionaryStore.remove(a, f, e.key());
                    changed(R.string.dict_removed);
                }
            });
        }
        if (r.origin == DictionaryStore.Origin.CHANGED || r.origin == DictionaryStore.Origin.REMOVED) {
            ids.add(R.id.dict_action_restore);
            labels.add(a.getString(R.string.dict_restore));
            runs.add(new Runnable() {
                @Override
                public void run() {
                    DictionaryStore.restore(a, f, e.key());
                    changed(R.string.dict_restored);
                }
            });
        }
        attach(b, e.pattern, new Actions(ids, labels, runs));
    }

    private void changed(int said) {
        a.render(a.focusedTag(), false);
        say(a.getString(said));
    }

    private void previewRow(String f, DictionaryStore.Entry e, int scope) {
        VoiceInfo v = voice(voices(f));
        if (v == null) return;
        a.speakPreview(v, e.pattern, DictionaryStore.previewEntries(a, f, e, null, Dictionaries.enabled(a), scope));
    }

    private void fetch(final Button b) {
        b.setEnabled(false);
        b.setText(R.string.dict_downloading);
        Dictionaries.fetchNow(a, new Dictionaries.Done() {
            @Override
            public void fetched(final Dictionaries.Changes ch) {
                a.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (a.isFinishing()) return;
                        String title, msg;
                        if (ch == null) {
                            title = a.getString(R.string.dict_fetch_failed);
                            msg = a.getString(R.string.dict_fetch_failed_text);
                        } else if (ch.text() != null) {
                            title = a.getString(R.string.dictionaries_updated);
                            msg = ch.text();
                        } else {
                            title = a.getString(R.string.dict_up_to_date);
                            msg = a.getString(R.string.dict_up_to_date_text);
                        }
                        a.render("dict:fetch", false);
                        alert(title, msg);
                    }
                });
            }
        });
    }

    private void alert(String title, String message) {
        new AlertDialog.Builder(a).setTitle(title).setMessage(message).setPositiveButton(android.R.string.ok, null).show();
    }

    // ---------------------------------------------------------------- one entry

    private void startEntry(String f, DictionaryStore.Row r) {
        file = f;
        isNew = r == null;
        problem = null;
        if (r != null) {
            DictionaryStore.Entry e = r.entry;
            oldKey = e.key();
            pattern = e.pattern;
            replacement = e.replacement;
            caseSensitive = e.caseSensitive;
            type = e.type;
            test = e.type == 1 ? "" : e.pattern;
        } else {
            oldKey = null;
            pattern = replacement = test = "";
            caseSensitive = false;
            type = 2;
        }
        List<VoiceInfo> vs = voices(f);
        voiceKey = vs.isEmpty() ? "" : vs.get(0).key;
        a.open("entry");
    }

    private DictionaryStore.Entry draft() {
        return new DictionaryStore.Entry(pattern, replacement.replace('\t', ' ').replace('\n', ' '), caseSensitive, type);
    }

    private interface Set1 {
        void set(String s);
    }

    private EditText field(String tag, String label, String value, final Set1 set) {
        EditText e = a.edit(tag, label, value, false);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int x, int y, int z) {}

            @Override
            public void onTextChanged(CharSequence s, int x, int y, int z) {}

            @Override
            public void afterTextChanged(Editable s) {
                set.set(s.toString());
            }
        });
        return e;
    }

    private void entryPage() {
        if (file == null) {
            a.text(a.getString(R.string.dict_entry_gone));
            return;
        }
        final List<VoiceInfo> vs = voices(file);
        a.text(fileTitle(file));
        field("dict:pattern", a.getString(R.string.dict_pattern), pattern, new Set1() {
            @Override
            public void set(String s) {
                pattern = s;
            }
        });
        field("dict:replacement", a.getString(R.string.dict_replacement), replacement, new Set1() {
            @Override
            public void set(String s) {
                replacement = s;
            }
        });
        Button ins = a.button("dict:insert", a.getString(R.string.dict_insert_phoneme), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                a.open("phonemes");
            }
        });
        ins.setEnabled(!vs.isEmpty());
        Button conv = a.button("dict:convert", a.getString(R.string.dict_convert), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                convert(voice(vs));
            }
        });
        conv.setEnabled(!vs.isEmpty());
        a.text(a.getString(R.string.dict_phonemes_explained));
        a.check("dict:case", a.getString(R.string.dict_case), caseSensitive, new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                caseSensitive = on;
            }
        });
        int ti = 0;
        for (int i = 0; i < TYPES.length; i++) if (TYPES[i] == type) ti = i;
        a.choice("dict:type", a.getString(R.string.dict_type), Arrays.asList(a.getString(R.string.dict_type_word),
                a.getString(R.string.dict_type_anywhere), a.getString(R.string.dict_type_regex)), ti, new MainActivity.Choice() {
            @Override
            public void chose(int i) {
                type = TYPES[i];
            }
        });
        field("dict:test", a.getString(R.string.dict_test), test, new Set1() {
            @Override
            public void set(String s) {
                test = s;
            }
        });
        if (vs.size() > 1) {
            List<String> names = new ArrayList<>();
            int sel = 0;
            for (int i = 0; i < vs.size(); i++) {
                names.add(vs.get(i).name);
                if (vs.get(i).key.equals(voiceKey)) sel = i;
            }
            a.choice("dict:voice", a.getString(R.string.voice), names, sel, new MainActivity.Choice() {
                @Override
                public void chose(int i) {
                    voiceKey = vs.get(i).key;
                }
            });
        }
        Button preview = a.button("dict:preview", a.getString(R.string.dict_preview), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                previewDraft(vs, DictionaryStore.ALONE);
            }
        });
        preview.setEnabled(!vs.isEmpty());
        if (!vs.isEmpty()) {
            List<Integer> ids = Arrays.asList(R.id.dict_action_preview_earlier, R.id.dict_action_preview_all);
            List<String> labels = Arrays.asList(a.getString(R.string.dict_preview_earlier), a.getString(R.string.dict_preview_all));
            List<Runnable> runs = Arrays.<Runnable>asList(new Runnable() {
                @Override
                public void run() {
                    previewDraft(vs, DictionaryStore.EARLIER);
                }
            }, new Runnable() {
                @Override
                public void run() {
                    previewDraft(vs, DictionaryStore.ALL);
                }
            });
            attach(preview, a.getString(R.string.dict_preview), new Actions(ids, labels, runs));
        }
        a.text(a.getString(vs.isEmpty() ? R.string.dict_no_voice : R.string.dict_preview_explained));
        if (problem != null) a.text(problem).setTag("dict:problem");
        a.button("dict:save", a.getString(R.string.dict_save), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                save(false);
            }
        });
        a.button("dict:cancel", a.getString(android.R.string.cancel), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                a.goBack();
            }
        });
    }

    private void previewDraft(List<VoiceInfo> vs, int scope) {
        VoiceInfo v = voice(vs);
        if (v == null) return;
        String text = test.isEmpty() ? pattern : test;
        if (text.isEmpty()) return;
        DictionaryStore.Entry e = draft();
        String entries = e.pattern.isEmpty() ? ""
                : DictionaryStore.previewEntries(a, file, e, oldKey, Dictionaries.enabled(a), scope);
        a.speakPreview(v, text, entries);
    }

    /** Why the entry can't be saved, null, or "empty" (an empty replacement: asked about first). */
    private String problem(DictionaryStore.Entry e, boolean emptyOK) {
        if (e.pattern.isEmpty()) return a.getString(R.string.dict_problem_pattern);
        if (e.replacement.isEmpty() && !emptyOK) return "empty";
        int spans = e.replacement.split("~~", -1).length - 1;
        if (spans % 2 == 1 && !e.replacement.endsWith("~~~")) return a.getString(R.string.dict_problem_span);
        if (e.type == 1 && !Native.regexCompiles(e.pattern.getBytes(UTF8))) return a.getString(R.string.dict_problem_regex);
        return null;
    }

    private void save(boolean emptyOK) {
        DictionaryStore.Entry e = draft();
        String p = problem(e, emptyOK);
        if ("empty".equals(p)) {
            new AlertDialog.Builder(a).setTitle(R.string.dict_empty_title)
                    .setMessage(a.getString(R.string.dict_empty_text, e.pattern))
                    .setPositiveButton(R.string.dict_save_anyway, new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface d, int w) {
                            save(true);
                        }
                    })
                    .setNegativeButton(android.R.string.cancel, null).show();
            return;
        }
        if (p != null) {
            problem = p;
            a.render("dict:problem", true);
            return;
        }
        DictionaryStore.put(a, file, e, oldKey);
        final boolean added = isNew;
        file = null;
        a.goBack();
        say(a.getString(added ? R.string.dict_added : R.string.dict_saved));
    }

    /** "~~X-SAMPA~~" inserted at the end of the replacement, inside a span that's open or ends it. */
    static String inserting(String x, String r) {
        int spans = r.split("~~", -1).length - 1;
        if (r.endsWith("~~") && spans % 2 == 0 && spans > 0) return r.substring(0, r.length() - 2) + x + "~~";
        if (spans % 2 == 1) return r + x;
        return r + "~~" + x + "~~";
    }

    /** The replacement's words as phonemes, as the voice says them (no stress marks: added by hand where needed). */
    private void convert(final VoiceInfo v) {
        if (v == null) return;
        final String from = replacement;
        final boolean regex = type == 1;
        new Thread(new Runnable() {
            @Override
            public void run() {
                final int[] counts = new int[2];
                final String out = toPhonemes(from, regex, v, counts);
                a.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (!from.equals(replacement)) return;  // typed meanwhile
                        replacement = out;
                        a.render("dict:convert", false);
                        say(counts[0] == 0 ? a.getString(R.string.dict_convert_none)
                                : counts[1] == 0 ? a.getString(R.string.dict_convert_done)
                                : a.getString(R.string.dict_convert_some, counts[0], counts[1]));
                    }
                });
            }
        }, "convert").start();
    }

    /** counts: {converted, left as they were}. Phonemes already there and (regular expressions) groups are kept. */
    private static String toPhonemes(String s, boolean regex, VoiceInfo v, int[] counts) {
        StringBuilder out = new StringBuilder();
        int n = s.length(), i = 0;
        while (i < n) {
            char ch = s.charAt(i);
            if (ch == '~' && i + 1 < n && s.charAt(i + 1) == '~' && !(i + 2 < n && s.charAt(i + 2) == '~')) {
                int j = i + 2;  // the span's close: the first run of two or more tildes
                while (j + 1 < n && !(s.charAt(j) == '~' && s.charAt(j + 1) == '~')) j++;
                int end = j + 1 < n ? j + 2 : n;
                while (end < n && s.charAt(end) == '~') end++;
                out.append(s, i, end);
                i = end;
                continue;
            }
            if (regex && ch == '\\') {
                int j = i + 1;
                if (j < n && Character.isDigit(s.charAt(j))) {
                    while (j < n && Character.isDigit(s.charAt(j))) j++;
                } else if (j + 1 < n && s.charAt(j) == 'g' && s.charAt(j + 1) == '<' && s.indexOf('>', j) > 0) {
                    j = s.indexOf('>', j) + 1;
                } else {
                    j = Math.min(i + 2, n);
                }
                out.append(s, i, j);
                i = j;
                continue;
            }
            if (Character.isLetter(ch)) {
                int j = i;
                while (j < n && (Character.isLetter(s.charAt(j))
                        || (s.charAt(j) == '\'' && j + 1 < n && Character.isLetter(s.charAt(j + 1))))) j++;
                String word = s.substring(i, j);
                String x = Native.textXsampa(v.key, v.language, word.getBytes(UTF8));
                if (x != null && !x.isEmpty()) {
                    out.append("~~").append(x).append("~~");
                    counts[0]++;
                } else {
                    out.append(word);
                    counts[1]++;
                }
                i = j;
                continue;
            }
            out.append(ch);
            i++;
        }
        return out.toString();
    }

    // ---------------------------------------------------------------- Insert Phoneme

    /** The voice's sounds: the language's phonemes/<lang>.txt in order (or a general list), each the voice has once. */
    private List<String[]> sounds(VoiceInfo v) {
        if (v.key.equals(soundsFor)) return sounds;
        List<String[]> listed = DictionaryStore.phonemeExamples(a, v.language);
        if (listed.isEmpty()) for (String g : GENERIC) listed.add(new String[] {g, ""});
        List<String[]> out = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (String[] l : listed) {
            String ipa = Native.xsampaToIpa(l[0].getBytes(UTF8));
            if (ipa == null) continue;
            String own = Native.phonemeText(v.key, v.language, ipa.getBytes(UTF8));
            if (own == null || !seen.add(own)) continue;  // the voice says it as one already in the list
            // a consonant is heard between two a's: alone, some voices say nothing
            String heard = Native.ipaHasVowel(ipa.getBytes(UTF8)) ? l[0] : "a" + l[0] + "a";
            out.add(new String[] {l[0], l[1], heard});
        }
        soundsFor = v.key;
        sounds = out;
        return out;
    }

    private void phonemePage() {
        final VoiceInfo v = file == null ? null : voice(voices(file));
        if (v == null) {
            a.text(a.getString(R.string.dict_no_phonemes));
            return;
        }
        List<String[]> items = sounds(v);
        a.section(a.getString(items.isEmpty() ? R.string.dict_no_phonemes : R.string.dict_sounds_of, v.name));
        for (final String[] it : items) {
            String label = it[1].isEmpty() ? it[0] : a.getString(R.string.dict_sound, it[0], it[1]);
            Button b = a.button("dict:sound:" + it[0], label, new View.OnClickListener() {
                @Override
                public void onClick(View view) {
                    replacement = inserting(it[0], replacement);
                    a.goBack();
                    say(a.getString(R.string.dict_inserted, it[0]));
                }
            });
            Runnable hear = new Runnable() {
                @Override
                public void run() {
                    // the sound, then its example word: a made-up word the entry turns into the sound
                    String word = it[1].replace('\u001b', ' ');
                    a.speakPreview(v, "cvsoundcv" + (word.isEmpty() ? "" : ", " + word), "cvsoundcv\t~~" + it[2] + "~~\t1\t2");
                }
            };
            attach(b, label, new Actions(Collections.singletonList(R.id.dict_action_preview),
                    Collections.singletonList(a.getString(R.string.dict_preview)), Collections.singletonList(hear)));
        }
    }

    // ---------------------------------------------------------------- Source

    private void sourcePage() {
        if (sourceRepo == null) {
            String[] s = Dictionaries.source(a);
            sourceRepo = s[0];
            sourceBranch = s[1];
        }
        a.text(a.getString(R.string.dict_source_explained));
        field("dict:repo", a.getString(R.string.dict_repo), sourceRepo, new Set1() {
            @Override
            public void set(String s) {
                sourceRepo = s;
            }
        }).setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        field("dict:branch", a.getString(R.string.dict_branch), sourceBranch, new Set1() {
            @Override
            public void set(String s) {
                sourceBranch = s;
            }
        });
        if (sourceProblem != null) a.text(sourceProblem).setTag("dict:sourceproblem");
        a.button("dict:sourcesave", a.getString(R.string.dict_save), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                String r = Dictionaries.parseRepo(sourceRepo);
                if (r == null) {
                    sourceProblem = a.getString(R.string.dict_repo_problem);
                    a.render("dict:sourceproblem", true);
                    return;
                }
                String b = sourceBranch.trim().isEmpty() ? Dictionaries.DEFAULT_BRANCH : sourceBranch.trim();
                String[] now = Dictionaries.source(a);
                sourceRepo = null;
                if (!r.equals(now[0]) || !b.equals(now[1])) {
                    Dictionaries.setSource(a, r, b);
                    Dictionaries.fetchNow(a, new Dictionaries.Done() {
                        @Override
                        public void fetched(final Dictionaries.Changes ch) {
                            a.runOnUiThread(new Runnable() {
                                @Override
                                public void run() {
                                    say(a.getString(ch != null ? R.string.dict_source_fetched : R.string.dict_source_failed));
                                    if (a.onDictionaryRoot()) a.render(a.focusedTag(), false);
                                }
                            });
                        }
                    });
                }
                a.goBack();
            }
        });
        a.button("dict:sourcedefault", a.getString(R.string.dict_use_default), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sourceRepo = Dictionaries.DEFAULT_REPO;
                sourceBranch = Dictionaries.DEFAULT_BRANCH;
                sourceProblem = null;
                a.render("dict:repo", false);
                say(a.getString(R.string.dict_default_set));
            }
        });
    }

    // ---------------------------------------------------------------- publishing

    private boolean canPublish() { return GitHub.login(a) != null; }

    private static boolean dontAsk(Context c) { return GitHub.state(c).getBoolean("dontAskSignIn", false); }

    static boolean automatic(Context c) { return GitHub.state(c).getBoolean("publishAutomatically", false); }

    private String publishingDetail() {
        String login = GitHub.login(a);
        int n = DictionaryStore.pending(a).size();
        String what = n == 0 ? a.getString(R.string.dict_nothing_pending)
                : a.getString(n == 1 ? R.string.dict_pending_one : R.string.dict_pending_n, n);
        return what + ", " + (login != null ? a.getString(R.string.dict_as_login, login) : a.getString(R.string.dict_not_signed_in));
    }

    private void publishingPage() {
        a.section(a.getString(R.string.dict_how_it_publishes));
        final String login = GitHub.login(a);
        if (!GitHub.supported()) {
            a.text(a.getString(R.string.dict_needs_android6));
        } else if (login != null) {
            a.text(a.getString(R.string.dict_signed_in_as, login));
            a.button("dict:signout", a.getString(R.string.dict_sign_out), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    GitHub.signOut(a);
                    a.render("dict:signin", false);
                    say(a.getString(R.string.dict_signed_out));
                }
            });
        } else {
            a.button("dict:signin", a.getString(R.string.dict_sign_in), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    a.open("signin", "0");
                }
            });
        }
        String fork = GitHub.fork(a);
        if (fork != null) a.text(a.getString(R.string.dict_fork_note, fork));
        a.section(a.getString(R.string.dict_your_changes));
        String pending = DictionaryStore.pendingText(a);
        a.text(pending != null ? a.getString(R.string.dict_not_published, pending) : a.getString(R.string.dict_nothing_to_publish_short));
        Button p = a.button("dict:publish", a.getString(busy ? R.string.dict_publishing_now : R.string.dict_publish_now),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        publishPressed();
                    }
                });
        p.setEnabled(!busy && GitHub.supported());
        a.section(a.getString(R.string.dict_automatic_heading));
        a.check("dict:auto", a.getString(R.string.dict_automatic), automatic(a), new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton b, boolean on) {
                GitHub.state(a).edit().putBoolean("publishAutomatically", on).apply();
            }
        });
        long last = GitHub.state(a).getLong("lastPublished", 0);
        if (last > 0) {
            a.text(a.getString(R.string.dict_last_published,
                    DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(new Date(last))));
        }
        String err = GitHub.state(a).getString("lastError", "");
        if (!err.isEmpty()) a.text(a.getString(R.string.dict_last_try, err));
    }

    /** Publish My Changes Now: with no sign-in, the sign-in page first, then publishing. */
    private void publishPressed() {
        if (!canPublish()) {
            a.open("signin", "1");
            return;
        }
        publish(true);
    }

    private void publish(final boolean manual) {
        if (busy) return;
        busy = true;
        if (a.onPublishingPage()) a.render("dict:publish", false);
        final Context app = a.getApplicationContext();
        new Thread(new Runnable() {
            @Override
            public void run() {
                final GitHub.Result r = GitHub.publish(app);
                a.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        handle(r, manual);
                    }
                });
            }
        }, "publish").start();
    }

    private void makeFork() {
        if (busy) return;
        busy = true;
        if (a.onPublishingPage()) a.render("dict:publish", false);
        say(a.getString(R.string.dict_making_copy));
        final Context app = a.getApplicationContext();
        new Thread(new Runnable() {
            @Override
            public void run() {
                GitHub.Result r = GitHub.makeFork(app);
                final GitHub.Result out = r.kind == GitHub.NOTHING ? GitHub.publish(app) : r;
                a.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        busy = false;
                        handle(out, true);
                    }
                });
            }
        }, "publish").start();
    }

    private void handle(GitHub.Result r, boolean manual) {
        android.content.SharedPreferences st = GitHub.state(a);
        switch (r.kind) {
            case GitHub.PUBLISHED:
                st.edit().remove("lastError").putLong("lastPublished", System.currentTimeMillis()).apply();
                if (manual) {
                    alert(a.getString(R.string.dict_published_title), r.summary.text() + "."
                            + (r.fork != null ? " " + a.getString(R.string.dict_published_fork, r.fork) : ""));
                }
                break;
            case GitHub.NOTHING:
                if (manual) {
                    String fork = GitHub.fork(a);
                    alert(a.getString(R.string.dict_nothing_title), fork != null && !DictionaryStore.pending(a).isEmpty()
                            ? a.getString(R.string.dict_nothing_fork, fork) : a.getString(R.string.dict_nothing_text));
                }
                break;
            case GitHub.NEEDS_FORK: {
                String repo = Dictionaries.source(a)[0];
                if (manual) {
                    new AlertDialog.Builder(a).setTitle(R.string.dict_fork_title)
                            .setMessage(a.getString(R.string.dict_fork_text, repo))
                            .setPositiveButton(R.string.dict_fork_yes, new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    makeFork();
                                }
                            })
                            .setNegativeButton(android.R.string.cancel, null).show();
                } else {
                    st.edit().putString("lastError", a.getString(R.string.dict_cant_publish_to, repo)).apply();
                }
                break;
            }
            case GitHub.NEEDS_INSTALL:
                if (manual) {
                    new AlertDialog.Builder(a).setTitle(R.string.dict_install_title).setMessage(R.string.dict_install_text)
                            .setPositiveButton(R.string.dict_open_github, new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    openUrl(GitHub.installUrl(a));
                                }
                            })
                            .setNegativeButton(android.R.string.cancel, null).show();
                }
                break;
            default:
                st.edit().putString("lastError", r.message).apply();
                if (manual) alert(a.getString(R.string.dict_failed_title), a.getString(R.string.dict_failed_text, r.message));
        }
        if (a.onPublishingPage() || a.onDictionaryRoot()) a.render(a.focusedTag(), false);
    }

    /** Once a day when turned on, when the app comes to the front. */
    void publishAutomatically() {
        android.content.SharedPreferences st = GitHub.state(a);
        if (!automatic(a) || busy || !canPublish()) return;
        if (System.currentTimeMillis() - st.getLong("publishTried", 0) < EVERY) return;
        if (DictionaryStore.pending(a).isEmpty()) return;
        st.edit().putLong("publishTried", System.currentTimeMillis()).apply();
        publish(false);
    }

    /**
     * Leaving the tab: asks to sign in when there's something to publish and no way to. then: run after the answer
     * (null: nothing). True when it asked.
     */
    boolean left(final Runnable then) {
        if (canPublish() || !GitHub.supported() || dontAsk(a) || DictionaryStore.pending(a).isEmpty()) return false;
        new AlertDialog.Builder(a).setTitle(R.string.dict_leave_title).setMessage(R.string.dict_leave_text)
                .setPositiveButton(R.string.dict_yes, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        a.showDictionaries();
                        a.open("signin", "1");
                    }
                })
                .setNegativeButton(R.string.dict_no, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        if (then != null) then.run();
                    }
                })
                .setNeutralButton(R.string.dict_dont_show, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        GitHub.state(a).edit().putBoolean("dontAskSignIn", true).apply();
                        if (then != null) then.run();
                    }
                })
                .setCancelable(false)
                .show();
        return true;
    }

    private void openUrl(String url) {
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            alert(a.getString(R.string.dict_no_browser), url);
        }
    }

    // ---------------------------------------------------------------- signing in

    /** A sign-in under way: kept here, so it goes on while the person is in the browser and the page is rebuilt. */
    static final class SignIn {
        static GitHub.DeviceCode code;
        static String problem;
        static boolean running, cancelled, thenPublish;
        static DictionaryEditor listener;
        private static int generation;

        static void start(final DictionaryEditor ed, boolean publish) {
            listener = ed;
            thenPublish = publish;
            if (running) return;
            running = true;
            cancelled = false;
            code = null;
            problem = null;
            final int gen = ++generation;
            final Context app = ed.a.getApplicationContext();
            new Thread(new Runnable() {
                @Override
                public void run() {
                    String login = null, err = null;
                    try {
                        final GitHub.DeviceCode c = GitHub.startSignIn(app);
                        post(new Runnable() {
                            @Override
                            public void run() {
                                if (gen != generation) return;
                                code = c;
                                if (listener != null) listener.codeArrived();
                            }
                        });
                        login = GitHub.finishSignIn(app, c, new GitHub.Cancelled() {
                            @Override
                            public boolean cancelled() {
                                return cancelled || gen != generation;
                            }
                        });
                    } catch (GitHub.Failure f) {
                        err = f.getMessage();
                    } catch (InterruptedException e) {
                        err = null;
                    }
                    final String l = login, e = err;
                    post(new Runnable() {
                        @Override
                        public void run() {
                            if (gen != generation) return;
                            running = false;
                            if (l != null) {
                                code = null;
                                if (listener != null) listener.signedIn(l);
                            } else if (e != null) {
                                code = null;
                                problem = e;
                                if (listener != null) listener.signInProblem();
                            }
                        }
                    });
                }
            }, "sign-in").start();
        }

        static void cancel() {
            cancelled = true;
            running = false;
            code = null;
            problem = null;
            generation++;
        }

        private static void post(Runnable r) {
            new android.os.Handler(android.os.Looper.getMainLooper()).post(r);
        }
    }

    /** The code as separate characters, so TalkBack reads it out letter by letter. */
    private String spelled(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            if (b.length() > 0) b.append(' ');
            char ch = s.charAt(i);
            b.append(ch == '-' ? a.getString(R.string.dict_dash) : String.valueOf(ch));
        }
        return b.toString();
    }

    private void signInPage(boolean thenPublish) {
        SignIn.listener = this;
        if (!SignIn.running && SignIn.problem == null && SignIn.code == null) SignIn.start(this, thenPublish);
        SignIn.thenPublish = thenPublish;
        if (SignIn.problem != null) {
            a.text(SignIn.problem).setTag("dict:signinproblem");
            a.button("dict:signinagain", a.getString(R.string.dict_try_again), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    SignIn.problem = null;
                    SignIn.start(DictionaryEditor.this, SignIn.thenPublish);
                    a.render("where", false);
                }
            });
        } else if (SignIn.code != null) {
            final GitHub.DeviceCode c = SignIn.code;
            String where = c.verification.replaceFirst("^https?://", "");
            android.widget.TextView t = a.text(a.getString(R.string.dict_enter_code, where, c.userCode));
            t.setContentDescription(a.getString(R.string.dict_enter_code, c.verification, spelled(c.userCode)));
            t.setTag("dict:code");
            a.button("dict:opengithub", a.getString(R.string.dict_copy_open), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    ClipboardManager cm = (ClipboardManager) a.getSystemService(Context.CLIPBOARD_SERVICE);
                    if (cm != null) cm.setPrimaryClip(ClipData.newPlainText(a.getString(R.string.dict_code_clip), c.userCode));
                    openUrl(c.verification);
                }
            });
            a.text(a.getString(R.string.dict_waiting));
        } else {
            a.text(a.getString(R.string.dict_asking_code));
        }
    }

    void codeArrived() {
        if (!a.onSignInPage()) return;
        a.render("dict:code", false);
        say(a.getString(R.string.dict_your_code, spelled(SignIn.code.userCode)));
    }

    void signInProblem() {
        if (a.onSignInPage()) a.render("dict:signinproblem", true);
    }

    void signedIn(String login) {
        boolean publish = SignIn.thenPublish;
        if (a.onSignInPage()) a.goBack();
        say(a.getString(R.string.dict_signed_in_said, login));
        if (publish) {
            if (!a.onPublishingPage()) a.open("publishing");
            publish(true);
        } else if (a.onPublishingPage()) {
            a.render(a.focusedTag(), false);
        }
    }
}
