package com.levigobin.classicvoices;

import android.content.Context;
import android.os.Build;
import android.util.TypedValue;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Small helpers for the app's plain, screen-reader-friendly layouts. */
final class Ui {
    private Ui() {}

    static int dp(Context c, int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static LinearLayout column(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(c, 16);
        l.setPadding(pad, pad, pad, pad);
        return l;
    }

    static TextView heading(Context c, LinearLayout parent, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        t.setPadding(0, dp(c, 20), 0, dp(c, 6));
        if (Build.VERSION.SDK_INT >= 28) t.setAccessibilityHeading(true);
        parent.addView(t);
        return t;
    }

    static TextView text(Context c, LinearLayout parent, String text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        t.setPadding(0, dp(c, 4), 0, dp(c, 4));
        parent.addView(t);
        return t;
    }

    static Button button(Context c, LinearLayout parent, String text, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(c, 4);
        parent.addView(b, lp);
        return b;
    }

    static String mb(long bytes) {
        return bytes >= 1L << 30 ? String.format(java.util.Locale.getDefault(), "%.1f GB", bytes / (double) (1L << 30))
                : (bytes >> 20) + " MB";
    }
}
