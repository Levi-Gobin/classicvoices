package com.levigobin.classicvoices;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.IOException;
import java.util.List;

/** The voices Apple still serves, for the engines already imported. */
public class DownloadsActivity extends Activity implements ImportService.Listener {
    private LinearLayout list;
    private TextView status;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle(R.string.download_voices);
        ScrollView scroll = new ScrollView(this);
        LinearLayout col = Ui.column(this);
        scroll.addView(col);
        setContentView(scroll);
        status = Ui.text(this, col, "");
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        col.addView(list);
    }

    @Override
    protected void onResume() {
        super.onResume();
        ImportService.addListener(this);
        fill();
    }

    @Override
    protected void onPause() {
        ImportService.removeListener(this);
        super.onPause();
    }

    private void fill() {
        list.removeAllViews();
        List<Downloads.Item> items;
        try {
            items = Downloads.available(this);
        } catch (IOException e) {
            status.setText(e.getMessage());
            return;
        }
        if (items.isEmpty()) {
            status.setText(R.string.downloads_none);
            return;
        }
        status.setText(getString(R.string.downloads_count, items.size()));
        String lastGroup = null;
        for (final Downloads.Item it : items) {
            String group = it.kind.equals("enhanced") ? getString(R.string.group_enhanced) : groupFor(it.build);
            if (!group.equals(lastGroup)) {
                Ui.heading(this, list, group);
                lastGroup = group;
            }
            Ui.button(this, list, it.label + ", " + Ui.mb(it.size), new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    ImportService.enqueue(DownloadsActivity.this, new ImportService.Job("download", it.id(), it.label));
                    Toast.makeText(DownloadsActivity.this, getString(R.string.import_queued, it.label), Toast.LENGTH_SHORT).show();
                    v.setEnabled(false);
                }
            });
        }
    }

    private String groupFor(String era) {
        if (era.equals("mac107")) return getString(R.string.group_lion);
        if (era.equals("mac108")) return getString(R.string.group_mountain_lion);
        return getString(R.string.group_mavericks);
    }

    @Override
    public void onImportStatus(boolean busy, String message, double fraction, String finished, String error) {
        if (message != null) {
            status.setText(fraction >= 0 ? message + ", " + getString(R.string.percent, (int) (fraction * 100)) : message);
        }
        if (finished != null) fill();
    }
}
