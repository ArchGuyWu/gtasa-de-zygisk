#!/usr/bin/env bash
set -euo pipefail

HOST_WORK=/data/data/com.termux/files/home/Projects/mod-workspace

if [[ -e /system/bin/linker64 && ! -f /etc/debian_version ]]; then
    exec proot-distro login ubuntu-build --bind "${HOST_WORK}:/work" -- bash /work/gtasa-de-zygisk/build.sh
fi

ROOT=$(cd "$(dirname "$0")" && pwd)
SYSROOT=/opt/toolchain/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/sysroot
NDK_ZIP=/opt/toolchain/android-ndk-r30-linux.zip
NDK_URL=https://dl.google.com/android/repository/android-ndk-r30-linux.zip
NDK_SHA1=5107f898313790e449e87eee2183d9a20602dee9
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

export DEBIAN_FRONTEND=noninteractive

as_root() {
    if [[ "$(id -u)" -eq 0 ]]; then
        "$@"
    else
        sudo "$@"
    fi
}

apt_update() {
    as_root apt-get update || [[ $? -eq 100 ]]
}

if ! command -v clang++-22 >/dev/null 2>&1 || ! command -v llvm-readelf-22 >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source /etc/os-release
    apt_update
    as_root apt-get install -y ca-certificates curl gnupg unzip zip python3
    as_root install -d /etc/apt/keyrings
    as_root curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/apt.llvm.org.asc
    echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-22 main" \
        | as_root tee /etc/apt/sources.list.d/llvm-22.list >/dev/null
    apt_update
    as_root apt-get install -y clang-22 lld-22 llvm-22
fi

if ! command -v unzip >/dev/null 2>&1 || ! command -v zip >/dev/null 2>&1; then
    apt_update
    as_root apt-get install -y unzip zip
fi

if [[ ! -f "${SYSROOT}/usr/lib/aarch64-linux-android/30/liblog.so" ]]; then
    as_root install -d /opt/toolchain
    if [[ ! -f "$NDK_ZIP" ]] || ! echo "${NDK_SHA1}  ${NDK_ZIP}" | sha1sum -c -; then
        as_root curl -fL --retry 3 -o "${NDK_ZIP}.partial" "$NDK_URL"
        as_root mv "${NDK_ZIP}.partial" "$NDK_ZIP"
        echo "${NDK_SHA1}  ${NDK_ZIP}" | sha1sum -c -
    fi
    as_root rm -rf /opt/toolchain/android-ndk-r30
    as_root unzip -q "$NDK_ZIP" -d /opt/toolchain
fi

OUT="${STAGE}/zygisk/arm64-v8a.so"
CRT="${SYSROOT}/usr/lib/aarch64-linux-android/30"
BUILTINS=/opt/toolchain/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a
install -d "${STAGE}/zygisk"
clang++-22 --target=aarch64-linux-android30 \
    --sysroot="$SYSROOT" \
    -fuse-ld=lld-22 \
    -std=c++17 \
    -shared -fPIC -fvisibility=hidden \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-c++-static-destructors \
    -nostdlib \
    -O2 -Wall -Wextra \
    -Wl,-z,max-page-size=16384 -Wl,-z,defs \
    -I "${ROOT}/include" -I "${ROOT}/src" \
    "${CRT}/crtbegin_so.o" \
    "${ROOT}/src/main.cpp" "${ROOT}/src/library_watch.cpp" \
    "${ROOT}/src/plugins.cpp" "${ROOT}/src/hook.cpp" \
    -llog -ldl -lc \
    "${BUILTINS}" \
    "${CRT}/crtend_so.o" \
    -o "$OUT"

install -d "${STAGE}/plugins"
compile_plugin() {
    clang++-22 --target=aarch64-linux-android30 \
        --sysroot="$SYSROOT" \
        -fuse-ld=lld-22 \
        -std=c++17 \
        -shared -fPIC -fvisibility=hidden \
        -fno-exceptions -fno-rtti \
        -nostdlib \
        -O2 -Wall -Wextra \
        -Wl,-z,max-page-size=16384 -Wl,-z,defs \
        "${CRT}/crtbegin_so.o" \
        "$1" \
        -llog -lc \
        "${BUILTINS}" \
        "${CRT}/crtend_so.o" \
        -o "$2"
}
compile_plugin "${ROOT}/plugins/probe.cpp" "${STAGE}/plugins/gtasa_probe.so"
compile_plugin "${ROOT}/plugins/fixes.cpp" "${STAGE}/plugins/gtasa_fixes.so"
compile_plugin "${ROOT}/plugins/cop_react.cpp" "${STAGE}/plugins/gtasa_cop_react.so"

echo "=== LOAD ==="
llvm-readelf-22 -l "$OUT"
echo "=== DYNAMIC ==="
llvm-readelf-22 -d "$OUT"
echo "=== SYMBOL ==="
llvm-readelf-22 -s "$OUT" | grep zygisk_module_entry

python3 - <<PY
import subprocess, sys
out = "${OUT}"
text = subprocess.check_output(["llvm-readelf-22", "-l", out], text=True)
aligns = []
for line in text.splitlines():
    parts = line.split()
    if parts and parts[0] == "LOAD":
        aligns.append(parts[-1])
if not aligns or any(item != "0x4000" for item in aligns):
    sys.exit("LOAD Align is %s" % aligns)
dyn = subprocess.check_output(["llvm-readelf-22", "-d", out], text=True)
needed = []
for line in dyn.splitlines():
    if "(NEEDED)" in line:
        needed.append(line.split("[", 1)[1].split("]", 1)[0])
allowed = {"libc.so", "liblog.so", "libdl.so", "libm.so", "libstdc++.so"}
bad = [name for name in needed if name not in allowed]
if bad:
    sys.exit("unexpected NEEDED: %s" % bad)
syms = subprocess.check_output(["llvm-readelf-22", "-s", out], text=True)
if "zygisk_module_entry" not in syms:
    sys.exit("missing zygisk_module_entry")
print("needed", needed)
PY

cp "${ROOT}/module.prop" "${STAGE}/module.prop"
VERSION="$(sed -n 's/^version=//p' "${ROOT}/module.prop")"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "module.prop version must be MAJOR.MINOR.PATCH, got '${VERSION}'" >&2
    exit 1
fi
DIST="${ROOT}/dist"
install -d "$DIST"
ZIP="${DIST}/gtasa-de-zygisk-${VERSION}-arm64.zip"
rm -f "$ZIP"
(
    cd "$STAGE"
    zip -X -r "$ZIP" module.prop zygisk/arm64-v8a.so plugins/gtasa_probe.so plugins/gtasa_fixes.so plugins/gtasa_cop_react.so
)
unzip -l "$ZIP"
echo "zip ${ZIP}"

NPATCH="${STAGE}/npatch/libgtasa_npatch.so"
install -d "${STAGE}/npatch"
clang++-22 --target=aarch64-linux-android30 \
    --sysroot="$SYSROOT" \
    -fuse-ld=lld-22 \
    -std=c++17 \
    -shared -fPIC -fvisibility=hidden \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-c++-static-destructors \
    -nostdlib \
    -O2 -Wall -Wextra \
    -Wl,-z,max-page-size=16384 -Wl,-z,defs \
    -I "${ROOT}/src" \
    "${CRT}/crtbegin_so.o" \
    "${ROOT}/src/npatch_main.cpp" "${ROOT}/src/library_watch.cpp" \
    "${ROOT}/src/plugins.cpp" "${ROOT}/src/hook.cpp" \
    -llog -ldl -lc \
    "${BUILTINS}" \
    "${CRT}/crtend_so.o" \
    -o "$NPATCH"
llvm-readelf-22 -s "$NPATCH" | grep -w native_init
install -d "${STAGE}/npatch-libs"
cp "$NPATCH" "${STAGE}/npatch-libs/libgtasa_npatch.so"
cp "${STAGE}/plugins/gtasa_cop_react.so" "${STAGE}/npatch-libs/libgtasa_cop_react.so"
cp "${STAGE}/plugins/gtasa_fixes.so" "${STAGE}/npatch-libs/libgtasa_fixes.so"
cp "${STAGE}/plugins/gtasa_probe.so" "${STAGE}/npatch-libs/libgtasa_probe.so"
APK="${DIST}/gtasa-de-zygisk-${VERSION}-npatch.apk"
VERSION_CODE="$(sed -n 's/^versionCode=//p' "${ROOT}/module.prop")"
python3 "${ROOT}/pack_npatch.py" \
    --version-name "$VERSION" \
    --version-code "$VERSION_CODE" \
    --lib-dir "${STAGE}/npatch-libs" \
    --out "$APK"
if command -v jarsigner >/dev/null 2>&1 && command -v keytool >/dev/null 2>&1; then
    KEYSTORE="${STAGE}/debug.keystore"
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -dname CN=Android -keyalg RSA -keysize 2048 -validity 10000
    jarsigner -sigalg SHA256withRSA -digestalg SHA-256 -keystore "$KEYSTORE" \
        -storepass android "$APK" androiddebugkey
fi
unzip -l "$APK"
echo "apk ${APK}"
