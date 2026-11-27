"""Relink the cart scenario against an existing optimized Quest build."""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
ANDROID = ROOT / "android"
BUILD = ROOT / "build/android-release"
TOOLCHAIN = ROOT.parent / "rechan-quest/.toolchain"


def verify_certificate(signer, apk, env):
    report = subprocess.check_output(
        [signer, "verify", "--print-certs", apk], env=env, text=True)
    return next(line for line in report.splitlines() if "certificate SHA-256 digest" in line)


def build(base, output, work, revision, diagnostic):
    sdk = Path(os.environ.get("ANDROID_HOME", TOOLCHAIN / "sdk")).resolve()
    java = Path(os.environ.get("JAVA_HOME", TOOLCHAIN / "jdk/Contents/Home")).resolve()
    ndk = sdk / "ndk/27.0.12077973/toolchains/llvm/prebuilt/darwin-x86_64/bin"
    ninja = sdk / "cmake/3.22.1/bin/ninja"
    tools = sdk / "build-tools/35.0.0"
    jar = sdk / "platforms/android-35/android.jar"
    env = dict(os.environ, JAVA_HOME=str(java), PATH=f"{java}/bin:{os.environ['PATH']}")
    target_list = subprocess.check_output([ninja, "-t", "targets", "all"], cwd=BUILD, text=True)

    def command(target):
        lines = subprocess.check_output([ninja, "-t", "commands", target], cwd=BUILD, text=True)
        return shlex.split(lines.splitlines()[-1])

    def compile_source(name, *options):
        target = next(line.split(": ", 1)[0] for line in target_list.splitlines()
                      if line.split(": ", 1)[0].endswith(f"/{name}.c.o"))
        args = command(target)
        if "-DTSFP_RELEASE" not in args:
            raise RuntimeError("Expected the optimized release build in build/android-release")
        obj = work / f"{name}.c.o"
        args[args.index("-o") + 1] = str(obj)
        args[args.index("-MF") + 1] = f"{obj}.d"
        subprocess.run([*args, "-DTSFP_CART_TEST", f"-I{ANDROID}", *options], cwd=BUILD, check=True)
        return obj, target

    probe, _ = compile_source("stereo_probe")
    xr, _ = compile_source("xr")
    hook_options = ("-DTSFP_CART_DIAGNOSTIC",) if diagnostic else ()
    hooks, hooks_target = compile_source("quest_guest_hooks", *hook_options)
    boot = compile_source("boot", "-UTSFP_RELEASE") if diagnostic else None
    archive_target = "xbox-runtime/src/d3d/libxbox_d3d8.a"
    archive = work / "libxbox_d3d8.a"
    shutil.copyfile(BUILD / archive_target, archive)
    subprocess.run([ndk / "llvm-ar", "r", archive, probe, xr], check=True)

    native = work / "libtsfp_boot.so"
    args = command("tsfp_boot")
    if args[:2] != [":", "&&"]:
        raise RuntimeError("Unexpected Ninja link command")
    args = args[2:args.index("&&", 2)]
    args[args.index("-o") + 1] = str(native)
    replacements = [(archive_target, archive), (hooks_target, hooks)]
    if boot:
        replacements.append((boot[1], boot[0]))
    for original, replacement in replacements:
        args[args.index(original)] = str(replacement)
    subprocess.run([*args, "-Wl,--wrap=sub_001B7680", "-Wl,--wrap=sub_000D5EB0"],
                   cwd=BUILD, check=True)
    # Confirm that the already compiled original frame loop really reaches
    # the test hook. A successful link alone also allows an unused wrapper.
    calls = subprocess.check_output(
        [ndk / "llvm-objdump", "-d", "--disassemble-symbols=sub_001CE850", native], text=True)
    if not any("bl" in line and "<__wrap_sub_000D5EB0" in line for line in calls.splitlines()):
        raise RuntimeError("Original player update does not call the test wrapper")
    stripped = work / "libtsfp_boot-stripped.so"
    subprocess.run([ndk / "llvm-strip", "--strip-debug", "-o", stripped, native], check=True)

    # Use the same Java source and OpenXR loader as the regular Quest package.
    config = work / "BuildConfig.java"
    config.write_text("package org.tsfp.quest;\n"
                      f"final class BuildConfig {{ static final boolean DIAGNOSTICS = {str(diagnostic).lower()}; "
                      "static final boolean CART_TEST = true; }\n")
    sdl = (ROOT / "external/tsfp-pc/external/ts3FPrecomp/third_party/SDL/"
           "android-project/app/src/main/java/org/libsdl/app")
    classes = work / "classes"
    classes.mkdir()
    subprocess.run([java / "bin/javac", "--release", "8", "-classpath", jar, "-d", classes,
                    *sorted(sdl.glob("*.java")), *sorted((ANDROID / "java/org/tsfp/quest").glob("*.java")),
                    config], env=env, check=True)
    class_archive = work / "classes.jar"
    with zipfile.ZipFile(class_archive, "w") as out:
        for entry in sorted(classes.rglob("*.class")):
            out.write(entry, entry.relative_to(classes).as_posix())
    subprocess.run([tools / "d8", "--min-api", "29", "--lib", jar, "--output", work, class_archive],
                   env=env, check=True)

    manifest = work / "AndroidManifest.xml"
    source = (ANDROID / "AndroidManifest.xml").read_text()
    source = re.sub(r'android:versionName="([^"]+)"', lambda match: f'android:versionName="{match[1]}-cart-test{revision}"',
                    source, count=1)
    source = source.replace('android:label="TimeSplitters Quest"', 'android:label="TimeSplitters Quest Cart Test"')
    if diagnostic:
        source = source.replace('android:allowBackup="false"',
                                'android:allowBackup="false" android:debuggable="true"')
    manifest.write_text(source)
    unsigned = work / "unsigned.apk"
    subprocess.run([tools / "aapt2", "link", "-I", jar, "--manifest", manifest, "-o", unsigned], check=True)
    with zipfile.ZipFile(base) as source_apk, zipfile.ZipFile(unsigned, "a") as test_apk:
        test_apk.write(work / "classes.dex", "classes.dex")
        loader = "lib/arm64-v8a/libopenxr_loader.so"
        test_apk.writestr(loader, source_apk.read(loader))
        test_apk.write(stripped, "lib/arm64-v8a/libtsfp_boot.so")

    aligned, signed = work / "aligned.apk", work / "signed.apk"
    subprocess.run([tools / "zipalign", "-P", "16", "-f", "4", unsigned, aligned], check=True)
    subprocess.run([tools / "apksigner", "sign", "--ks", ROOT / "private/signing/debug.keystore",
                    "--ks-key-alias", "androiddebugkey", "--ks-pass", "pass:android",
                    "--v4-signing-enabled", "false", "--out", signed, aligned], env=env, check=True)
    if verify_certificate(tools / "apksigner", signed, env) != verify_certificate(tools / "apksigner", base, env):
        raise RuntimeError("Test APK signer differs from the supplied debug-signed base")
    subprocess.run([tools / "zipalign", "-c", "-P", "16", "4", signed], check=True)
    with output.open("xb") as destination, signed.open("rb") as source_apk:
        shutil.copyfileobj(source_apk, destination)
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_apk", type=Path, help="Debug-signed Quest APK matching the optimized build")
    parser.add_argument("output_apk", type=Path)
    parser.add_argument("--revision", default="")
    parser.add_argument("--diagnostic", action="store_true",
                        help="Enable run-as, boot logs, frame captures and scripted input")
    args = parser.parse_args()
    if args.output_apk.exists():
        parser.error("output APK already exists")
    with tempfile.TemporaryDirectory(prefix="tsfp-cart-segment-") as work:
        build(args.base_apk.resolve(strict=True), args.output_apk.resolve(), Path(work), args.revision, args.diagnostic)


if __name__ == "__main__":
    main()
