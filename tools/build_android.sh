#!/bin/bash
# The Android app's APK, from our code only (docs/RECOMPILATION_PLAN.md §7; CLAUDE.md §1: no game data in it): the
# native library (src/platform/android/ and the NDK's native_app_glue) for arm64-v8a, packed with the manifest by
# aapt2, aligned and signed with a debug key kept outside the repository.
#
#   tools/build_android.sh <out dir>        (needs $ANDROID_HOME, default /opt/android-sdk, with ndk;27.2.12479018,
#                                            platforms;android-34 and build-tools;34.0.0)
#
# Writes <out dir>/pkm-origins.apk. First form (7 November): the device check (AndroidDeviceCheck.c).
set -euo pipefail
out=$(mkdir -p "$1" && cd "$1" && pwd)
repo=$(cd "$(dirname "$0")/.." && pwd)
sdk=${ANDROID_HOME:-/opt/android-sdk}
ndk=$sdk/ndk/27.2.12479018
bt=$sdk/build-tools/34.0.0
cc=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
glue=$ndk/sources/android/native_app_glue
work=$out/apk_work
rm -rf "$work"; mkdir -p "$work/lib/arm64-v8a"
"$cc" -std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -fPIC -shared \
    -I "$glue" -o "$work/lib/arm64-v8a/libpkmorigins.so" \
    "$repo/src/platform/android/AndroidDeviceCheck.c" "$glue/android_native_app_glue.c" \
    -u ANativeActivity_onCreate -landroid -llog -lvulkan
"$bt/aapt2" link -o "$work/base.apk" --manifest "$repo/src/platform/android/AndroidManifest.xml" \
    -I "$sdk/platforms/android-34/android.jar"
(cd "$work" && zip -q -r base.apk lib)
"$bt/zipalign" -f -p 4 "$work/base.apk" "$work/aligned.apk"
key=$out/debug.keystore
[ -f "$key" ] || keytool -genkeypair -keystore "$key" -storepass android -keypass android -alias debug \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Pkm Origins debug" > /dev/null 2>&1
"$bt/apksigner" sign --ks "$key" --ks-pass pass:android --out "$out/pkm-origins.apk" "$work/aligned.apk"
"$bt/apksigner" verify "$out/pkm-origins.apk" && ls -l "$out/pkm-origins.apk"
