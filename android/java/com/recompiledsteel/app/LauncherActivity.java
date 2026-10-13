package com.recompiledsteel.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.Map;

/**
 * The first screen: the banner, what is installed, and the buttons. It
 * installs the game from the player's own Xbox 360 files (a folder they pick,
 * scanned like the PC setup programs do), checks that the game and its title
 * update are there before starting, and after a start that failed saves the
 * app's log so it can be sent without a PC.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;

    private static final int BACKGROUND = Color.rgb(12, 16, 20);
    private static final int CARD = Color.rgb(22, 28, 34);
    private static final int CARD_EDGE = Color.rgb(40, 50, 58);
    private static final int TEAL = Color.rgb(70, 215, 225);
    private static final int TEAL_DARK = Color.rgb(28, 127, 138);
    private static final int TEXT = Color.rgb(225, 230, 234);
    private static final int MUTED = Color.rgb(140, 152, 160);
    private static final int GOOD = Color.rgb(90, 210, 120);
    private static final int WARN = Color.rgb(240, 180, 70);
    private static final int BAD = Color.rgb(240, 95, 90);

    private TextView status;
    private TextView message;
    private TextView footer;
    private ProgressBar progress;
    private Button start;
    private Button install;
    private Button access;
    private String crashNote = "";
    private boolean installing;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setBackgroundColor(BACKGROUND);

        // The banner across the top, cropped to a strip that leaves room below
        // on a landscape phone as well as on a 4:3 handheld.
        ImageView banner = new ImageView(this);
        banner.setImageResource(R.drawable.launcher_banner);
        banner.setScaleType(ImageView.ScaleType.CENTER_CROP);
        int screenHeight = getResources().getDisplayMetrics().heightPixels;
        int screenWidth = getResources().getDisplayMetrics().widthPixels;
        int bannerHeight = Math.min(screenWidth / 4, (int) (Math.min(screenHeight, screenWidth) * 0.30f));
        column.addView(banner, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, bannerHeight));

        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(20), dp(12), dp(20), dp(16));
        column.addView(body);

        TextView version = new TextView(this);
        version.setText("Version " + BuildConfigVersion.NAME);
        version.setTextColor(MUTED);
        version.setTextSize(13);
        body.addView(version);

        // What is installed, in a card.
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(12), dp(16), dp(12));
        card.setBackground(rounded(CARD, CARD_EDGE, 12));
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cardParams.topMargin = dp(8);
        body.addView(card, cardParams);
        status = new TextView(this);
        status.setTextSize(16);
        status.setTextColor(TEXT);
        status.setLineSpacing(dp(4), 1f);
        card.addView(status);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setProgressTintList(ColorStateList.valueOf(TEAL));
        progress.setProgressBackgroundTintList(ColorStateList.valueOf(CARD_EDGE));
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        progressParams.topMargin = dp(10);
        body.addView(progress, progressParams);

        message = new TextView(this);
        message.setTextSize(14);
        message.setTextColor(TEXT);
        message.setPadding(0, dp(8), 0, dp(4));
        body.addView(message);

        // One row of buttons; it scrolls sideways on a narrow screen.
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.START);
        buttons.setPadding(0, dp(6), 0, dp(6));
        HorizontalScrollView buttonScroll = new HorizontalScrollView(this);
        buttonScroll.setHorizontalScrollBarEnabled(false);
        buttonScroll.addView(buttons);
        body.addView(buttonScroll);
        // "Allow file access" leads while it is needed (it hides once given).
        access = addButton(buttons, "Allow file access", true, v -> askFileAccess());
        start = addButton(buttons, "Start", true, v -> startGame());
        install = addButton(buttons, "Install game files", false, v -> pickFolder());
        addButton(buttons, "Save diagnostics", false, v -> {
            File saved = RunState.saveLogcat(this, "logcat");
            crashNote = saved != null ? "Diagnostics saved: " + saved.getAbsolutePath()
                                      : "Could not save diagnostics.";
            refresh();
        });
        addButton(buttons, "Exit", false, v -> finish());

        footer = new TextView(this);
        footer.setTextSize(12);
        footer.setTextColor(MUTED);
        footer.setPadding(0, dp(8), 0, 0);
        body.addView(footer);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACKGROUND);
        scroll.addView(column);
        setContentView(scroll);

        checkLastRun();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        (access.getVisibility() == View.VISIBLE ? access : start.isEnabled() ? start : install).requestFocus();
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                                               getResources().getDisplayMetrics());
    }

    private GradientDrawable rounded(int fill, int edge, int radiusDp) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(fill);
        shape.setCornerRadius(dp(radiusDp));
        if (edge != 0) shape.setStroke(dp(1), edge);
        return shape;
    }

    /** A flat rounded button; a bright outline shows focus for controllers. */
    private Button addButton(LinearLayout row, String label, boolean primary, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTypeface(Typeface.DEFAULT_BOLD);
        button.setTextColor(new ColorStateList(
            new int[][] {new int[] {-android.R.attr.state_enabled}, new int[] {}},
            new int[] {Color.rgb(88, 96, 102), primary ? Color.WHITE : TEXT}));
        StateListDrawable states = new StateListDrawable();
        int fill = primary ? TEAL_DARK : CARD;
        states.addState(new int[] {-android.R.attr.state_enabled}, rounded(Color.rgb(30, 34, 38), CARD_EDGE, 10));
        states.addState(new int[] {android.R.attr.state_pressed}, rounded(TEAL, TEAL, 10));
        states.addState(new int[] {android.R.attr.state_focused}, rounded(fill, TEAL, 10));
        states.addState(new int[] {}, rounded(fill, primary ? TEAL_DARK : CARD_EDGE, 10));
        button.setBackground(states);
        button.setPadding(dp(18), dp(10), dp(18), dp(10));
        button.setFocusable(true);
        button.setOnClickListener(listener);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.rightMargin = dp(10);
        row.addView(button, params);
        return button;
    }

    /** A start that crashed (state still "running") or ended within 20 s. */
    private void checkLastRun() {
        String state = RunState.read(this, "run_state");
        String step = RunState.read(this, "run_step");
        boolean failed = false;
        String why = "";
        if (state.startsWith("running ")) {
            // Installing an update ends a running game: not a crash.
            long started = 0;
            long updated = 0;
            try {
                started = Long.parseLong(state.substring(8).trim());
                updated = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            } catch (Exception ignored) {
            }
            if (updated > started && started != 0) {
                RunState.write(this, "run_state", "reported");
                return;
            }
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
        crashNote = note.toString();
        RunState.write(this, "run_state", "reported");
    }

    private void line(SpannableStringBuilder text, int color, String label, String value) {
        int from = text.length();
        text.append("●  ");
        text.setSpan(new ForegroundColorSpan(color), from, text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        text.append(label).append("   ");
        int v = text.length();
        text.append(value).append("\n");
        text.setSpan(new ForegroundColorSpan(MUTED), v, text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }

    private void refresh() {
        boolean canRead = RunState.hasFileAccess();
        File root = RunState.dataRoot();
        boolean game = new File(root, "game/default.xex").isFile();
        boolean update = new File(root, "game/default.xexp").isFile() && new File(root, "tu/pac").isDirectory();
        File addonDir = new File(root, "user/0000000000000000/584111E0/00000002");
        String[] addonList = addonDir.list();
        int addons = addonList != null ? addonList.length : 0;

        SpannableStringBuilder text = new SpannableStringBuilder();
        line(text, canRead ? GOOD : BAD, "File access",
             canRead ? "allowed" : "not allowed yet - press \"Allow file access\"");
        if (canRead) {
            line(text, game ? GOOD : BAD, "Game", game ? "installed" : "not installed - press \"Install game files\"");
            line(text, update ? GOOD : (game ? BAD : MUTED), "Title update",
                 update ? "installed" : "not installed (required)");
            line(text, addons > 0 ? GOOD : MUTED, "Add-ons", addons > 0 ? addons + " installed" : "none (optional)");
        }
        if (text.length() > 0) text.delete(text.length() - 1, text.length());
        status.setText(text);

        if (!installing) message.setText(crashNote);
        footer.setText("Game folder: " + root.getAbsolutePath() + "   ·   Logs: "
                       + RunState.logsDir(this).getAbsolutePath() + "\n" + Build.MANUFACTURER + " "
                       + Build.MODEL + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
        access.setVisibility(canRead ? View.GONE : View.VISIBLE);
        start.setEnabled(!installing && canRead && game && update);
        install.setEnabled(!installing && canRead);
        install.setText(game && update ? "Reinstall game files" : "Install game files");
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

    // ---------------------------------------------------------------- install

    private void pickFolder() {
        crashNote = "";
        message.setText("Pick the folder that holds your Xbox 360 files: the game, its title update "
                        + "and any add-ons (for example the Content folder copied from your console "
                        + "or USB stick). Sub-folders are searched too.");
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        try {
            startActivityForResult(intent, PICK_FOLDER);
        } catch (Exception e) {
            message.setText("This device has no folder picker: " + e.getMessage());
        }
    }

    /** The real path of a picked folder on internal storage or an SD card, or null. */
    private static String treeToPath(Uri tree) {
        try {
            String id = DocumentsContract.getTreeDocumentId(tree);
            if (id.startsWith("raw:")) return id.substring(4);
            int colon = id.indexOf(':');
            if (colon < 0) return null;
            String volume = id.substring(0, colon);
            String rel = id.substring(colon + 1);
            File base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory()
                                                           : new File("/storage/" + volume);
            File folder = rel.isEmpty() ? base : new File(base, rel);
            return folder.isDirectory() ? folder.getAbsolutePath() : null;
        } catch (Exception e) {
            return null;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FOLDER) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            message.setText("");
            return;
        }
        String folder = treeToPath(data.getData());
        if (folder == null) {
            message.setText("That folder cannot be read directly. Pick a folder on the device's own "
                            + "storage or its SD card (not a cloud or Downloads shortcut).");
            return;
        }
        message.setText("Looking through " + folder + " ...");
        new Thread(() -> {
            Map<String, String> found = Installer.find(folder);
            runOnUiThread(() -> confirmInstall(folder, found));
        }, "RS scan").start();
    }

    private void confirmInstall(String folder, Map<String, String> found) {
        String game = found.getOrDefault("game", "");
        String update = found.getOrDefault("update", "");
        String addons = found.getOrDefault("addons", "0");
        if (game.isEmpty()) {
            message.setText("No Real Steel game package was found in " + folder + " (looked at "
                            + found.getOrDefault("looked", "0") + " files). On the console it is under "
                            + "Content/0000000000000000/584111E0/000D0000.");
            return;
        }
        if (update.isEmpty()) {
            message.setText("The game was found, but not its title update, which ReCompiled Steel needs. "
                            + "Copy the title update package (on the console under "
                            + "Content/0000000000000000/584111E0/000B0000) into that folder and try again.");
            return;
        }
        message.setText("");
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Install Real Steel")
            .setMessage("From " + folder + ":\n\n• Game: " + game + "\n• Title update: " + update
                        + "\n• Add-ons: " + addons + "\n\nInto " + RunState.dataRoot().getAbsolutePath()
                        + " (about 210 MB plus the add-ons). Your saves are kept.")
            .setPositiveButton("Install", (d, w) -> runInstall(folder))
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void runInstall(String folder) {
        installing = true;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        progress.setProgress(0);
        progress.setVisibility(View.VISIBLE);
        message.setText("Installing...");
        refresh();
        File root = RunState.dataRoot();
        ArrayDeque<String> lines = new ArrayDeque<>();
        new Thread(() -> {
            String error = Installer.run(folder, root.getAbsolutePath(), new File(root, "user").getAbsolutePath(),
                new Installer.Listener() {
                    @Override
                    public void onProgress(double fraction, String what) {
                        runOnUiThread(() -> {
                            progress.setProgress((int) (fraction * 1000));
                            message.setText(what + " ... " + (int) (fraction * 100) + "%");
                        });
                    }

                    @Override
                    public void onLog(String line) {
                        synchronized (lines) {
                            lines.addLast(line);
                            while (lines.size() > 6) lines.removeFirst();
                        }
                    }
                });
            runOnUiThread(() -> {
                installing = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                progress.setVisibility(View.GONE);
                refresh();
                if (error.isEmpty()) {
                    message.setText("Installed. Press Start to play.");
                    start.requestFocus();
                } else {
                    StringBuilder text = new StringBuilder("The install stopped: ").append(error);
                    synchronized (lines) {
                        for (String l : lines) text.append("\n").append(l);
                    }
                    message.setText(text.toString());
                }
            });
        }, "RS install").start();
    }

    private void startGame() {
        crashNote = "";
        startActivity(new Intent(this, RealSteelActivity.class));
        finish();
    }
}
