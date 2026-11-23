#!/usr/bin/env bash
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Extract WSA (Android 13) and AOSP GSI (Android 14) partition images into plain
# directory trees, then unpack every APEX payload, so the DEX tools can work on files.
#
# Usage: extract_images.sh <inputs_dir> <work_dir>
#   <inputs_dir>/wsa-2407-a13-pkg/{system,system_ext,product,vendor}.vhdx
#   <inputs_dir>/aosp<N>-gsi/*.zip         AOSP GSI zips (aosp14-gsi, aosp15-gsi, ...)
# Output:
#   <work_dir>/img/a<N>/*.img            raw ext4 images
#   <work_dir>/tree/a<N>/<part>/         extracted file trees
#   <work_dir>/tree/a<N>/apex/<name>/    extracted APEX payloads
set -euo pipefail

IN=${1:?inputs dir}
WORK=${2:?work dir}
mkdir -p "$WORK"/img/a13 "$WORK"/tree/a13

log() { printf '[extract] %s\n' "$*" >&2; }

vhdx_to_raw() {  # <src.vhdx> <dst.img>
    [[ -s $2 ]] && return 0
    log "convert $(basename "$1") -> $2"
    qemu-img convert -f vhdx -O raw "$1" "$2.tmp" && mv "$2.tmp" "$2"
}

rdump_tree() {  # <img> <dest_dir>
    [[ -d $2 ]] && return 0
    log "rdump $(basename "$1") -> $2"
    mkdir -p "$2.tmp"
    debugfs -R "rdump / $2.tmp" "$1" 2>/dev/null
    mv "$2.tmp" "$2"
}

# Android 13 (WSA 2407.40000.4.0)
for p in system system_ext product vendor; do
    vhdx_to_raw "$IN/wsa-2407-a13-pkg/$p.vhdx" "$WORK/img/a13/$p.img"
    rdump_tree "$WORK/img/a13/$p.img" "$WORK/tree/a13/$p"
done

# Newer Android versions (AOSP GSI, system only): one aosp<N>-gsi/ directory per version
GSI_VERS=()
for d in "$IN"/aosp*-gsi; do
    zip=$(ls "$d"/*.zip 2>/dev/null | head -n1)
    [[ -n $zip ]] || continue
    ver=$(basename "$d"); ver=${ver#aosp}; ver=a${ver%-gsi}
    GSI_VERS+=("$ver")
    mkdir -p "$WORK/img/$ver" "$WORK/tree/$ver"
    if [[ ! -s $WORK/img/$ver/system.img ]]; then
        log "unzip $(basename "$zip") system.img -> $ver"
        unzip -o -q "$zip" system.img -d "$WORK/img/$ver"
    fi
    rdump_tree "$WORK/img/$ver/system.img" "$WORK/tree/$ver/system"
done

# APEX payloads: <tree>/system/system/apex/*.apex|*.capex (system-as-root layout)
unpack_apexes() {  # <ver>
    local ver=$1 sysroot apexdir out f name tmp
    sysroot="$WORK/tree/$ver/system"
    [[ -d $sysroot/system ]] && sysroot="$sysroot/system"
    apexdir="$sysroot/apex"
    out="$WORK/tree/$ver/apex"
    mkdir -p "$out"
    for f in "$apexdir"/*.apex "$apexdir"/*.capex; do
        [[ -e $f ]] || continue
        name=$(basename "$f"); name=${name%.apex}; name=${name%.capex}
        [[ -d $out/$name ]] && continue
        tmp=$(mktemp -d)
        if [[ $f == *.capex ]]; then
            unzip -q -o "$f" original_apex -d "$tmp"
            unzip -q -o "$tmp/original_apex" apex_payload.img -d "$tmp"
        else
            unzip -q -o "$f" apex_payload.img -d "$tmp" 2>/dev/null || { rm -rf "$tmp"; continue; }
        fi
        if file "$tmp/apex_payload.img" | grep -q ext[234]; then
            mkdir -p "$out/$name"
            debugfs -R "rdump / $out/$name" "$tmp/apex_payload.img" 2>/dev/null
        else
            log "WARN $name: payload is not ext4 ($(file -b "$tmp/apex_payload.img" | cut -c1-40))"
        fi
        rm -rf "$tmp"
    done
    log "$ver: $(ls "$out" | wc -l) APEX payloads unpacked"
}
unpack_apexes a13
for v in "${GSI_VERS[@]}"; do unpack_apexes "$v"; done
log done
