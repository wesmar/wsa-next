#!/usr/bin/env bash
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Package a WSA-Next release: every component as one zstd stream plus manifest.json, the file the
# installer (WSA-Next.exe) reads. See src/core/Manifest.h for the format.
#
# Usage: package-release.sh --images <dir> --wsa <dir> --initrd <file> --out <dir> --version <v>
#                           [--kernel <file>] [--android <text>] [--variant <text>] [--base-url <url>] [--wsapatch <file>]
#   --images    folder with system.vhdx, system_ext.vhdx and product.vhdx (tools/build.sh output)
#   --wsa       unpacked WSA 2407 package (AppxManifest.xml, WsaClient\, vendor.vhdx, Tools\kernel ...)
#   --initrd    the initrd to boot with (tools/make_stock_initrd.sh)
#   --kernel    optional replacement kernel bzImage (defaults to <wsa>/Tools/kernel)
#   --out       output folder (manifest.json and the .zst files)
#   --base-url  prefix for the component URLs in the manifest (default: relative to the manifest)
#
# Unchanged components keep their file: a .zst whose source has not changed since the last run
# (same size and SHA-256 recorded in <out>/.sources) is not compressed again.
set -euo pipefail

IMAGES= WSA= INITRD= KERNEL= OUT= VERSION= ANDROID="Android 15" VARIANT="Google Play" BASE_URL= WSAPATCH=
while [[ $# -gt 0 ]]; do
    case $1 in
        --images) IMAGES=$2; shift 2 ;;
        --wsa) WSA=$2; shift 2 ;;
        --initrd) INITRD=$2; shift 2 ;;
        --kernel) KERNEL=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --version) VERSION=$2; shift 2 ;;
        --android) ANDROID=$2; shift 2 ;;
        --variant) VARIANT=$2; shift 2 ;;
        --base-url) BASE_URL=$2; shift 2 ;;
        --wsapatch) WSAPATCH=$2; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
for v in IMAGES WSA INITRD OUT VERSION; do
    [[ -n ${!v} ]] || { echo "--${v,,} is required" >&2; exit 2; }
done
for tool in zstd tar sha256sum; do
    command -v $tool >/dev/null || { echo "$tool is required" >&2; exit 1; }
done
log() { printf '[package] %s\n' "$*" >&2; }

mkdir -p "$OUT"
SOURCES=$OUT/.sources
touch "$SOURCES"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
COMPONENTS=()

# component <id> <kind> <target> <source file> [optional]
component() {
    local id=$1 kind=$2 target=$3 src=$4 optional=${5:-false}
    local zst="${target##*/}.zst"  # system.vhdx.zst, kernel.zst, initrd.img.zst
    [[ $kind == archive ]] && zst="$id.tar.zst"
    local size sum stamp
    size=$(stat -c %s "$src")
    sum=$(sha256sum "$src" | cut -d' ' -f1)
    stamp="$id $size $sum"
    if grep -qxF "$stamp" "$SOURCES" && [[ -f $OUT/$zst ]]; then
        log "$id: unchanged"
    else
        log "$id: compressing $(numfmt --to=iec "$size")"
        # A 2 GB window (--long=31, which the installer's decoder allows) for the best ratio.
        # Every worker holds its own job buffers, so four workers keep the memory of a
        # multi-gigabyte image well inside a 16 GB WSL; the ratio hardly depends on their number.
        zstd -q -19 --long=31 -T4 -f "$src" -o "$OUT/$zst.new"
        mv -f "$OUT/$zst.new" "$OUT/$zst"
        grep -v "^$id " "$SOURCES" > "$SOURCES.new" || true
        echo "$stamp" >> "$SOURCES.new"
        mv -f "$SOURCES.new" "$SOURCES"
    fi
    local packed packedSum
    packed=$(stat -c %s "$OUT/$zst")
    packedSum=$(sha256sum "$OUT/$zst" | cut -d' ' -f1)
    log "$id: $(numfmt --to=iec "$size") -> $(numfmt --to=iec "$packed")"
    # "sha256": the unpacked file as installed (disks and files). The manifest is signed, so the
    # installed Android images can be checked against the official release.
    COMPONENTS+=("$(printf '    {\n      "id": "%s",\n      "kind": "%s",\n      "target": "%s",\n      "url": "%s",\n      "packedSize": %s,\n      "packedSha256": "%s",\n      "size": %s,%s\n      "version": "%s"%s\n    }' \
        "$id" "$kind" "$target" "$BASE_URL$zst" "$packed" "$packedSum" "$size" \
        "$([[ $kind != archive ]] && printf '\n      "sha256": "%s",' "$sum")" "$VERSION" \
        "$([[ $optional == true ]] && printf ',\n      "optional": true')")")
}

# The Windows part: the package without the Android disks, the boot files (own components) and
# the scripts of the tools it was unpacked with. The tar is reproducible (sorted, fixed owner).
STAGE=$WORK/windows
mkdir -p "$STAGE"
(cd "$WSA" && find . -type f \
    ! -name '*.vhdx' ! -path './Tools/*' ! -name 'Install.ps1' ! -name 'Run.bat' ! -name 'MakePri.ps1' \
    ! -name 'makepri.exe' ! -name 'filelist.txt' ! -name 'upgrade-magisk.md' ! -name 'WSAPatch.exe' \
    ! -path './pri/*' ! -path './xml/*' -print0) |
    (cd "$WSA" && xargs -0 cp --parents -t "$STAGE")
[[ -f $STAGE/AppxManifest.xml ]] || { echo "no AppxManifest.xml in $WSA" >&2; exit 1; }
tar --format=pax --sort=name --owner=0 --group=0 --numeric-owner --mtime='2024-07-01 00:00Z' \
    --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime -C "$STAGE" -cf "$WORK/windows.tar" .
# Metadata and the empty user data disk are part of Microsoft's package.
for disk in metadata userdata; do
    [[ -f $WSA/$disk.vhdx ]] && cp "$WSA/$disk.vhdx" "$WORK/$disk.vhdx"
done

component system disk system.vhdx "$IMAGES/system.vhdx"
component system_ext disk system_ext.vhdx "$IMAGES/system_ext.vhdx"
component product disk product.vhdx "$IMAGES/product.vhdx"
component vendor disk vendor.vhdx "$WSA/vendor.vhdx"
component kernel file Tools/kernel "${KERNEL:-$WSA/Tools/kernel}"
component initrd file Tools/initrd.img "$INITRD"
component windows archive . "$WORK/windows.tar"
[[ -f $WORK/metadata.vhdx ]] && component metadata disk metadata.vhdx "$WORK/metadata.vhdx"
[[ -f $WORK/userdata.vhdx ]] && component userdata disk userdata.vhdx "$WORK/userdata.vhdx"
[[ -n $WSAPATCH ]] && component wsapatch file WSAPatch.exe "$WSAPATCH" true

{
    printf '{\n  "format": 1,\n  "name": "WSA-Next",\n  "version": "%s",\n  "android": "%s",\n  "variant": "%s",\n  "minWindowsBuild": 22000,\n  "components": [\n' \
        "$VERSION" "$ANDROID" "$VARIANT"
    for i in "${!COMPONENTS[@]}"; do
        printf '%s' "${COMPONENTS[$i]}"
        [[ $i -lt $((${#COMPONENTS[@]} - 1)) ]] && printf ','
        printf '\n'
    done
    printf '  ]\n}\n'
} > "$OUT/manifest.json.new"
mv -f "$OUT/manifest.json.new" "$OUT/manifest.json"
log "manifest.json: release $VERSION, ${#COMPONENTS[@]} components, $(du -ch "$OUT"/*.zst | tail -1 | cut -f1) to download"
