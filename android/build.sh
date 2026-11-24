#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd -P)
if [[ -r "$root/../build-env.sh" ]]; then
    source "$root/../build-env.sh"
fi
sdk=${ANDROID_HOME:-"$root/../rechan-quest/.toolchain/sdk"}
java_home=${JAVA_HOME:-"$root/../rechan-quest/.toolchain/jdk/Contents/Home"}
export JAVA_HOME="$java_home"
export PATH="$java_home/bin:$PATH"
ndk="$sdk/ndk/27.0.12077973"
cmake="$sdk/cmake/3.22.1/bin"
tools="$sdk/build-tools/35.0.0"
android_jar="$sdk/platforms/android-35/android.jar"
pc="$root/external/tsfp-pc"
runtime="$pc/external/ts3FPrecomp"
recompvr=${TSFP_RECOMPVR_ROOT:-"$root/external/RecompVR"}
if [[ ! -f "$recompvr/CMakeLists.txt" || ! -f "$recompvr/include/recompvr/openxr.h" ]]; then
    printf 'RecompVR source is unavailable: %s\nRun from the checkout root: git submodule update --init --recursive external/RecompVR\n' \
        "$recompvr" >&2
    exit 1
fi
recompvr=$(cd "$recompvr" && pwd -P)
output="$root/build/android"
release=OFF
optimize_guest=OFF
if [[ ${1:-} == --release ]]; then
    release=ON
    output="$root/build/android-release"
    optimize_guest=ON
    : "${TSFP_KEYSTORE:?Set TSFP_KEYSTORE to your release keystore}"
    : "${TSFP_KEY_ALIAS:?Set TSFP_KEY_ALIAS to your release key alias}"
    : "${TSFP_KEYSTORE_PASSWORD_FILE:?Set TSFP_KEYSTORE_PASSWORD_FILE to the local password file}"
elif [[ ${TSFP_OPTIMIZE_GUEST:-0} == 1 ]]; then
    output="$root/build/android-optimized"
    optimize_guest=ON
fi
if [[ $# -gt 1 || ( $# -eq 1 && $release == OFF ) ]]; then
    echo "Usage: $0 [--release]" >&2
    exit 2
fi
key="$output/debug.keystore"
if [[ -L "$root/build" ]]; then
    key="$root/private/signing/debug.keystore"
fi

guest_gen="$runtime/titles/futureperfect/src/recomp/gen"
for path in "$root/build" "$guest_gen"; do
    if [[ -L "$path" && ! -d "$path" ]]; then
        printf 'Storage is unavailable: %s -> %s\nMount the project data drive (docs/build.md).\n' \
            "$path" "$(readlink "$path")" >&2
        exit 1
    fi
done

apply_patch() {
    local checkout=$1 patch="$root/patches/$2"
    if ! git -C "$checkout" apply --reverse --check "$patch" 2>/dev/null; then
        git -C "$checkout" apply "$patch"
    fi
}
apply_patch "$pc" android-proc-maps.patch
apply_patch "$pc" android-release-logging.patch
apply_patch "$runtime" legacy-gl-ambient-occlusion.patch
# Later patches edit the context of earlier ones, so an applied stack is
# recognised by its last patch.
runtime_patches=(
    android-runtime-boot.patch
    android-vulkan-runtime.patch
    android-openxr.patch
    android-hitch-trace.patch
    android-guest-byte-read.patch
    android-stereo-camera.patch
    android-stereo-vertex.patch
    android-stereo-world.patch
    android-stereo-avatar.patch
)
runtime_followups=(
    android-guest-span-read.patch
    android-uplink-render.patch
    android-uplink-physics.patch
    android-xr-quality.patch
    android-frame-time.patch
    android-xr-weapon-effects.patch
    android-xr-screen-effects.patch
    android-xr-uplink-original.patch
    android-xr-story-effects.patch
    android-xr-train-effects.patch
    android-xr-paired-clears.patch
    android-xr-effect-scope.patch
    android-xr-scene-copy-resolution.patch
    android-xr-turret-reticle.patch
    android-xr-uplink-map.patch
    android-xr-sniper-scope.patch
    android-xr-ghosts.patch
    android-xr-turret-sprites.patch
    android-xr-cinematics.patch
    android-xr-chapter-mattes.patch
    android-xr-matte-fallback.patch
    android-xr-mag-charger-resolution.patch
    android-xr-weapon-effect-logging.patch
)
# Check the newest matching patch first because later patches can change
# context needed to reverse-check an earlier one.
count=${#runtime_followups[@]}
for (( applied=count-1; applied>=0; --applied )); do
    if git -C "$runtime" apply --reverse --check "$root/patches/${runtime_followups[applied]}" 2>/dev/null; then
        break
    fi
done
if (( applied < 0 )) &&
    ! git -C "$runtime" apply --reverse --check "$root/patches/${runtime_patches[${#runtime_patches[@]}-1]}" 2>/dev/null; then
    for patch in "${runtime_patches[@]}"; do
        apply_patch "$runtime" "$patch"
    done
fi
for (( next=applied+1; next<count; ++next )); do
    apply_patch "$runtime" "${runtime_followups[next]}"
done
apply_patch "$runtime" android-vr-options.patch
apply_patch "$runtime" android-xr-catacombs-trace.patch

xr_api="$root/build/openxr-api"
xr_source="$root/external/openxr-sdk-source"
if [[ $release == ON ]]; then
    xr_api="$root/build/openxr-release-api"
    xr_build="$root/build/openxr-loader"
    "$cmake/cmake" -S "$xr_source" -B "$xr_build" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$cmake/ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29 -DANDROID_STL=c++_static \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=OFF \
        -DBUILD_CONFORMANCE_TESTS=OFF
    "$cmake/cmake" --build "$xr_build" --target openxr_loader --parallel "${TSFP_JOBS:-6}"
    mkdir -p "$xr_api/include/openxr"
    cp -p "$xr_source/include/openxr/"*.h "$xr_build/include/openxr/"*.h "$xr_api/include/openxr/"
    cp -p "$xr_build/src/loader/libopenxr_loader.so" "$xr_api/"
else
xr_sample="$root/external/openxr-sdk-source/src/tests/hello_xr"
xr_apk="$root/build/openxr-diagnostic/hello-xr-vulkan-arm64-debug.apk"
if [[ ! -f "$xr_apk" || ! -d "$xr_sample/.cxx" ]]; then
    echo "Build the pinned OpenXR diagnostic APK first (docs/m0.md)." >&2
    exit 1
fi
xr_header=$(find "$xr_sample/.cxx" -path '*/arm64-v8a/include/openxr/openxr.h' -print -quit)
if [[ ! -f "$xr_header" ]]; then
    echo "OpenXR generated headers are missing. Rebuild the diagnostic APK." >&2
    exit 1
fi
mkdir -p "$xr_api/include/openxr"
cp -p "$(dirname "$xr_header")/"*.h "$xr_api/include/openxr/"
if [[ ! -f "$xr_api/libopenxr_loader.so" || "$xr_apk" -nt "$xr_api/libopenxr_loader.so" ]]; then
    unzip -p "$xr_apk" lib/arm64-v8a/libopenxr_loader.so > "$xr_api/libopenxr_loader.so.tmp"
    mv "$xr_api/libopenxr_loader.so.tmp" "$xr_api/libopenxr_loader.so"
fi
fi

shaderc="$root/build/shaderc"
if [[ ! -f "$shaderc/libs/c++_static/arm64-v8a/libshaderc.a" ]]; then
    "$ndk/ndk-build" -j "${TSFP_JOBS:-6}" NDK_PROJECT_PATH=null \
        APP_BUILD_SCRIPT="$ndk/sources/third_party/shaderc/Android.mk" \
        APP_ABI=arm64-v8a APP_PLATFORM=android-29 APP_STL=c++_static \
        NDK_OUT="$shaderc/obj" NDK_LIBS_OUT="$shaderc/libs" libshaderc_combined
fi

"$cmake/cmake" -S "$root/android" -B "$output" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$cmake/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DTSFP_BUILD_RUNTIME=ON \
    -DTSFP_RELEASE="$release" -DXR_API="$xr_api" -DRECOMPVR_ROOT="$recompvr" \
    -DTSFP_OPTIMIZE_GUEST="$optimize_guest"
"$cmake/cmake" --build "$output" --target tsfp_boot --parallel "${TSFP_JOBS:-6}"

# Keep APK staging on the host filesystem so ZIP never sees exFAT metadata.
stage=$(mktemp -d "${TMPDIR:-/tmp}/tsfp-apk-stage.XXXXXX")
apk_tmp=
cleanup() {
    rm -rf "$stage"
    if [[ -n "$apk_tmp" ]]; then
        rm -f "$apk_tmp"
    fi
}
trap cleanup EXIT
mkdir -p "$stage/classes" "$stage/lib/arm64-v8a"
SDL_JAVA="$runtime/third_party/SDL/android-project/app/src/main/java/org/libsdl/app"
manifest="$root/android/AndroidManifest.xml"
diagnostics=false
if [[ $release == OFF ]]; then
    diagnostics=true
    sed 's/android:allowBackup="false"/android:allowBackup="false" android:debuggable="true"/' \
        "$manifest" > "$stage/AndroidManifest.xml"
    manifest="$stage/AndroidManifest.xml"
fi
printf 'package org.tsfp.quest;\nfinal class BuildConfig { static final boolean DIAGNOSTICS = %s; }\n' \
    "$diagnostics" > "$stage/BuildConfig.java"
javac --release 8 -classpath "$android_jar" -d "$stage/classes" \
    "$SDL_JAVA"/*.java "$root/android/java/org/tsfp/quest/"*.java "$stage/BuildConfig.java"
(cd "$stage/classes" && zip -q -r "$stage/classes.jar" .)
"$tools/d8" --min-api 29 --lib "$android_jar" --output "$stage" \
    "$stage/classes.jar"
"$tools/aapt2" link -I "$android_jar" --manifest "$manifest" \
    -o "$stage/unsigned.apk"
strip=$(sed -n 's/^CMAKE_STRIP:FILEPATH=//p' "$output/CMakeCache.txt")
"$strip" --strip-debug -o "$stage/lib/arm64-v8a/libtsfp_boot.so" "$output/libtsfp_boot.so"
"$strip" --strip-debug -o "$stage/lib/arm64-v8a/libopenxr_loader.so" "$xr_api/libopenxr_loader.so"
(cd "$stage" && zip -q -0 unsigned.apk classes.dex lib/arm64-v8a/*.so)
"$tools/zipalign" -P 16 -f 4 "$stage/unsigned.apk" "$stage/aligned.apk"

if [[ $release == ON ]]; then
    apk="$output/timesplitters-quest3-release.apk"
    "$tools/apksigner" sign --ks "$TSFP_KEYSTORE" --ks-key-alias "$TSFP_KEY_ALIAS" \
        --ks-pass "file:$TSFP_KEYSTORE_PASSWORD_FILE" --out "$stage/signed.apk" "$stage/aligned.apk"
else
    if [[ ! -f "$key" ]]; then
        mkdir -p "$(dirname "$key")"
        keytool -genkeypair -keystore "$key" -storepass android -keypass android \
            -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
            -dname 'CN=Android Debug,O=Android,C=US'
    fi
    apk="$output/timesplitters-xr-debug.apk"
    "$tools/apksigner" sign --ks "$key" --ks-pass pass:android \
        --out "$stage/signed.apk" "$stage/aligned.apk"
fi
"$tools/apksigner" verify "$stage/signed.apk"
apk_tmp=$(mktemp "$output/.tsfp-apk.XXXXXX")
cp -p "$stage/signed.apk" "$apk_tmp"
mv -f "$apk_tmp" "$apk"
printf '%s\n' "$apk"
