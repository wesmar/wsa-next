#!/usr/bin/env bash
# WSA-Next © 2026 Marek Wesołowski - wesmar
# Integrate the Google apps collected from Google Play (tools/collect-play-apps.ps1) into the
# Google Play variant's images, so a new installation starts with this month's versions instead
# of downloading them in its first minutes.
#
# Usage: integrate-play-apps.sh <aosp_dir> <collected_dir>
#
# <collected_dir> holds play-apps.json and one folder per app with its APKs (base and splits)
# and extracted native libraries, exactly as Android installed them from Google Play. They are
# installed unchanged (prebuilt_root: copied, never re-signed or re-aligned) to the folder the
# image's version used (e.g. /product/priv-app/GmsCore), and the image's version is left out
# (overrides). Google Play keeps updating them, since the signature is Google's own.
#
# Checked here: the size and SHA-256 of every file, the signing certificate (the same as the
# image's version), exactly one base APK per app, and that Chrome and WebView load the Trichrome
# library the image carries. Privileged permissions the new versions hold and the image's
# allowlist does not name (found on the device by collect-play-apps.ps1) are allowlisted, since
# Android 15 stops the system server at boot for any that are missing.
#
# Output: vendor/google_apps/play (Android.bp, play.mk, the files), included by
# wsa_x86_64_gapps.mk when present. An unchanged input leaves it untouched.
set -euo pipefail
AOSP=$(readlink -f "$1")
SRC=$(readlink -f "$2")
D=$AOSP/vendor/google_apps/play
HOST=$AOSP/out/host/linux-x86/bin
log() { echo "[play] $*" >&2; }

# The build's staging folders (out/target/product/<device>/<partition>) keep the files of every
# module ever installed: Make does not remove what an overridden module installed in an earlier
# build. Left there, the image's old APK sits next to the new base.apk - two base APKs in one
# folder, which the package manager rejects, so the app would be missing. Every folder an
# integrated app owns is therefore reduced to exactly its files, and the folder of a module left
# out (dropped.txt) is removed, on every run.
clean_staging() {
    local out folder file
    [[ -f $D/expected.txt ]] || return 0
    for out in "$AOSP"/out/target/product/*/; do
        while read -r folder; do
            [[ -n $folder && -d $out$folder ]] && { rm -rf "${out:?}$folder"; log "removed $folder (left out)"; }
        done < "$D/dropped.txt"
        while read -r folder; do
            [[ -d $out$folder ]] || continue
            while IFS= read -r -d '' file; do
                grep -qxF "${file#"$out"}" "$D/expected.txt" && continue
                rm -f "$file"
                log "removed ${file#"$out"} (not part of the integrated app)"
            done < <(find "$out$folder" -type f -print0)
        done < "$D/folders.txt"
    done
}

[[ -f $SRC/play-apps.json ]] || { log "no play-apps.json in $SRC"; exit 1; }
[[ -x $HOST/aapt2 && -x $HOST/zipalign ]] || { log "build the host tools first: m aapt2 zipalign"; exit 1; }
stamp=$(sha256sum "$SRC/play-apps.json" | cut -d' ' -f1)
if [[ $(cat "$D/source.txt" 2>/dev/null) == "$stamp" && -f $D/expected.txt ]]; then
    log "unchanged ($(basename "$SRC"))"
    clean_staging
    exit 0
fi
# Generated next to the output and synchronised by content: files that come out the same keep
# their time, so an unchanged Android.bp does not make Soong regenerate (about 20 minutes).
NEW=$D.new
rm -rf "$NEW"
mkdir -p "$NEW"

# apksigner: the AOSP host build's, or the SDK's jar with AOSP's JDK.
APKSIGNER=()
if [[ -x $HOST/apksigner ]]; then
    APKSIGNER=("$HOST/apksigner")
else
    jar=$(ls -1 "$AOSP"/prebuilts/sdk/tools/lib/apksigner.jar /mnt/c/Users/*/AppData/Local/Android/Sdk/build-tools/*/lib/apksigner.jar 2>/dev/null | sort -V | tail -1 || true)
    java=$(ls -1 "$AOSP"/prebuilts/jdk/jdk*/linux-x86/bin/java 2>/dev/null | sort -V | tail -1 || true)
    [[ -n $jar && -n $java ]] && APKSIGNER=("$java" -jar "$jar")
fi
[[ ${#APKSIGNER[@]} -gt 0 ]] || { log "no apksigner (m apksigner, or Google's SDK build-tools)"; exit 1; }

python3 - "$SRC" "$NEW" "$AOSP" "$HOST/aapt2" "$HOST/zipalign" "${APKSIGNER[@]}" <<'PY'
import functools, glob, hashlib, json, os, re, shutil, subprocess, sys
import xml.etree.ElementTree as ET

src, out, aosp, aapt2, zipalign = sys.argv[1:6]
apksigner = sys.argv[6:]

def fail(message):
    print("[play] " + message, file=sys.stderr)
    sys.exit(1)

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()

def badging(apk):
    return subprocess.run([aapt2, "dump", "badging", apk], capture_output=True, text=True).stdout

ANDROID_SDK = 35  # Android 15

@functools.lru_cache(maxsize=None)  # one apksigner run (a JVM) per APK
def signer(apk):
    """The signing certificate (SHA-256) Android 15 uses for the app, after checking the APK.

    apksigner names a signer "Signer #1" (v1/v2/v3), or "Signer (minSdkVersion=a,
    maxSdkVersion=b)" when the signers differ by platform version (v3.1 key rotation, apps
    signed with v3 only); then the one whose range holds Android 15 counts."""
    text = subprocess.run(apksigner + ["verify", "--print-certs", apk], capture_output=True, text=True)
    if text.returncode != 0:
        fail("%s: signature does not verify: %s" % (apk, (text.stderr or text.stdout).strip()))
    ranged = re.findall(r"^Signer \(minSdkVersion=(\d+), maxSdkVersion=(\d+)\) certificate SHA-256 digest: ([0-9a-f]{64})",
                        text.stdout, re.M)
    for low, high, digest in ranged:
        if int(low) <= ANDROID_SDK <= int(high):
            return digest
    numbered = re.findall(r"^Signer #1 certificate SHA-256 digest: ([0-9a-f]{64})", text.stdout, re.M)
    if numbered:
        return numbered[0]
    fail("%s: no signer certificate for Android %d" % (apk, ANDROID_SDK))

@functools.lru_cache(maxsize=None)
def lineage(apk):
    """Every certificate the app was ever signed with (APK Signature Scheme v3 key rotation)."""
    certs = {signer(apk)}
    text = subprocess.run(apksigner + ["lineage", "--in", apk, "--print-certs"], capture_output=True, text=True)
    if text.returncode == 0:
        certs.update(re.findall(r"certificate SHA-256 digest: ([0-9a-f]{64})", text.stdout))
    return certs

def same_app_signer(old, new):
    # Android accepts an update signed by the same key or by a successor in its lineage.
    return signer(old) in lineage(new)

def preprocessed_issues(apk, privileged):
    """What Soong's check_prebuilt_presigned_apk.py finds in an unchanged ("preprocessed") APK:
    zip alignment, compressed JNI libraries, compressed dex in a privileged app (Android
    uncompresses privileged apps' dex). Soong refuses skip_preprocessed_apk_checks on an APK
    without issues, so the property is set exactly when there are some."""
    import zipfile
    if subprocess.run([zipalign, "-c", "-p", "4", apk], stdout=subprocess.DEVNULL).returncode != 0:
        return True
    with zipfile.ZipFile(apk) as z:
        for info in z.infolist():
            stored = info.compress_type == zipfile.ZIP_STORED
            if info.filename.startswith("lib/") and info.filename.endswith(".so") and not stored:
                return True
            if privileged and info.filename.endswith(".dex") and not stored:
                return True
    return False

def image_apk(kind, module):
    # The image's version of the app: our vendor/google_apps or MindTheGapps' vendor/gapps.
    for pattern in ("vendor/google_apps/%s/%s/%s.apk", "vendor/gapps/**/%s/%s/%s.apk"):
        found = glob.glob(os.path.join(aosp, pattern % (kind, module, module)), recursive=True)
        if found:
            return found[0]
    return None

with open(os.path.join(src, "play-apps.json"), encoding="utf-8-sig") as f:
    collected = json.load(f)

modules, privapp, versions, uses = [], {}, {}, {}
for app in collected["apps"]:
    package, image_dir = app["package"], app["imageDir"]
    m = re.fullmatch(r"/(system|system_ext|product|vendor)/(app|priv-app)/([^/]+)", image_dir)
    if not m:
        fail("%s: unexpected place in the image: %s" % (package, image_dir))
    partition, kind, folder = m.groups()
    # The base APK goes through android_app_import: Make honours "overrides" only for app
    # modules (PACKAGES.<name>.OVERRIDES), and an overridden module installed anyway would put
    # its own base APK next to this one. It installs <kind>/<name>/<name>.apk; the splits and
    # libraries go to the same folder through prebuilt_root.
    name = folder + "Play"
    copy_dir = os.path.join(out, folder)
    os.makedirs(copy_dir)
    srcs, dsts, bases = [], [], []
    for entry in app["files"]:
        rel = entry["name"]
        path = os.path.join(src, package, *rel.split("/"))
        if not os.path.isfile(path) or os.path.getsize(path) != entry["size"] or sha256(path) != entry["sha256"]:
            fail("%s: %s differs from play-apps.json" % (package, rel))
        is_base = rel.endswith(".apk") and "/" not in rel and not rel.startswith("split_")
        if is_base:
            bases.append(rel)
        installed = "base.apk" if is_base else rel
        target = os.path.join(copy_dir, *installed.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        try:
            os.link(path, target)  # same disk: no second copy of the APKs
        except OSError:
            shutil.copy2(path, target)
        if not is_base:
            srcs.append("%s/%s" % (folder, installed))
            dsts.append("%s/%s/%s" % (kind, name, installed))
    if len(bases) != 1:
        fail("%s: expected one base APK, found %s" % (package, bases or "none"))
    base = os.path.join(copy_dir, "base.apk")
    for rel in [r for r in os.listdir(copy_dir) if r.endswith(".apk")]:
        if signer(os.path.join(copy_dir, rel)) != signer(base):
            fail("%s: %s is signed by another certificate than its base APK" % (package, rel))
    old = image_apk(kind, folder)
    if old is None:
        fail("%s: the image's version (%s) was not found under vendor/" % (package, image_dir))
    if not same_app_signer(old, base):
        fail("%s: not signed by the image's version's key or a successor of it" % package)
    info = badging(base)
    versions[package] = re.search(r"versionCode='(\d+)'", info).group(1)
    lib = re.search(r"^static-library: name='([^']+)' version='(\d+)'", info, re.M)
    if lib:
        versions[lib.group(1)] = lib.group(2)
    uses[package] = re.findall(r"^uses-static-library: name='([^']+)' version='(\d+)'", info, re.M)
    overrides = [folder] + (["webview"] if package == "com.google.android.webview" else [])
    privileged = kind == "priv-app"
    modules.append({"name": name, "partition": partition, "kind": kind, "folder": folder,
                    "srcs": srcs, "dsts": dsts, "overrides": overrides, "privileged": privileged,
                    "skip_checks": preprocessed_issues(base, privileged)})
    if app.get("privappMissing"):
        privapp.setdefault(partition, {})[package] = app["privappMissing"]
    print("[play] %-40s %-28s %3d files -> /%s/%s/%s" % (package, app["versionName"], len(srcs) + 1, partition, kind, name),
          file=sys.stderr)

# Chrome and WebView load the Trichrome library of exactly their own version: either the one
# integrated here or the image's. The image's apps that stay (not collected) count as well.
for kind, module, package in (("app", "Chrome", "com.android.chrome"), ("app", "WebViewGoogle", "com.google.android.webview")):
    if package not in uses:
        old = image_apk(kind, module)
        if old:
            uses[package] = re.findall(r"^uses-static-library: name='([^']+)' version='(\d+)'", badging(old), re.M)
# A library no app loads any more (Chrome and WebView from Google Play stand alone) leaves the
# image instead of staying there unused.
needed = {library for needs in uses.values() for library, _ in needs}
if "com.google.android.trichromelibrary" not in needed and "com.google.android.trichromelibrary" not in versions:
    host = next((m for m in modules if m["name"] in ("ChromePlay", "WebViewGooglePlay")), None)
    if host:
        host["overrides"].append("TrichromeLibrary")
        print("[play] TrichromeLibrary: no app uses it any more, left out", file=sys.stderr)
for package, needs in uses.items():
    for library, version in needs:
        have = versions.get(library)
        if have is None:
            image = glob.glob(os.path.join(aosp, "vendor/google_apps/app/TrichromeLibrary/TrichromeLibrary.apk"))
            if image:
                found = re.search(r"^static-library: name='[^']+' version='(\d+)'", badging(image[0]), re.M)
                have = found.group(1) if found else None
        if have != version:
            fail("%s needs %s %s, the image would carry %s" % (package, library, version, have))

def partition_flag(partition):
    return {"system": None, "system_ext": "system_ext_specific", "product": "product_specific",
            "vendor": "vendor"}[partition]

def quoted(items):
    return "[" + ", ".join('"%s"' % i for i in items) + "]"

bp = ["// Generated by tools/integrate-play-apps.sh from %s. Do not edit." % os.path.basename(src)]
names = []
for m in modules:
    flag = partition_flag(m["partition"])
    names.append(m["name"])
    bp += ["", "android_app_import {", '    name: "%s",' % m["name"],
           '    apk: "%s/base.apk",' % m["folder"],
           "    presigned: true,", "    preprocessed: true,"]
    if m["skip_checks"]:
        bp.append("    skip_preprocessed_apk_checks: true,")
    bp += ["    dex_preopt: {", "        enabled: false,", "    },"]
    if m["privileged"]:
        bp.append("    privileged: true,")
    bp.append("    overrides: %s," % quoted(m["overrides"]))
    if flag:
        bp.append("    %s: true," % flag)
    bp.append("}")
    if m["srcs"]:
        names.append(m["name"] + "Splits")
        bp += ["", "prebuilt_root {", '    name: "%sSplits",' % m["name"],
               "    srcs: %s," % quoted(m["srcs"]), "    dsts: %s," % quoted(m["dsts"])]
        if flag:
            bp.append("    %s: true," % flag)
        bp.append("}")
for partition, packages in sorted(privapp.items()):
    name = "privapp-permissions-wsa-next-play-%s" % partition.replace("_", "-")
    root = ET.Element("permissions")
    for package, permissions in sorted(packages.items()):
        block = ET.SubElement(root, "privapp-permissions", package=package)
        for permission in permissions:
            ET.SubElement(block, "permission", name=permission)
    ET.indent(root)
    with open(os.path.join(out, name + ".xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n')
        f.write("<!-- Generated by tools/integrate-play-apps.sh: privileged permissions the Google Play\n"
                "     versions hold that the image's allowlist does not name. -->\n")
        f.write(ET.tostring(root, encoding="unicode") + "\n")
    names.append(name)
    flag = partition_flag(partition)
    bp += ["", "prebuilt_etc {", '    name: "%s",' % name, '    src: "%s.xml",' % name,
           "    filename_from_src: true,", '    sub_dir: "permissions",']
    if flag:
        bp.append("    %s: true," % flag)
    bp.append("}")
with open(os.path.join(out, "Android.bp"), "w") as f:
    f.write("\n".join(bp) + "\n")
with open(os.path.join(out, "play.mk"), "w") as f:
    f.write("# Generated by tools/integrate-play-apps.sh. Do not edit.\n")
    f.write("PRODUCT_PACKAGES += \\\n" + " \\\n".join("    " + n for n in names) + "\n")
# What the staging folders may hold (see clean_staging): every installed file of the
# integrated apps, their folders, and the folders of modules left out.
with open(os.path.join(out, "expected.txt"), "w") as f:
    for m in modules:
        f.write("%s/%s/%s/%s.apk\n" % (m["partition"], m["kind"], m["name"], m["name"]))
        f.writelines("%s/%s\n" % (m["partition"], d) for d in m["dsts"])
with open(os.path.join(out, "folders.txt"), "w") as f:
    f.writelines("%s/%s/%s\n" % (m["partition"], m["kind"], m["name"]) for m in modules)
with open(os.path.join(out, "dropped.txt"), "w") as f:
    f.writelines("%s/%s/%s\n" % (m["partition"], m["kind"], m["folder"]) for m in modules)
    if any("TrichromeLibrary" in m["overrides"] for m in modules):
        f.write("product/app/TrichromeLibrary\n")
print("[play] %d apps integrated" % len(modules), file=sys.stderr)
PY
echo "$stamp" > "$NEW/source.txt"
mkdir -p "$D"
# No -t: a file with the same content keeps its time (rsync -a would set it again).
rsync -rl --checksum --delete "$NEW/" "$D/"
rm -rf "$NEW"
clean_staging
