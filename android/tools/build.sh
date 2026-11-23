#!/usr/bin/env bash
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Build Android system images for WSA and convert them to VHDX.
#
# Usage: build.sh <aosp_dir> [vendor_wsa_dir] [variant] [targets...]
#   aosp_dir        AOSP checkout (android-15.0.0_r36)
#   vendor_wsa_dir  optional private WSA integration layer, copied to vendor/wsa
#   variant         userdebug (default) or user
#   targets         make targets (default: systemimage systemextimage productimage)
# Environment:
#   WSA_PRODUCT     lunch product (default wsa_x86_64; wsa_x86_64_gapps needs vendor/gapps)
#   BUILD_NUMBER    build number in the fingerprint (default: the build's date and time, UTC)
set -euo pipefail

# A new fingerprint for every build. Android treats an installation over a different fingerprint
# as a system update (OTA): default permissions and roles are granted again and packages are
# scanned again. AOSP's default, "eng.<user>", is the same for every build, so an update of the
# images kept the permissions of the first installation.
export BUILD_NUMBER=${BUILD_NUMBER:-$(date -u +%Y%m%d%H%M)}

AOSP=${1:?aosp dir}
VENDOR_WSA=${2:-}
VARIANT=${3:-userdebug}
shift $(( $# < 3 ? $# : 3 ))
TARGETS=("$@")
[[ ${#TARGETS[@]} -gt 0 ]] || TARGETS=(systemimage systemextimage productimage)

REPO=$(cd "$(dirname "$0")/.." && pwd)
log() { printf '[build] %s\n' "$*" >&2; }

# Device tree (public) and integration layer (private) into the AOSP tree.
mkdir -p "$AOSP/device/wsa"
rsync -a --delete "$REPO/device/wsa/" "$AOSP/device/wsa/"
if [[ -n $VENDOR_WSA ]]; then
    mkdir -p "$AOSP/vendor/wsa"
    rsync -a --delete "$VENDOR_WSA/" "$AOSP/vendor/wsa/"
    log "vendor/wsa synced from $VENDOR_WSA"
else
    rm -rf "$AOSP/vendor/wsa"
    log "no vendor/wsa: plain Android 15 build"
fi

# Source patches: patches/<aosp project path>/*.patch, applied in name order.
while IFS= read -r -d '' patch; do
    rel=${patch#"$REPO/patches/"}
    project=$(dirname "$rel")
    if git -C "$AOSP/$project" apply --reverse --check "$patch" 2>/dev/null; then
        continue  # already applied
    fi
    git -C "$AOSP/$project" apply "$patch"
    log "applied $rel"
done < <(find "$REPO/patches" -name '*.patch' -print0 2>/dev/null | sort -z)

cd "$AOSP"
set +u
# shellcheck disable=SC1091
source build/envsetup.sh >/dev/null
PRODUCT=${WSA_PRODUCT:-wsa_x86_64}
if [[ $PRODUCT == *_gapps && ! -d vendor/gapps ]]; then
    log "vendor/gapps is missing (MindTheGapps, branch vic)"
    exit 1
fi
lunch "$PRODUCT-bp1a-$VARIANT"
set -u
log "build number $BUILD_NUMBER"

start=$(date +%s)
m "${TARGETS[@]}"
log "build finished in $(( ($(date +%s) - start) / 60 )) min"

OUT=$(get_build_var PRODUCT_OUT)
for img in system system_ext product; do
    [[ -f $OUT/$img.img ]] || continue
    # Dynamic VHDX: the file holds only the written blocks, so it is the size of the image.
    # qemu-img's fixed VHDX is twice that size (with block_state_zero=on every payload block
    # starts in the "zero" state and each write appends another one), and with
    # block_state_zero=off Windows refuses to attach it. Hyper-V reads both subformats alike;
    # WSA's own user data disk is dynamic too.
    qemu-img convert -f raw -O vhdx -o subformat=dynamic "$OUT/$img.img" "$OUT/$img.vhdx"
    log "$img.vhdx $(du -h --apparent-size "$OUT/$img.vhdx" | cut -f1) (from $(du -h --apparent-size "$OUT/$img.img" | cut -f1) raw)"
done
