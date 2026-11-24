# TimeSplitters: Future Perfect for Meta Quest 3

An experimental standalone VR port of the original US Xbox game. The game code is statically recompiled to ARM64 and runs directly on Quest 3 with Vulkan and OpenXR.

## Latest alpha: 0.3.107-xr

This update fixes player-body posing in the deer-room sequence, adjusts campaign
weapon placement in missions 5–8, and updates stereo rendering for Dispersion Gun
impacts, Time Grenade effects and Mag-Charger firing and vision. Mag-Charger vision
uses an intermediate target at eye resolution. Stereo-frame recovery and session
restart handling have also been updated.

The author tested the 0.3.107 release payload on Quest 3 and reported it working.
A full campaign playthrough, every Arcade mode, sustained performance and thermal
behavior have not been established. Some minecart interactions and extended
suspend/resume behavior still need targeted checks.

Quest 3 is the testing target. Quest 2 and Quest 3S compatibility is unverified;
a Quest 2 user reported a black screen after selecting Play.

## What's playable

- Stereoscopic first-person gameplay with 6DoF head tracking. Menus and selected scenes appear on a virtual screen.
- Touch Plus tracking for hands, first-person weapons and aiming, including supported dual-wield weapons. Movement follows your head direction, and turning can be smooth or stepped.
- Wrist-mounted SmartLink with a map and the original beam effect rendered per eye. Selected story effects also render per eye.
- The Story menu, profile creation and reload, the first mission and scenes in later missions have been exercised on Quest 3 in development builds.

XMV videos are skipped. Some effects and later missions still need checks on the headset. See the [test history and open issues](docs/port-status.md).

## Install a release APK

Download `timesplitters-quest3-release.apk` from [GitHub Releases](https://github.com/Yurgil/timesplitters-future-perfect-quest3/releases). You can also [build the release profile locally](docs/build.md).

1. Enable Developer Mode on your Quest 3, connect it over USB and accept the USB debugging prompt. Install Android Platform Tools and run `adb devices` to check the connection. [Meta's sideloading guide](https://developers.meta.com/vr/essentials/quick-start/)
2. Install the APK and open **TimeSplitters Quest** once to initialize storage. Use your own supported **US Xbox** copy, title ID `45410066`.

   ```sh
   adb install -r timesplitters-quest3-release.apk
   ```

3. Choose either an ISO or an already extracted game folder using the instructions below.

### Option 1 — Import an ISO

Copy your ISO directly into `/sdcard/Android/data/org.tsfp.quest/files/` for the release app.

```sh
QUEST_FILES=/sdcard/Android/data/org.tsfp.quest/files
adb shell mkdir -p "$QUEST_FILES"
adb push "/path/to/your/game.iso" "$QUEST_FILES/"
```

The app may display `/storage/emulated/0/Android/data/org.tsfp.quest/files/`. On the tested Quest 3, `/sdcard` resolves to `/storage/emulated/0`, so both spellings refer to the same storage. Preserve the capital **A** in `Android`. For **Fresh Install**, substitute `org.tsfp.quest.cleaninstall` for the package name.

Reopen the app under **Unknown Sources** or select **Refresh**, then select **Import** beside the ISO. The app checks the executable revision and free storage before extracting the game. Keep room for both the ISO and about 4 GB of extracted files. Import starts when you select **Import**. You can cancel it and retry. When it finishes, select **Play TimeSplitters**.

### Option 2 — Copy an extracted game folder

Copy the extracted game folder to the headset as `game`. It must contain `default.xbe`, `pak/` and `xmv/` directly at its top level. The required layout is shown below.

```text
Android/data/org.tsfp.quest/files/game/
├── default.xbe
├── pak/
└── xmv/
```

You can also copy these items into an existing `game` folder using ADB.

```sh
GAME_DIR="/path/to/extracted/game"
QUEST_GAME=/sdcard/Android/data/org.tsfp.quest/files/game
adb shell mkdir -p "$QUEST_GAME"
adb push "$GAME_DIR/default.xbe" "$GAME_DIR/pak" "$GAME_DIR/xmv" "$QUEST_GAME/"
```

Reopen the app or select **Refresh** to use the copied game files. For a first check, choose `Story → 2401 — Time to Split → Normal`. Later launches open the game directly.

For the separate **Fresh Install** test app, use `org.tsfp.quest.cleaninstall` in these paths. Files extracted by the in-app ISO importer are stored in that app's private storage.

Android accepts in-place updates signed by the same key. Back up your profile before switching to a build signed by a different key. Uninstalling removes the app's saves and app-specific game directory.

## Touch Plus controls

| Input | In immersive gameplay |
| --- | --- |
| Head movement | Look and lean |
| Left stick | Move relative to where you are looking |
| Right stick left/right | Turn smoothly or snap by 30°, 45°, or 60° |
| Right stick up/down | Next/previous weapon, one step per flick |
| Tracked controllers | Aim held weapons |
| Right stick click | Toggle the lens on the Vintage Rifle, Sniper Rifle, or Future Sniper |
| Right / left trigger | Xbox RT / LT, including the game's firing actions |
| Right **B** | Reload (Xbox **X**) |
| Left **X** | Change grenade type, or step down one zoom level with the lens open |
| Left **Y** | Change the current weapon's function, or step up one zoom level with the lens open |
| Right **A** | Xbox **A** |
| Left / right Grip | Xbox left / right shoulder inputs |
| Left **Menu** (☰) | Pause / Xbox Start |
| Hold both stick clicks for two seconds | Calibrate the tracked hands |

With Mag-Charger equipped, left **Y** toggles its vision mode.

The right stick aims mounted guns horizontally and vertically. Press left **Y** to leave a turret or vehicle. At a mounted gun, left **X** sends Xbox **B**. In menus and on the virtual screen, the face buttons and sticks keep their original Xbox mappings. Hold both Grips and move the left stick to use the D-pad there.

With a sniper rifle equipped, raise its optic into view and click the right stick. The lens moves with the rifle and shows the same magnified image to both eyes. The world around it stays in stereo. The lens closes when you switch weapons, open a menu, or lose controller tracking.

Open **Settings → VR Options** from the main menu or **Options → VR Options** while paused. **Turn Type** defaults to **Smooth**. Select **Snap** to show **Snap Angle**. Changes apply immediately and are saved between launches.

| VR option | Values |
| --- | --- |
| **Turn Type** | **Smooth** or **Snap** |
| **Snap Angle** | **30 degrees**, **45 degrees**, or **60 degrees**, shown when **Snap** is selected |

For a one-launch snap-turn override, stop the app and restart it with ADB.

```sh
adb shell am force-stop org.tsfp.quest
adb shell am start -n org.tsfp.quest/.XrActivity --ez snap_turn true
```

You can also pass `--ei xr_msaa 1` or `--ei xr_anisotropy 1` at startup. The defaults are 2× MSAA and 4× anisotropic filtering.

## Build from source

The [build guide](docs/build.md) covers the pinned Apple Silicon toolchain, game-code generation, release signing and APK validation. The release profile optimizes the generated game code, strips native debug information and disables the port's capture, profiling and file logging.

The shared OpenXR helpers are included as the pinned `external/RecompVR` Git
submodule. Clone with `--recurse-submodules`, or run
`git submodule update --init --recursive external/RecompVR` in an existing
checkout. See the build guide for development overrides.

On the project Mac, connect Kingston before building. The [storage layout](docs/build.md#local-storage-on-kingston) lists the data paths and their checkout links.

## License and credits

The original Quest port code and documentation are available under [GPL-3.0-only](LICENSE), subject to notices in individual files. The upstream [NOTICE](NOTICE) and [component licenses](LICENSES/README.md) cover separately licensed work.

**Special thanks to TheDoffman** and the [TimeSplitters: Future Perfect Recomp contributors](https://github.com/TheDoffman/timesplitters-future-perfect-pc). Their PC recompilation and shared runtime made this Quest fork possible. Thanks also to [nemonicicon and the ts3FPrecomp contributors](https://github.com/nemonicicon/ts3FPrecomp) and the [xboxrecomp contributors](https://github.com/sp00nz/xboxrecomp).

The shared [RecompVR helpers](https://github.com/Yurgil/RecompVR/tree/f59e9d7abc2c4697c6dae2b6b365b87b3d1603e8) use their own
[MIT license](https://github.com/Yurgil/RecompVR/blob/f59e9d7abc2c4697c6dae2b6b365b87b3d1603e8/LICENSE).

TimeSplitters: Future Perfect was developed by Free Radical Design and published by Electronic Arts. Rights in the original executable, translated game logic and assets remain with their respective rights holders. Generate the translated game code locally from your own supported dump.
