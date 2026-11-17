package org.tsfp.quest;

import android.os.Bundle;
import android.view.WindowManager;
import android.widget.Toast;
import java.io.File;
import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (!BuildConfig.DIAGNOSTICS && !new File(gameDirectory(), "default.xbe").isFile()) {
            Toast.makeText(this, "Copy your Xbox game files to Android/data/org.tsfp.quest/files/game",
                    Toast.LENGTH_LONG).show();
        }
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
        String turnOverride = getIntent().hasExtra("snap_turn")
                ? (getIntent().getBooleanExtra("snap_turn", false) ? "snap" : "smooth") : "";
        String vrOptions = new File(files, "vr-options.txt").getAbsolutePath();
        if (!BuildConfig.DIAGNOSTICS) {
            return new String[] {
                gameDirectory().getAbsolutePath(), new File(files, "saves").getAbsolutePath(),
                cache.getAbsolutePath(), "", "", useOpenXR() ? "openxr" : "", "",
                useOpenXR() ? "stereo_geometry" : "",
                turnOverride,
                Integer.toString(getIntent().getIntExtra("xr_msaa", 2)),
                Integer.toString(getIntent().getIntExtra("xr_anisotropy", 4)), vrOptions
            };
        }
        boolean benchmark = getIntent().getBooleanExtra("benchmark", false);
        boolean stereoProbe = getIntent().getBooleanExtra("stereo_probe", false);
        boolean projectionProbe = getIntent().getBooleanExtra("projection_probe", false);
        boolean stereoGeometry = getIntent().getBooleanExtra("stereo_geometry",
                useOpenXR() && !benchmark && !stereoProbe && !projectionProbe);
        return new String[] {
            new File(files, "game").getAbsolutePath(),
            (benchmark ? new File(cache, "benchmark-saves") : new File(files, "saves")).getAbsolutePath(),
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
            Integer.toString(getIntent().getIntExtra("xr_anisotropy", 4)), vrOptions
        };
    }

    private File gameDirectory() {
        File internal = new File(getFilesDir(), "game");
        if (new File(internal, "default.xbe").isFile()) {
            return internal;
        }
        File external = getExternalFilesDir(null);
        return external == null ? internal : new File(external, "game");
    }

    protected boolean useOpenXR() {
        return false;
    }
}
