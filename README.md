# TimeSplitters: Future Perfect for Meta Quest 3

An experimental standalone VR port of the original US Xbox game. The game code is statically recompiled to ARM64 and runs directly on Quest 3 with Vulkan and OpenXR.

## What's playable

- Stereoscopic first-person gameplay with 6DoF head tracking. Menus and selected scenes appear on a virtual screen.
- Touch Plus tracking for hands, first-person weapons and aiming, including supported dual-wield weapons. Movement follows your head direction, and turning can be smooth or stepped.
- Wrist-mounted SmartLink with a map and the original beam effect rendered per eye. Selected story effects also render per eye.
- The Story menu, profile creation and reload, the first mission and scenes in later missions have been exercised on Quest 3 in development builds.

XMV videos are skipped. Some effects and later missions still need checks on the headset. See the [test history and open issues](docs/port-status.md).

## Install a release APK

If a Quest APK is available under [GitHub Releases](https://github.com/Yurgil/timesplitters-future-perfect-quest3/releases), download `timesplitters-quest3-release.apk`. You can also [build the release profile locally](docs/build.md).

1. Enable Developer Mode on your Quest 3, connect it over USB and accept the USB debugging prompt. Install Android Platform Tools and run `adb devices` to check the connection. [Meta's sideloading guide](https://developers.meta.com/vr/essentials/quick-start/)
2. Prepare your own extracted **US Xbox** copy, title ID `45410066`, with `default.xbe`, `pak/` and `xmv/` at the top level. If you have an ISO, see [disc import](docs/build.md#prepare-the-game-dump). Verify the dump, install the APK and launch **TimeSplitters Quest** once to initialize storage. Close the app before transferring the game.

   ```sh
   GAME_DIR="/path/to/extracted/game"
   python3 tools/check_game_dump.py "$GAME_DIR"
   adb install -r timesplitters-quest3-release.apk
   ```

3. Copy the game files to the headset and open **TimeSplitters Quest** under **Unknown Sources**. For a first check, choose `Story → 2401 — Time to Split → Normal`.

   ```sh
   QUEST_GAME=/sdcard/Android/data/org.tsfp.quest/files/game
   adb shell mkdir -p "$QUEST_GAME"
   adb push "$GAME_DIR/default.xbe" "$GAME_DIR/pak" "$GAME_DIR/xmv" "$QUEST_GAME/"
   ```

Android accepts in-place updates signed by the same key. Back up your profile before switching to a build signed by a different key. Uninstalling removes the app's saves and app-specific game directory.

## Touch Plus controls

| Input | In immersive gameplay |
| --- | --- |
| Head movement | Look and lean |
| Left stick | Move relative to where you are looking |
| Right stick left/right | Turn smoothly or snap by 30°, 45°, or 60° |
| Tracked controllers | Aim held weapons |
| Right / left trigger | Xbox RT / LT, including the game's firing actions |
| Right **B** | Xbox **X** (reload / action) |
| Left **X** | Change the current weapon's function |
| Left **Y** | Cycle to the next weapon |
| Right **A** | Xbox **A** |
| Left / right Grip | Xbox left / right shoulder inputs |
| Left **Menu** (☰) | Pause / Xbox Start |
| Hold both stick clicks for two seconds | Calibrate the tracked hands |

The right stick aims mounted guns. While mounted, left **X** and **Y** send Xbox **B** and **Y**. In menus and on the virtual screen, the face buttons and sticks keep their original Xbox mappings. Hold both Grips and move the left stick to use the D-pad there.

Open **VR Options** from Settings or the pause menu. **Turn Type** defaults to **Smooth**. Select **Snap** to show **Snap Angle**. Settings are saved for the next launch.

For a one-launch snap-turn override, stop the app and restart it with ADB.

```sh
adb shell am force-stop org.tsfp.quest
adb shell am start -n org.tsfp.quest/.XrActivity --ez snap_turn true
```

You can also pass `--ei xr_msaa 1` or `--ei xr_anisotropy 1` at startup. The defaults are 2× MSAA and 4× anisotropic filtering.

## Build from source

The [build guide](docs/build.md) covers the pinned Apple Silicon toolchain, game-code generation, release signing and APK validation. The release profile optimizes the generated game code, strips native debug information and disables the port's capture, profiling and file logging.

## License and credits

The original Quest port code and documentation are available under [GPL-3.0-only](LICENSE), subject to notices in individual files. The upstream [NOTICE](NOTICE) and [component licenses](LICENSES/README.md) cover separately licensed work.

**Special thanks to TheDoffman** and the [TimeSplitters: Future Perfect Recomp contributors](https://github.com/TheDoffman/timesplitters-future-perfect-pc). Their PC recompilation and shared runtime made this Quest fork possible. Thanks also to [nemonicicon and the ts3FPrecomp contributors](https://github.com/nemonicicon/ts3FPrecomp) and the [xboxrecomp contributors](https://github.com/sp00nz/xboxrecomp).

TimeSplitters: Future Perfect was developed by Free Radical Design and published by Electronic Arts. Rights in the original executable, translated game logic and assets remain with their respective rights holders. Generate the translated game code locally from your own supported dump.
