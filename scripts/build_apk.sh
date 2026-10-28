#!/usr/bin/env bash
# Builds the Quest APK: native library (NDK/CMake) -> aapt2 manifest link -> add native libs
# (stored, page-aligned) -> zipalign + APK Signature Scheme v2/v3 signing -> verification.
# Output: build/apk/GiantsVR-dev.apk
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
TC="${GVR_TOOLCHAINS:-$here/../toolchains}"
NDK="${GVR_NDK:-$TC/ndk/android-ndk-r27c}"
AAPT2="$TC/sdk/aapt2"
FRAMEWORK="$TC/sdk/android-framework.jar"
SIGNER="$TC/uber-apk-signer-1.3.0.jar"
LOADER="$TC/openxr-loader/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so"
KEYSTORE="${GVR_KEYSTORE:-$here/keys/giantsvr-dev.keystore}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
OUT="$here/build/apk"
for f in "$NDK/build/cmake/android.toolchain.cmake" "$AAPT2" "$FRAMEWORK" "$SIGNER" "$LOADER"; do
  [[ -e "$f" ]] || { echo "missing: $f (run scripts/fetch_deps.sh)"; exit 1; }
done

# --check: the host unit tests first (2026-10-28).
if [[ "${1:-}" == "--check" ]]; then
  echo "== host unit tests"
  cmake -S "$here" -B "$here/build/host" -G Ninja >/dev/null && ninja -C "$here/build/host" gvr_tests >/dev/null
  "$here/build/host/gvr_tests" | tail -1
fi

echo "== native build ($BUILD_TYPE)"
cmake -S "$here" -B "$here/build/android" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DOPENXR_LOADER_SO="$LOADER" >/dev/null
ninja -C "$here/build/android" giantsvr

echo "== package"
rm -rf "$OUT" && mkdir -p "$OUT/stage/lib/arm64-v8a"
"$AAPT2" link -o "$OUT/base.apk" --manifest "$here/android/AndroidManifest.xml" -I "$FRAMEWORK" \
  --min-sdk-version 29 --target-sdk-version 32 --version-code 1 --version-name 0.1.0-dev
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
cp "$here/build/android/libgiantsvr.so" "$OUT/stage/lib/arm64-v8a/"
cp "$here/build/android/libgiantsvr.so" "$OUT/libgiantsvr.unstripped.so"   # for ndk-stack symbolication
# (2026-10-28) ...and kept per commit: a crash report from a headset names this build's offsets
# (tools/py/symbolize_crash.py <report> build/symbols/<commit>.so).
mkdir -p "$here/build/symbols"
rev="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)"
git -C "$here" diff --quiet HEAD -- src 2>/dev/null || rev="$rev-dirty"
cp "$here/build/android/libgiantsvr.so" "$here/build/symbols/$rev.so"
"$STRIP" --strip-unneeded "$OUT/stage/lib/arm64-v8a/libgiantsvr.so"
cp "$LOADER" "$OUT/stage/lib/arm64-v8a/"
python3 -I - "$OUT/base.apk" "$OUT/unsigned.apk" "$OUT/stage" <<'PY'
import os, sys, zipfile
src, dst, stage = sys.argv[1:4]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
    for item in zin.infolist():
        zout.writestr(item, zin.read(item.filename))
    for root, _, files in os.walk(stage):
        for f in sorted(files):
            full = os.path.join(root, f)
            arc = os.path.relpath(full, stage)
            zi = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED  # uncompressed so the loader can mmap it
            with open(full, "rb") as fh:
                zout.writestr(zi, fh.read())
PY

if [[ ! -f "$KEYSTORE" ]]; then
  echo "== generating development keystore $KEYSTORE"
  mkdir -p "$(dirname "$KEYSTORE")"
  keytool -genkeypair -noprompt -keystore "$KEYSTORE" -storepass giantsvr -keypass giantsvr \
    -alias giantsvr -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=GiantsVR unofficial dev build" 2>/dev/null
fi
echo "== zipalign + sign"
java -jar "$SIGNER" -a "$OUT/unsigned.apk" -o "$OUT/signed" \
  --ks "$KEYSTORE" --ksAlias giantsvr --ksPass giantsvr --ksKeyPass giantsvr \
  --allowResign 2>&1 | grep -vE "^Picked up JAVA_TOOL_OPTIONS" | tail -25
mv "$OUT"/signed/*.apk "$OUT/GiantsVR-dev.apk"
rm -rf "$OUT/signed" "$OUT/stage" "$OUT/base.apk" "$OUT/unsigned.apk"
ls -la "$OUT/GiantsVR-dev.apk"
sha256sum "$OUT/GiantsVR-dev.apk"
