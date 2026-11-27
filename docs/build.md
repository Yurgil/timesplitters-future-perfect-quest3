# Build TimeSplitters for Quest 3

The commands target Apple Silicon macOS. Install Xcode Command Line Tools, Git, Python 3.12 or newer, JDK 17, and Android Platform Tools. In the Android SDK install **Platform 35**, **Build Tools 35.0.0**, **NDK 27.0.12077973** and **CMake 3.22.1**. Allow several gigabytes for the extracted game and build intermediates.

From the Quest fork's root, set the SDK and JDK paths and fetch the pinned dependencies.

```sh
export ANDROID_HOME="/path/to/android/sdk"
export JAVA_HOME="/path/to/jdk-17/Contents/Home"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
mkdir -p external
git submodule update --init --recursive external/RecompVR

GIT_LFS_SKIP_SMUDGE=1 git clone --no-checkout \
  https://github.com/TheDoffman/timesplitters-future-perfect-pc.git external/tsfp-pc
GIT_LFS_SKIP_SMUDGE=1 git -C external/tsfp-pc switch --detach be641d9db1f12aa757213388738a24829d5fc083

git clone --branch release-1.1.49 --depth 1 \
  https://github.com/KhronosGroup/OpenXR-SDK-Source.git external/openxr-sdk-source
test "$(git -C external/openxr-sdk-source rev-parse HEAD)" = 47c4761d05b27f884247480ac3fb8a6657907325
```

## Shared RecompVR source

RecompVR is pinned as the `external/RecompVR` Git submodule. A checkout created
with `git clone --recurse-submodules` includes it automatically. After pulling
changes that update its pin, run:

```sh
git submodule update --init --recursive external/RecompVR
```

`android/build.sh` explicitly selects this copy, including when a reused CMake
cache points to a previous shared directory. For development, set
`TSFP_RECOMPVR_ROOT=/absolute/path/to/RecompVR` before running the script; direct
CMake builds accept `-DRECOMPVR_ROOT=/absolute/path/to/RecompVR`. Updating a
submodule pin is a deliberate source change and requires validation in this
game. Other projects can pin their own revisions independently.

## Local storage on Kingston

On the project Mac, mount **KINGSTON** before generating code, building or accessing the extracted dump. The data root is `/Volumes/KINGSTON/StandaloneVRProjects/data/timesplitters-future-perfect-quest3`. The checkout uses these ignored symbolic links, so commands still run from the repository using the paths below.

| Checkout path | Directory under the data root |
| --- | --- |
| `private/game` | `game` |
| `external/tsfp-pc/external/ts3FPrecomp/titles/futureperfect/src/recomp/gen` | `recomp-gen` |
| `external/tsfp-pc/private/pipeline` | `pipeline` |
| `captures` (historical screenshots) | `captures` |

On this Mac, the checkout, local tools and signing files live on the mounted `/Volumes/StandaloneVRProjects` APFS volume backed by Kingston. The `build` link uses `/Volumes/StandaloneVRProjects/builds/timesplitters-future-perfect-quest3` for current native builds. Historical APKs and the original US Xbox ISO are under `StandaloneVRProjects/data` on Kingston. The build stages APK packaging in the workspace `build-tmp` directory. Follow the README's installation steps to provide the game data separately on the headset.

For a fresh checkout, normal local directories work on any supported Mac. To use Kingston, connect it and verify the mount with `mount | grep ' on /Volumes/KINGSTON '` before creating the project directory and the links. Create links only where the checkout path is absent. If a path already contains data, copy it and verify the copy before replacing the directory with a link.

```sh
data=/Volumes/KINGSTON/StandaloneVRProjects/data/timesplitters-future-perfect-quest3
mkdir -p "$data/game" "$data/recomp-gen" "$data/pipeline" private build
ln -s "$data/game" private/game
```

After preparing the Xbox runtime in the next section and before generating C, create the other two build links from the Quest repository root.

```sh
pc=external/tsfp-pc
recomp="$pc/external/ts3FPrecomp/titles/futureperfect/src/recomp"
mkdir -p "$pc/private" "$recomp"
ln -s "$data/pipeline" "$pc/private/pipeline"
ln -s "$data/recomp-gen" "$recomp/gen"
```

If `build` is a link, `android/build.sh` reads or creates the debug keystore at `private/signing/debug.keystore`. For an existing installation, preserve its debug signing identity by copying the previous profile's `debug.keystore` there before moving the build tree.

## Prepare the game dump

Place your extracted **US Xbox** dump in `private/game` or make that path a symbolic link to the dump. If you have an ISO, import it using the pinned PC project's importer. On this Mac, Kingston uses exFAT and the importer needs hard links, so import into a temporary directory on the internal APFS volume first.

```sh
mkdir -p private
game_dir="$PWD/private/game"
# For Kingston, use game_dir="$(mktemp -d /private/tmp/tsfp-game.XXXXXX)".
python3 external/tsfp-pc/tools/build/import-game.py \
  --iso "/path/to/your/game.iso" --output "$game_dir"
```

For Kingston, copy from the temporary directory into an empty `game` directory. Check that the final command reports no differences before removing the temporary copy.

```sh
rsync -rt "$game_dir/" "$data/game/"
rsync -rctni --modify-window=1 "$game_dir/" "$data/game/"
```

For either path, check the executable revision and presence of `pak/` and `xmv/`.

```sh
python3 tools/check_game_dump.py private/game
```

## Generate the game code

Prepare the pinned Xbox runtime (`d90c92988a9f99d0580d2ca3f7efd6ec3a7b7845`), apply the PC patches and generate C from your verified XBE.

```sh
root="$PWD"
pc="$root/external/tsfp-pc"
runtime="$pc/external/ts3FPrecomp"

python3 -m venv "$pc/.venv"
py="$pc/.venv/bin/python"
"$py" -m pip install -r "$pc/requirements.txt"
"$py" "$pc/tools/build/prepare-runtime.py" --destination "$runtime"
"$py" "$pc/tools/build/apply-patches.py" "$runtime" "$pc/patches"
git -C "$runtime" submodule update --init \
  third_party/SDL third_party/XbSymbolDatabase third_party/Vulkan-Headers \
  third_party/volk third_party/VulkanMemoryAllocator

scanner="$runtime/third_party/XbSymbolDatabase"
"$pc/.venv/bin/cmake" -S "$scanner" -B "$scanner/build" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$pc/.venv/bin/ninja" -DCMAKE_BUILD_TYPE=Release -DXBSDB_BUILD_UNITTEST=OFF
"$pc/.venv/bin/cmake" --build "$scanner/build" --target XbSymbolDatabaseCLI

(
  cd "$runtime"
  "$py" -m tools.xdk_symbols "$root/private/game/default.xbe"
  "$py" scripts/recompile.py "$root/private/game/default.xbe" \
    --work-dir "$pc/private/pipeline" --project titles/futureperfect \
    --game-name "TimeSplitters: Future Perfect" --trace-all-entries
)
```

After generation, `android/build.sh` applies the Quest patches. Keep the dump, generated C files, Android intermediates and signing files out of Git.

## Sign and build a release APK

If you have a release key, point the three `TSFP_*` variables below to your keystore, alias and private password file. Otherwise generate a key once. Keep it for future updates.

```sh
(
  set -eu
  umask 077
  mkdir -p private/signing
  set -C
  openssl rand -hex 24 > private/signing/password
  keytool -genkeypair -keystore private/signing/quest-release.p12 -storetype PKCS12 \
    -storepass:file private/signing/password -alias tsfp-quest3 \
    -keyalg RSA -keysize 3072 -validity 10000 -dname 'CN=TimeSplitters Quest 3'
)
```

```sh
export TSFP_KEYSTORE="$PWD/private/signing/quest-release.p12"
export TSFP_KEY_ALIAS=tsfp-quest3
export TSFP_KEYSTORE_PASSWORD_FILE="$PWD/private/signing/password"
android/build.sh --release
```

The APK is written to `build/android-release/timesplitters-quest3-release.apk`. The release builds the OpenXR loader directly from the pinned Khronos SDK. The APK is non-debuggable and packages only the ARM64 ABI.

```sh
apk=build/android-release/timesplitters-quest3-release.apk
tools="$ANDROID_HOME/build-tools/35.0.0"
"$tools/apksigner" verify --print-certs "$apk"
"$tools/zipalign" -c -P 16 4 "$apk"
"$tools/aapt2" dump badging "$apk"
```

Follow the [installation instructions](../README.md#install-a-release-apk) using this APK. Saves live in the app's private data directory. For diagnostic builds, see [M0](m0.md) and [M3](m3.md).

## Catacombs cart test

Build the optimized ARM64 runtime first using the release build above. Run
the scenario packager against that build and a debug-signed APK for this
checkout. The reference APK supplies the OpenXR loader and signing identity.

```sh
python3 android/tests/build_cart_segment.py \
  build/android-release/timesplitters-quest3-0.3.108-xr-mech-reticle.apk \
  build/android-release/timesplitters-quest3-0.3.108-xr-cart-test-r3.apk --revision=-r3 --diagnostic
```

The test package uses the installed game's application ID and imported game
data. Updating closes the open game session. On first launch it copies
`files/saves/UserData` to `files/cart-test-saves/UserData` to make the same
missions available in the test profile. The test stores its progress there.

Choose mission 6 (What Lies Below) in single-player Story. As soon as the
intro hands control to the player, the test runs the authored minecart spawn
action and refreshes the player room at the track. Use the right stick, look
down, then fire while moving the weapon hand. Reload the mission for another run.

```sh
adb logcat -s TSFP_CART
```

Look for `cart action executed`, `board ok`, `turn`, `body draw` and `shot`.
The shot entry records the guest call site, muzzle, direction and the cart entity origin.
The diagnostic build also keeps boarding output in `files/boot.log` and supports
the scripted input and frame readback described in [M2](m2.md).

## Component licenses

RecompVR has its own [MIT license](https://github.com/Yurgil/RecompVR/blob/f59e9d7abc2c4697c6dae2b6b365b87b3d1603e8/LICENSE). Include this
notice when distributing the library or a linked game build. Its source version
is the commit pinned by the submodule; it does not change the Quest port's license.

The fork preserves the upstream [LICENSE](../LICENSE), [NOTICE](../NOTICE) and [component notices](../LICENSES/README.md). The pinned Xbox runtime includes the SDL, Vulkan-Headers, Vulkan Memory Allocator and volk license files at the pinned submodule revisions. The [OpenXR SDK source](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/47c4761d05b27f884247480ac3fb8a6657907325) carries its own notices, and the Android NDK Shaderc sources retain their licenses. Keep the applicable notices and corresponding sources with any authorized redistribution. The upstream GPL does not grant rights to the original game or its translated logic.
