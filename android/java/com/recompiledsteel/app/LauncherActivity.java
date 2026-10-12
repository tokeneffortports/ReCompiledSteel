package com.recompiledsteel.app;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The first screen: checks that the app may read /sdcard/RealSteel and that
 * the game files are there before starting the game, and after a start that
 * failed saves the app's log so it can be sent without a PC.
 */
public class LauncherActivity extends Activity {
    private TextView status;
    private Button start;
    private String crashNote = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(48, 32, 48, 32);
        column.setBackgroundColor(Color.rgb(18, 22, 28));

        TextView title = new TextView(this);
        title.setText("ReCompiled Steel " + BuildConfigVersion.NAME);
        title.setTextSize(24);
        title.setTextColor(Color.rgb(90, 220, 225));
        column.addView(title);

        status = new TextView(this);
        status.setTextSize(15);
        status.setTextColor(Color.rgb(225, 228, 232));
        status.setPadding(0, 16, 0, 16);
        column.addView(status);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.START);
        start = addButton(buttons, "Start", v -> startGame());
        addButton(buttons, "Allow file access", v -> askFileAccess());
        addButton(buttons, "Save diagnostics", v -> {
            File saved = RunState.saveLogcat(this, "logcat");
            crashNote = saved != null ? "Diagnostics saved: " + saved.getAbsolutePath() + "\n\n"
                                      : "Could not save diagnostics.\n\n";
            refresh();
        });
        addButton(buttons, "Exit", v -> finish());
        column.addView(buttons);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        setContentView(scroll);

        checkLastRun();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        start.requestFocus();
    }

    private Button addButton(LinearLayout row, String label, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(listener);
        row.addView(button);
        return button;
    }

    /** A start that crashed (state still "running") or ended within 20 s. */
    private void checkLastRun() {
        String state = RunState.read(this, "run_state");
        String step = RunState.read(this, "run_step");
        boolean failed = false;
        String why = "";
        if (state.startsWith("running ")) {
            failed = true;
            why = "The last start stopped unexpectedly (step: " + step + ").";
        } else if (state.startsWith("ended ")) {
            String[] parts = state.split(" ");
            try {
                long seconds = (Long.parseLong(parts[2]) - Long.parseLong(parts[1])) / 1000;
                if (seconds < 20) {
                    failed = true;
                    why = "The last start closed after " + seconds + " s (step: " + step + ").";
                }
            } catch (Exception ignored) {
            }
        }
        if (!failed) return;
        File saved = RunState.saveLogcat(this, "logcat_failed_start");
        StringBuilder note = new StringBuilder(why).append("\n");
        note.append(saved != null ? "Diagnostics saved: " + saved.getAbsolutePath()
                                  : "Diagnostics could not be saved.");
        File crash = new File(RunState.logsDir(this), "native_crash.txt");
        if (crash.isFile()) {
            note.append("\nCrash report: ").append(crash.getAbsolutePath());
            try {
                String text = new String(Files.readAllBytes(crash.toPath()), StandardCharsets.UTF_8);
                int from = Math.max(0, text.lastIndexOf("== native crash"));
                String last = text.substring(from);
                note.append("\n\n").append(last.length() > 1800 ? last.substring(0, 1800) : last);
            } catch (Exception ignored) {
            }
        }
        crashNote = note.append("\n\n").toString();
        RunState.write(this, "run_state", "reported");
    }

    private void refresh() {
        boolean access = RunState.hasFileAccess();
        File root = RunState.dataRoot();
        boolean game = new File(root, "game/default.xex").isFile();
        boolean tu = new File(root, "tu/pac").isDirectory();
        boolean addons = new File(root, "user/0000000000000000").isDirectory();
        StringBuilder text = new StringBuilder(crashNote);
        text.append("File access: ").append(access ? "allowed" : "NOT allowed - press \"Allow file access\"").append("\n");
        if (access) {
            text.append("Game files (").append(root.getAbsolutePath()).append("/game): ")
                .append(game ? "found" : "MISSING").append("\n");
            text.append("Title update (tu): ").append(tu ? "found" : "missing").append("\n");
            text.append("Add-ons (user/0000000000000000): ").append(addons ? "found" : "none").append("\n");
        }
        text.append("\nLogs go to ").append(RunState.logsDir(this).getAbsolutePath()).append("\n");
        text.append(Build.MANUFACTURER).append(" ").append(Build.MODEL).append(", Android ")
            .append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        status.setText(text.toString());
        start.setEnabled(access && game);
    }

    private void askFileAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void startGame() {
        crashNote = "";
        startActivity(new Intent(this, RealSteelActivity.class));
        finish();
    }
}
