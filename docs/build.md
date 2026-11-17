# Build TimeSplitters for Quest 3

The commands target Apple Silicon macOS. Install Xcode Command Line Tools, Git, Python 3.12 or newer, JDK 17, and Android Platform Tools. In the Android SDK install **Platform 35**, **Build Tools 35.0.0**, **NDK 27.0.12077973** and **CMake 3.22.1**. Allow several gigabytes for the extracted game and build intermediates.

From the Quest fork's root, set the SDK and JDK paths and fetch the pinned dependencies.

```sh
export ANDROID_HOME="/path/to/android/sdk"
export JAVA_HOME="/path/to/jdk-17/Contents/Home"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
mkdir -p external

GIT_LFS_SKIP_SMUDGE=1 git clone --no-checkout \
  https://github.com/TheDoffman/timesplitters-future-perfect-pc.git external/tsfp-pc
GIT_LFS_SKIP_SMUDGE=1 git -C external/tsfp-pc switch --detach be641d9db1f12aa757213388738a24829d5fc083

git clone --branch release-1.1.49 --depth 1 \
  https://github.com/KhronosGroup/OpenXR-SDK-Source.git external/openxr-sdk-source
test "$(git -C external/openxr-sdk-source rev-parse HEAD)" = 47c4761d05b27f884247480ac3fb8a6657907325
```

## Prepare the game dump

Place your extracted **US Xbox** dump in `private/game` or make that path a symbolic link to the dump. If you have an ISO, import it using the pinned PC project's importer.

```sh
mkdir -p private
python3 external/tsfp-pc/tools/build/import-game.py \
  --iso "/path/to/your/game.iso" --output "$PWD/private/game"
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

After generation, `android/build.sh` applies the Quest patches. Keep the dump, generated C files, Android intermediates and signing files local.

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

## Component licenses

The fork preserves the upstream [LICENSE](../LICENSE), [NOTICE](../NOTICE) and [component notices](../LICENSES/README.md). The pinned Xbox runtime includes the SDL, Vulkan-Headers, Vulkan Memory Allocator and volk license files at the pinned submodule revisions. The [OpenXR SDK source](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/47c4761d05b27f884247480ac3fb8a6657907325) carries its own notices, and the Android NDK Shaderc sources retain their licenses. Keep the applicable notices and corresponding sources with any authorized redistribution. The upstream GPL does not grant rights to the original game or its translated logic.
