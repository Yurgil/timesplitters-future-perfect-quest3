package org.tsfp.quest;

import android.os.Bundle;
import android.view.WindowManager;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Iterator;
import java.util.stream.Stream;
import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected String[] getLibraries() {
        // SDL and the game runtime are linked into the same shared library.
        return new String[] { "tsfp_boot" };
    }

    @Override
    protected String getMainFunction() {
        return "tsfp_android_main";
    }

    @Override
    protected String[] getArguments() {
        File files = getFilesDir();
        File cache = getCacheDir();
        File saves = BuildConfig.CART_TEST ? cartTestSaves(files) : new File(files, "saves");
        String turnOverride = getIntent().hasExtra("snap_turn")
                ? (getIntent().getBooleanExtra("snap_turn", false) ? "snap" : "smooth") : "";
        String vrOptions = new File(files, "vr-options.txt").getAbsolutePath();
        if (!BuildConfig.DIAGNOSTICS) {
            return new String[] {
                gameDirectory().getAbsolutePath(), saves.getAbsolutePath(),
                cache.getAbsolutePath(), "", "", useOpenXR() ? "openxr" : "", "",
                useOpenXR() ? "stereo_geometry" : "",
                turnOverride,
                Integer.toString(getIntent().getIntExtra("xr_msaa", 2)),
                Integer.toString(getIntent().getIntExtra("xr_anisotropy", 4)), vrOptions,
                new File(files, "hand-calibration.txt").getAbsolutePath()
            };
        }
        boolean benchmark = getIntent().getBooleanExtra("benchmark", false);
        boolean stereoProbe = getIntent().getBooleanExtra("stereo_probe", false);
        boolean projectionProbe = getIntent().getBooleanExtra("projection_probe", false);
        boolean stereoGeometry = getIntent().getBooleanExtra("stereo_geometry",
                useOpenXR() && !benchmark && !stereoProbe && !projectionProbe);
        return new String[] {
            gameDirectory().getAbsolutePath(),
            (benchmark ? new File(cache, "benchmark-saves") : saves).getAbsolutePath(),
            cache.getAbsolutePath(),
            new File(files, "boot.log").getAbsolutePath(),
            getIntent().getBooleanExtra("capture_frames", false)
                    ? new File(cache, "frame").getAbsolutePath() : "",
            useOpenXR() ? "openxr" : "",
            benchmark ? new File(cache, "present.csv").getAbsolutePath() : "",
            stereoGeometry ? "stereo_geometry" : projectionProbe ? "projection_probe"
                : stereoProbe ? "stereo_probe" : "",
            turnOverride,
            Integer.toString(getIntent().getIntExtra("xr_msaa", 2)),
            Integer.toString(getIntent().getIntExtra("xr_anisotropy", 4)), vrOptions,
            new File(files, "hand-calibration.txt").getAbsolutePath()
        };
    }

    private static File cartTestSaves(File files) {
        File test = new File(files, "cart-test-saves");
        Path source = new File(files, "saves/UserData").toPath();
        Path userData = new File(test, "UserData").toPath();
        Path staged = new File(test, "UserData.seed").toPath();
        try {
            Files.createDirectories(test.toPath());
            if (Files.isDirectory(source) && !Files.exists(userData)) {
                try (Stream<Path> entries = Files.walk(source)) {
                    Iterator<Path> entry = entries.iterator();
                    while (entry.hasNext()) {
                        Path path = entry.next();
                        Path target = staged.resolve(source.relativize(path));
                        if (Files.isDirectory(path)) {
                            Files.createDirectories(target);
                        } else if (Files.isRegularFile(path)) {
                            Files.copy(path, target, StandardCopyOption.REPLACE_EXISTING);
                        }
                    }
                }
                Files.move(staged, userData, StandardCopyOption.ATOMIC_MOVE);
            }
        } catch (IOException e) {
            throw new IllegalStateException("Could not prepare Catacombs test saves", e);
        }
        return test;
    }

    private File gameDirectory() {
        return LauncherActivity.gameDirectory(this);
    }

    protected boolean useOpenXR() {
        return false;
    }
}
