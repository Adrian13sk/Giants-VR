#!/usr/bin/env bash
# Downloads the build toolchain into $GVR_TOOLCHAINS (default: ../toolchains next to the repo)
# and verifies SHA-256 checksums. Only GitHub-hosted artefacts are used because Google's
# dl.google.com / maven.google.com are not reachable from the original build environment.
#
# Provenance (see docs/DEPENDENCIES.md):
#  - NDK r27c (27.2.12479018) rebuilt from AOSP sources by HomuHomu833/android-ndk-custom
#    (x86_64 musl host). Swap for Google's official NDK r27c when available - the build
#    scripts only need $GVR_NDK to point at an NDK root.
#  - aapt2 + android framework resources, extracted from Apktool 2.12.1 (Apache-2.0).
#  - apksig + zipalign bundled in uber-apk-signer 1.3.0 (Apache-2.0).
#  - Khronos OpenXR loader for Android 1.1.53 (Apache-2.0).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
TC="${GVR_TOOLCHAINS:-$here/../toolchains}"
mkdir -p "$TC"
cd "$TC"

fetch() {  # url file sha256
  local url="$1" file="$2" sha="$3"
  if [[ ! -f "$file" ]] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
    echo "downloading $file"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  echo "$sha  $file" | sha256sum -c -
}

fetch https://github.com/HomuHomu833/android-ndk-custom/releases/download/r27/android-ndk-r27c-x86_64-linux-musl.tar.xz \
      ndk-r27c.tar.xz e369ee4df37a0c5653d59ee9582bf66d23170909d20ecfed30b3c1d84356c6d0
fetch https://github.com/iBotPeaches/Apktool/releases/download/v2.12.1/apktool_2.12.1.jar \
      apktool_2.12.1.jar 66cf4524a4a45a7f56567d08b2c9b6ec237bcdd78cee69fd4a59c8a0243aeafa
fetch https://github.com/patrickfav/uber-apk-signer/releases/download/v1.3.0/uber-apk-signer-1.3.0.jar \
      uber-apk-signer-1.3.0.jar e1299fd6fcf4da527dd53735b56127e8ea922a321128123b9c32d619bba1d835
mkdir -p openxr-loader
fetch https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.53/openxr_loader_for_android-1.1.53.aar \
      openxr-loader/openxr_loader_for_android-1.1.53.aar 8495a9cb5f792d53effbee25825557227dee2192f15455fbd2214037d0d3b97c

[[ -d ndk/android-ndk-r27c ]] || { mkdir -p ndk && tar -xJf ndk-r27c.tar.xz -C ndk; }
[[ -f openxr-loader/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so ]] || \
  (cd openxr-loader && unzip -o -q openxr_loader_for_android-1.1.53.aar)
mkdir -p sdk
if [[ ! -x sdk/aapt2 ]]; then
  (cd sdk && unzip -o -q ../apktool_2.12.1.jar prebuilt/linux/aapt2_64 prebuilt/android-framework.jar &&
   mv prebuilt/linux/aapt2_64 aapt2 && mv prebuilt/android-framework.jar android-framework.jar &&
   rm -rf prebuilt && chmod +x aapt2)
fi
echo "toolchain ready in $TC"
