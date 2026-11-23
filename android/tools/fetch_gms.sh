#!/usr/bin/env bash
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Prepare vendor/gapps for the wsa_x86_64_gapps product.
#
# MindTheGapps (branch "vic", Android 15) provides the makefiles and the common GMS
# packages, but its x86_64 Play services (22.21) predate Android 15: Android 15's
# Google Services Framework expects Play services to own the gservices provider, so the
# old GmsCore crash-loops. This script replaces Play services, the Play Store and the
# Google Services Framework with the matching x86_64 builds from Google's official
# Android 15 emulator image (google_apis_playstore), together with their privileged
# permission allowlists. It also prepares vendor/ndk_translation, Google's ARM64
# translator, from the Android 16 emulator image (see step 4), and vendor/google_apps,
# the Google apps preinstalled on a stock phone (see step 5). Nothing downloaded here is
# redistributed by this repository.
#
# Usage: fetch_gms.sh <aosp_dir> [work_dir]
set -euo pipefail

AOSP=${1:?aosp dir}
WORK=${2:-$HOME/gapps}
MTG_URL=https://gitlab.com/MindTheGapps/vendor_gapps.git
EMU_ZIP=x86_64-35-ext15_r01.zip
NDK_ZIP=x86_64-36_r07.zip
APPS_ZIP=x86_64-37.0_r08.zip
# The browser set (Chrome, its Trichrome library and WebView, one version) comes from the newest
# image whose WebView still carries 32-bit x86 libraries next to x86_64 (see step 5).
BROWSER_ZIP=x86_64-36.1_r04.zip
APPS_SET=4  # bump when step 5 changes, so it runs again
IMG_URL=https://dl.google.com/android/repository/sys-img/google_apis_playstore
HOST=$AOSP/out/host/linux-x86/bin
log() { printf '[gms] %s\n' "$*" >&2; }

# extract_image <zip> <dir> <partition>...: download an emulator image and unpack the
# given partitions of its super partition to <dir>/x/<partition>; only the extracted
# trees are kept.
extract_image() {
    local zip=$1 dir=$2 start end p tool
    shift 2
    for tool in lpunpack fsck.erofs; do
        [[ -x $HOST/$tool ]] || { log "build $tool first: m lpunpack fsck.erofs"; exit 1; }
    done
    mkdir -p "$dir" && cd "$dir"
    [[ -f $zip || -f x86_64/system.img ]] || { log "downloading $zip"; curl -f -o "$zip" "$IMG_URL/$zip"; }
    [[ -f x86_64/system.img ]] || unzip -o -q "$zip" x86_64/system.img
    # The disk holds vbmeta and super partitions; take super by its GPT entry.
    read -r start end < <(sgdisk -i 2 x86_64/system.img |
        awk '/First sector/ {s=$3} /Last sector/ {e=$3} END {print s, e}')
    dd if=x86_64/system.img of=super.img bs=512 skip="$start" count=$((end - start + 1)) status=none
    rm -rf parts && mkdir parts
    "$HOST/lpunpack" $(printf -- '-p %s ' "$@") super.img parts/ >/dev/null
    for p in "$@"; do
        rm -rf "x/$p" && mkdir -p "x/$p"
        "$HOST/fsck.erofs" --extract="x/$p" "parts/$p.img" >/dev/null
    done
    rm -rf super.img parts x86_64 "$zip"
}

mkdir -p "$WORK"
G=$AOSP/vendor/gapps

# 1. MindTheGapps without the arm/arm64 trees.
if [[ ! -f $G/x86_64/x86_64-vendor.mk ]]; then
    log "cloning MindTheGapps (vic, x86_64 and common only)"
    rm -rf "$WORK/vendor_gapps"
    git clone -q --depth 1 --filter=blob:none --no-checkout -b vic "$MTG_URL" "$WORK/vendor_gapps"
    git -C "$WORK/vendor_gapps" sparse-checkout init --no-cone
    printf '/*\n!/arm/\n!/arm64/\n' > "$WORK/vendor_gapps/.git/info/sparse-checkout"
    git -C "$WORK/vendor_gapps" checkout -q vic
    rm -rf "$G"
    mv "$WORK/vendor_gapps" "$G"
fi

# 2. Official Android 15 emulator image: product and system_ext.
E=$WORK/emu/x
[[ -f $E/product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk ]] ||
    extract_image "$EMU_ZIP" "$WORK/emu" product system_ext

# 3. Matching Play services / Play Store / GSF and their allowlists.
cp "$E/product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk" \
   "$G/x86_64/proprietary/product/priv-app/GmsCore/GmsCore.apk"
cp "$E/product/priv-app/Phonesky/Phonesky.apk" \
   "$G/x86_64/proprietary/product/priv-app/Phonesky/Phonesky.apk"
cp "$E/system_ext/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk" \
   "$G/common/proprietary/system_ext/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk"
cp "$E/product/etc/permissions/privapp-permissions-google-p.xml" \
   "$G/common/proprietary/product/etc/permissions/privapp-permissions-google-product.xml"
cp "$E/system_ext/etc/permissions/privapp-permissions-google-se.xml" \
   "$G/common/proprietary/system_ext/etc/permissions/privapp-permissions-google-system-ext.xml"
cp "$E/product/etc/sysconfig/google-hiddenapi-package-whitelist.xml" \
   "$G/common/proprietary/product/etc/sysconfig/google-hiddenapi-package-allowlist.xml"
cp "$E/product/etc/sysconfig/google.xml" "$G/common/proprietary/product/etc/sysconfig/google.xml"
cp "$E/product/etc/sysconfig/google_build.xml" "$G/common/proprietary/product/etc/sysconfig/google_build.xml"
log "vendor/gapps ready (Play services, Play Store and GSF from the Android 15 emulator image)"

# 4. Google's ARM64 translator (ndk_translation), for vendor/ndk_translation.
#    WSA's Houdini (2023) cannot load many current ARM64 apps (BTI/PAC code, 16 KB pages,
#    packed libraries). The translator comes from the Android 16 image: the Android 15
#    images carry ndk_translation 0.2.3, which hands translated code the host JNIEnv, so
#    apps that replace JNI functions in it (Microsoft Edge replaces FindClass) crash the
#    runtime; the Android 16 build gives translated code its own JNIEnv. Its host-side
#    dependencies are all present in Android 15. The images carry only the 64-bit
#    translator, so 32-bit ARM apps keep using Houdini.
N=$AOSP/vendor/ndk_translation
if [[ $(cat "$N/source.txt" 2>/dev/null) != "$NDK_ZIP" ]]; then
    S=$WORK/ndk/x/system/system
    [[ -f $S/lib64/libndk_translation.so ]] || extract_image "$NDK_ZIP" "$WORK/ndk" system
    rm -rf "$N" && mkdir -p "$N/system/lib64" "$N/system/bin" "$N/system/etc/binfmt_misc"
    cp -a "$S"/lib64/libndk_translation*.so "$S/lib64/libberberis_exec_region.so" "$N/system/lib64/"
    cp -a "$S/lib64/arm64" "$N/system/lib64/"
    cp -a "$S/etc/cpuinfo.arm64.txt" "$S/etc/ld.config.arm64.txt" "$N/system/etc/"
    cp -a "$S/bin/arm64" "$S/bin/ndk_translation_program_runner_binfmt_misc_arm64" "$N/system/bin/"
    cp -a "$S/etc/binfmt_misc/arm64_exe" "$S/etc/binfmt_misc/arm64_dyn" "$N/system/etc/binfmt_misc/"
    grep -h '^ro.ndk_translation.version=' "$S/build.prop" > "$N/version.prop" || true
    # The image files carry 2009 timestamps; give them the current time so make sees a
    # translator change as newer than the files already installed in out/.
    find "$N" -exec touch -h {} +
    echo "$NDK_ZIP" > "$N/source.txt"
    rm -rf "$WORK/ndk"
fi
log "vendor/ndk_translation ready (from $NDK_ZIP, $(find "$N/system" -type f | wc -l) files)"

# 5. Google apps of a stock Android 15 phone that make sense on a PC, for vendor/google_apps.
#    Google ships them compressed behind stub APKs, which the package manager unpacks to /data
#    on first boot; on a PC disk the plain APK is smaller in total, so they are unpacked here.
#    Google Play updates them all.
#
#    Gmail, YouTube and the Google app come from the newest Google Play emulator image whose
#    build still runs on Android 15 (minSdkVersion <= 35), otherwise from the Android 15 image.
#
#    The browser set - Chrome, Google WebView and the Trichrome library both are built on - is
#    one version from one image (BROWSER_ZIP): WebView and Chrome load the Trichrome library of
#    exactly their own version. WSA runs 32-bit apps too (x86, and 32-bit ARM through Houdini),
#    which load WebView in their own process, so WebView needs x86 next to x86_64 libraries; from
#    API 37 on, the images carry a stand-alone 64-bit-only WebView. Google WebView replaces AOSP's
#    (overrides): AOSP's is never updated, Google's is kept current by Google Play.
A=$AOSP/vendor/google_apps
APPS="app/PrebuiltGmail app/YouTube priv-app/Velvet"
BROWSER="app/Chrome app/TrichromeLibrary app/WebViewGoogle"
min_sdk() {
    "$HOST/aapt2" dump badging "$1" 2>/dev/null | sed -nE "s/^(min)?[sS]dkVersion:'([0-9]+)'.*/\2/p" | head -1
}
badging() { "$HOST/aapt2" dump badging "$1" 2>/dev/null; }
if [[ $(cat "$A/source.txt" 2>/dev/null) != "$APPS_ZIP $BROWSER_ZIP $APPS_SET" ]]; then
    P=$WORK/apps/x/product
    [[ -d $P/app ]] || extract_image "$APPS_ZIP" "$WORK/apps" product
    B=$WORK/browser/x/product
    [[ -d $B/app ]] || extract_image "$BROWSER_ZIP" "$WORK/browser" product
    rm -rf "$A" && mkdir -p "$A"
    # The browser set: from its image only, checked before anything else is taken.
    for entry in $BROWSER; do
        name=${entry#*/} dst=$A/$entry/${entry#*/}.apk src=$B/$entry/${entry#*/}.apk
        mkdir -p "$A/$entry"
        if [[ -f $src.gz ]]; then gunzip -c "$src.gz" > "$dst"
        elif [[ -f $src ]]; then cp "$src" "$dst"
        else log "$name is not in $BROWSER_ZIP"; exit 1
        fi
        sdk=$(min_sdk "$dst")
        [[ -n $sdk && $sdk -le 35 ]] || { log "$name from $BROWSER_ZIP needs API ${sdk:-?}"; exit 1; }
        badging "$dst" | grep -q "alt-native-code: 'x86'" || [[ $name == Chrome ]] ||
            { log "$name from $BROWSER_ZIP has no 32-bit x86 libraries"; exit 1; }
        log "$name $(badging "$dst" | sed -nE "s/.*versionName='([^']*)'.*/\1/p" | head -1)"
    done
    trichrome=$(badging "$A/app/TrichromeLibrary/TrichromeLibrary.apk" | sed -nE "s/^static-library: .* version='([0-9]+)'.*/\1/p")
    for user in Chrome WebViewGoogle; do
        wants=$(badging "$A/app/$user/$user.apk" | sed -nE "s/^uses-static-library: name='com.google.android.trichromelibrary' version='([0-9]+)'.*/\1/p")
        [[ $wants == "$trichrome" ]] || { log "$user wants Trichrome $wants, the set has $trichrome"; exit 1; }
    done
    PRIV=()  # "package|image root" of the privileged apps
    for entry in $APPS; do
        name=${entry#*/} dst=$A/$entry/${entry#*/}.apk
        mkdir -p "$A/$entry"
        for root in "$P" "$E/product"; do
            src=$root/$entry/$name.apk
            if [[ -f $src.gz ]]; then gunzip -c "$src.gz" > "$dst"
            elif [[ -f $src ]]; then cp "$src" "$dst"
            else continue
            fi
            sdk=$(min_sdk "$dst")
            [[ -n $sdk && $sdk -le 35 ]] && break
            log "$name from $root needs API ${sdk:-?}, trying the next image"
            rm -f "$dst"
        done
        [[ -f $dst ]] || { log "no Android 15 build of $name found"; exit 1; }
        log "$name $("$HOST/aapt2" dump badging "$dst" | sed -nE "s/.*versionName='([^']*)'.*/\1/p" | head -1)"
        [[ $entry == priv-app/* ]] &&
            PRIV+=("$("$HOST/aapt2" dump badging "$dst" | sed -nE "s/^package: name='([^']*)'.*/\1/p")|$root")
    done
    # A privileged app from a newer image may hold privileged permissions that the Android 15
    # allowlist (privapp-permissions-google-p.xml) does not name, and Android 15 stops the
    # system server at boot for that. Its allowlist entries come from the image the app came from.
    mkdir -p "$A/etc/permissions"
    python3 - "$A/etc/permissions/privapp-permissions-google-apps.xml" "${PRIV[@]}" <<'PY'
import glob, os, sys
import xml.etree.ElementTree as ET
out, pairs = sys.argv[1], sys.argv[2:]
root = ET.Element("permissions")
for pair in pairs:
    package, image = pair.split("|", 1)
    merged = ET.SubElement(root, "privapp-permissions", package=package)
    seen = set()
    for path in sorted(glob.glob(os.path.join(image, "etc", "permissions", "*.xml"))):
        for block in ET.parse(path).getroot().iter("privapp-permissions"):
            if block.get("package") != package:
                continue
            for child in block:
                key = (child.tag, child.get("name"))
                if key not in seen:
                    seen.add(key)
                    ET.SubElement(merged, child.tag, name=child.get("name"))
    print("[gms] %s: %d allowlist entries" % (package, len(seen)), file=sys.stderr)
ET.indent(root)
comment = "<!-- Generated by tools/fetch_gms.sh from the images the privileged Google apps come from. -->"
with open(out, "w") as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>' + chr(10) + comment + chr(10))
    f.write(ET.tostring(root, encoding="unicode") + chr(10))
PY
    find "$A" -exec touch -h {} +
    {
        echo "// Generated by tools/fetch_gms.sh. Do not edit."
        echo
        echo "soong_namespace {}"
        for entry in $BROWSER $APPS; do
            # The Google app is a module of vendor/gapps; only its APK is replaced below.
            [[ $entry == priv-app/Velvet ]] && continue
            name=${entry#*/}
            echo
            echo "android_app_import {"
            echo "    name: \"$name\","
            echo "    apk: \"$entry/$name.apk\","
            echo "    presigned: true,"
            echo "    preprocessed: true,"
            echo "    dex_preopt: {"
            echo "        enabled: false,"
            echo "    },"
            # Google's WebView instead of AOSP's, which Google Play cannot update.
            [[ $name == WebViewGoogle ]] && echo "    overrides: [\"webview\"],"
            echo "    product_specific: true,"
            echo "}"
        done
    } > "$A/Android.bp"
    echo "$APPS_ZIP $BROWSER_ZIP $APPS_SET" > "$A/source.txt"
    rm -rf "$WORK/apps" "$WORK/browser"
fi
cp "$A/priv-app/Velvet/Velvet.apk" "$G/x86_64/proprietary/product/priv-app/Velvet/Velvet.apk"
log "vendor/google_apps ready ($BROWSER $APPS)"
