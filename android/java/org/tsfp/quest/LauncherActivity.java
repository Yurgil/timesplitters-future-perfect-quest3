package org.tsfp.quest;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.StatFs;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/** First-run setup stays in a system panel until the user imports their ISO. */
public final class LauncherActivity extends Activity {
    private LinearLayout content;
    private Thread worker;
    private TextView status;
    private ProgressBar progress;
    private long lastProgress;

    static boolean isGameReady(File game) {
        return new File(game, "default.xbe").isFile() && new File(game, "pak").isDirectory() &&
                new File(game, "xmv").isDirectory();
    }

    static File gameDirectory(Context context) {
        File internal = new File(context.getFilesDir(), "game");
        if (isGameReady(internal)) {
            return internal;
        }
        File external = context.getExternalFilesDir(null);
        return external == null ? internal : new File(external, "game");
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (isGameReady(gameDirectory(this))) {
            verifyAndPlay();
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        showSetup();
    }

    private void text(String value, int size) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setPadding(0, 12, 0, 12);
        content.addView(view);
    }

    private Button button(String label, Runnable action) {
        Button view = new Button(this);
        view.setText(label);
        view.setOnClickListener(ignored -> action.run());
        content.addView(view);
        return view;
    }

    private void createContent() {
        ScrollView scroll = new ScrollView(this);
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (24 * getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding, padding, padding);
        scroll.addView(content);
        setContentView(scroll);
    }

    private void showSetup() {
        createContent();
        text("TimeSplitters setup", 26);
        text("Import your US Xbox ISO to prepare the game files. Select Import below to begin.", 18);
        File external = getExternalFilesDir(null);
        if (external == null) {
            text("App storage is unavailable. Reconnect storage and reopen the app.", 18);
            return;
        }
        File game = new File(external, "game");
        if (new File(game, "default.xbe").isFile() && !isGameReady(game)) {
            text("default.xbe was found. Also copy the pak and xmv folders from the same Xbox dump.", 18);
        }
        List<File> images = new ArrayList<>();
        for (File directory : new File[] {game, external}) {
            File[] candidates = directory.listFiles(file -> file.isFile() &&
                    file.getName().toLowerCase(Locale.ROOT).endsWith(".iso"));
            if (candidates != null) {
                images.addAll(Arrays.asList(candidates));
            }
        }
        images.sort((left, right) -> left.getName().compareTo(right.getName()));
        if (images.isEmpty()) {
            text("Copy your ISO into this folder, then select Refresh:\n" + external.getAbsolutePath() +
                    "/\n\nADB path:\n/sdcard/Android/data/" + getPackageName() + "/files/", 18);
        }
        for (File image : images) {
            text(image.getName(), 18);
            button("Import " + image.getName(), () -> importImage(image));
        }
        button("Refresh", () -> {
            if (isGameReady(gameDirectory(this))) {
                verifyAndPlay();
            } else {
                showSetup();
            }
        });
    }

    private void verifyAndPlay() {
        if (worker != null) {
            return;
        }
        createContent();
        text("Checking game files…", 22);
        File game = gameDirectory(this);
        worker = new Thread(() -> {
            String failure = null;
            try {
                XboxDisc.verifyExecutable(new File(game, "default.xbe"));
                if (!isGameReady(game)) {
                    throw new IOException("The pak and xmv folders are required beside default.xbe.");
                }
            } catch (IOException | RuntimeException error) {
                failure = error.getMessage();
            }
            final String message = failure;
            runOnUiThread(() -> {
                worker = null;
                if (isDestroyed()) {
                    return;
                }
                if (message == null) {
                    play();
                } else {
                    text("Could not use these game files: " + message, 18);
                    button("Back to setup", this::showSetup);
                }
            });
        }, "Xbox executable check");
        worker.start();
    }

    private void importImage(File source) {
        if (worker != null) {
            return;
        }
        content.removeAllViews();
        text("Importing game", 26);
        status = new TextView(this);
        status.setText("Checking the ISO and executable revision…");
        status.setTextSize(18);
        content.addView(status);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        content.addView(progress);
        Button cancel = button("Cancel import", () -> {
            if (worker != null) {
                worker.interrupt();
                status.setText("Cancelling import…");
            }
        });
        lastProgress = 0;
        worker = new Thread(() -> {
            String result;
            boolean success = false;
            try {
                install(source);
                success = true;
                result = "Import complete. The game is ready.";
            } catch (IOException | RuntimeException error) {
                result = "Import stopped: " + error.getMessage();
            }
            final String message = result;
            final boolean ready = success;
            runOnUiThread(() -> {
                worker = null;
                if (isDestroyed()) {
                    return;
                }
                status.setText(message);
                cancel.setEnabled(false);
                if (ready) {
                    progress.setProgress(100);
                    button("Play TimeSplitters", this::play);
                } else {
                    button("Back to setup", this::showSetup);
                }
            });
        }, "Xbox ISO import");
        worker.start();
    }

    private void install(File source) throws IOException {
        File destination = new File(getFilesDir(), "game");
        File staging = new File(getFilesDir(), ".game-import");
        try (FileOutputStream lockFile = new FileOutputStream(new File(getFilesDir(), ".game-import.lock"), true);
                FileLock lock = lockFile.getChannel().tryLock()) {
            if (lock == null) {
                throw new IOException("Another import is already running.");
            }
            if (destination.exists()) {
                throw new IOException("The internal game folder already exists. Its files were kept.");
            }
            removeStaging(staging);
            try (XboxDisc disc = new XboxDisc(source)) {
                disc.verifySupported();
                if (new StatFs(getFilesDir().getAbsolutePath()).getAvailableBytes() <
                        disc.requiredBytes() + 64L * 1024 * 1024) {
                    throw new IOException("Not enough free storage to extract the game. Free some space and retry.");
                }
                if (!staging.mkdir()) {
                    throw new IOException("Could not create the import folder.");
                }
                disc.extract(staging, (copied, total, file) -> {
                    long now = android.os.SystemClock.elapsedRealtime();
                    if (now - lastProgress >= 250) {
                        lastProgress = now;
                        int percent = total == 0 ? 0 : (int) (100 * copied / total);
                        runOnUiThread(() -> {
                            if (!isDestroyed()) {
                                progress.setProgress(percent);
                                status.setText(percent + "%\n" + file);
                            }
                        });
                    }
                });
                if (Thread.currentThread().isInterrupted()) {
                    throw new InterruptedIOException("Import cancelled.");
                }
                if (!isGameReady(staging) || !staging.renameTo(destination)) {
                    throw new IOException("Could not finish the import.");
                }
            } finally {
                removeStaging(staging);
            }
        }
    }

    private static void removeStaging(File directory) throws IOException {
        if (!directory.exists()) {
            return;
        }
        // Only the app-owned staging tree is removed. Do not follow symlinks.
        if (!Files.isSymbolicLink(directory.toPath()) && directory.isDirectory()) {
            File[] children = directory.listFiles();
            if (children == null) {
                throw new IOException("Could not read interrupted import files.");
            }
            for (File child : children) {
                removeStaging(child);
            }
        }
        Files.delete(directory.toPath());
    }

    private void play() {
        startActivity(new Intent(this, XrActivity.class).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP));
        finish();
    }

    @Override
    public void onBackPressed() {
        if (worker == null) {
            super.onBackPressed();
        } else {
            worker.interrupt();
            if (status != null) {
                status.setText("Cancelling import…");
            }
        }
    }

    @Override
    protected void onDestroy() {
        if (worker != null) {
            worker.interrupt();
        }
        super.onDestroy();
    }
}
