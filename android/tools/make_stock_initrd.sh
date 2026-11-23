#!/usr/bin/env bash
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Rebuild Microsoft's original WSA initrd (a single /init) from a WSABuilds/MagiskOnWSA
# initrd, where the original init is kept as /wsainit next to Magisk's loaders.
# Usage: make_stock_initrd.sh <initrd.img in> <initrd.img out>
set -euo pipefail
IN=${1:?input initrd}; OUT=${2:?output initrd}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/x" "$TMP/root"
(cd "$TMP/x" && cpio -idm --quiet < "$(realpath "$IN")")
if [[ -f $TMP/x/wsainit ]]; then
    cp "$TMP/x/wsainit" "$TMP/root/init"      # patched initrd: original init was renamed
else
    cp "$TMP/x/init" "$TMP/root/init"         # already a stock initrd
fi
chmod 0750 "$TMP/root/init"
(cd "$TMP/root" && find . | LC_ALL=C sort | cpio -o -H newc --quiet --owner=0:0) > "$OUT"
sha256sum "$TMP/root/init" "$OUT"
