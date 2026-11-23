# WSA-Next © 2026 Marek Wesołowski - wesmar
<#
.SYNOPSIS
    Collects the Google apps that Google Play has updated on a running WSA-Next, to integrate
    them into the next image (android/tools/integrate-play-apps.sh).

.DESCRIPTION
    Every WSA-Next installation is the same device for Google Play (x86_64 with x86, the same
    density and Android version), so the updates Play delivers to one of them are the ones every
    user would download in their first minutes. This script takes them from a reference
    installation, bit for bit, the way uupdump takes Windows from Microsoft's servers:

      1. every system app whose current code is an update in /data/app (Play services, the Play
         Store, Chrome, WebView, YouTube, ...), with all of its APKs (base and splits) and, when
         the app extracts its native libraries, the extracted lib/ folders;
      2. every newer version of a static shared library the image carries (TrichromeLibrary).

    Nothing is changed or re-signed. Android verified each update against the signature of the
    app in the image when Play installed it; play-apps.json records versions, the place in the
    image and the SHA-256 of every file, and the integration step checks them again.

    Google Play delivers an app's language splits only for the languages on the device's list,
    so the reference installation lists every language first (tools/locales/SetLocales.java,
    the first language stays the UI's); Play then adds the missing language splits to every app
    by itself. With fewer than -MinLanguages languages the script stops: the image would greet
    the users of the missing languages with Play's "switching to your language" screen.

    Needs adb (Google's Platform-Tools) and WSA's developer mode on. Run it after Google Play has
    finished "Update all" on a clean installation that is signed in.

.PARAMETER Serial
    adb address of WSA (default 127.0.0.1:58526).

.PARAMETER Out
    Output folder; a dated subfolder is created in it.

.PARAMETER DryRun
    Only list what would be collected.

.PARAMETER MinLanguages
    Fewest languages the device must list (default 70).
#>
param(
    [string]$Serial = '127.0.0.1:58526',
    [string]$Out = 'F:\WSA\android-15\inputs\play-apps',
    [switch]$DryRun,
    [int]$MinLanguages = 70
)
$ErrorActionPreference = 'Stop'

function Find-Adb {
    $onPath = Get-Command adb -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
    if (Test-Path $sdk) { return $sdk }
    throw 'adb not found: install Google''s SDK Platform-Tools or put adb on PATH.'
}
$adb = Find-Adb

function Invoke-Adb {
    $output = & $adb -s $Serial @args 2>&1
    if ($LASTEXITCODE -ne 0) { throw "adb $($args -join ' '): $output" }
    return $output
}
function Invoke-Shell([string]$command) { Invoke-Adb shell $command }

& $adb connect $Serial | Out-Null
if ((Invoke-Shell 'getprop sys.boot_completed') -ne '1') { throw "Android at $Serial has not finished starting." }
Invoke-Adb root | Out-Null
Invoke-Adb wait-for-device | Out-Null
$languages = @((Invoke-Shell 'settings get system system_locales') -split ',' | Where-Object { $_ })
if ($languages.Count -lt $MinLanguages) {
    throw "The device lists $($languages.Count) languages; Google Play delivers language splits only for listed ones. List them all first (tools/locales/SetLocales.java), let Play add the splits, then collect."
}

# --- What Android knows about every package -------------------------------------------------
# "dumpsys package packages" lists the current packages, then "Hidden system packages:" - the
# image's versions of the apps that have an update.
function Read-Packages([string[]]$lines) {
    $list = @(); $current = $null
    foreach ($line in $lines) {
        if ($line -match '^  Package \[(.+?)\] \(') {
            if ($current) { $list += [pscustomobject]$current }
            $current = @{ Name = $Matches[1]; CodePath = ''; VersionCode = ''; VersionName = ''; Flags = '' }
        } elseif ($current) {
            if ($line -match '^    codePath=(.+)$') { $current.CodePath = $Matches[1].Trim() }
            elseif ($line -match '^    versionCode=(\d+)') { $current.VersionCode = $Matches[1] }
            elseif ($line -match '^    versionName=(.+)$') { $current.VersionName = $Matches[1].Trim() }
            elseif ($line -match '^    flags=\[(.+)\]') { $current.Flags = $Matches[1] }
        }
    }
    if ($current) { $list += [pscustomobject]$current }
    return $list
}
$dump = Invoke-Shell 'dumpsys package packages'
$split = [Array]::IndexOf($dump, ($dump | Where-Object { $_ -match '^Hidden system packages:' } | Select-Object -First 1))
$currentLines = if ($split -ge 0) { $dump[0..($split - 1)] } else { $dump }
$hiddenLines = if ($split -ge 0) { $dump[$split..($dump.Count - 1)] } else { @() }
$packages = Read-Packages $currentLines
$hidden = @{}
foreach ($p in (Read-Packages $hiddenLines)) { $hidden[$p.Name] = $p }

$partitions = '^/(system|system_ext|product|vendor)/(app|priv-app)/[^/]+$'
$targets = @()
foreach ($p in $packages) {
    if (-not $p.CodePath.StartsWith('/data/app/')) { continue }
    if ($p.Flags -match '\bUPDATED_SYSTEM_APP\b' -and $hidden.ContainsKey($p.Name)) {
        $targets += [pscustomobject]@{ Package = $p.Name; Current = $p; ImageDir = $hidden[$p.Name].CodePath; ImageVersion = $hidden[$p.Name].VersionCode }
        continue
    }
    # A static shared library is a package per version ("<name>_<version>"): a newer one from Play
    # replaces the one in the image.
    if ($p.Name -match '^(.+)_(\d+)$') {
        $base = $Matches[1]
        $inImage = $packages | Where-Object { $_.Name -match ('^' + [regex]::Escape($base) + '_\d+$') -and $_.CodePath -match $partitions } | Select-Object -First 1
        if ($inImage -and [int64]$p.VersionCode -gt [int64]$inImage.VersionCode) {
            $targets += [pscustomobject]@{ Package = $base; Current = $p; ImageDir = $inImage.CodePath; ImageVersion = $inImage.VersionCode }
        }
    }
}
if (-not $targets) { throw 'No updated system apps: let Google Play finish "Update all" first.' }
foreach ($t in $targets) {
    if ($t.ImageDir -notmatch $partitions) { throw "$($t.Package): unexpected place in the image: $($t.ImageDir)" }
}

# --- Privileged permissions -------------------------------------------------------------------
# Android 15 stops the system server at boot when a privileged app in the image requests a
# privileged permission of the platform ("android") that the allowlist of its partition does not
# name. Updated apps are exempt, so a newer version may hold permissions the image's allowlist
# lacks. They are found here, on the device that granted them: requested by the update,
# privileged, defined by "android", and neither allowed nor denied in <partition>/etc/permissions.
$privileged = @{}
$name = $null; $owner = $null
foreach ($line in (Invoke-Shell 'pm list permissions -f')) {
    if ($line -match '^\+ permission:(.+)$') { $name = $Matches[1].Trim(); $owner = $null }
    elseif ($line -match '^\s+package:(.+)$') { $owner = $Matches[1].Trim() }
    elseif ($line -match '^\s+protectionLevel:(.+)$' -and $owner -eq 'android' -and $Matches[1] -match '\bprivileged\b') { $privileged[$name] = $true }
}
function Get-Allowlisted([string]$partition, [string]$package) {
    $listed = @{}
    $files = Invoke-Shell "ls /$partition/etc/permissions/*.xml 2>/dev/null"
    foreach ($file in $files) {
        if (-not $file) { continue }
        $xml = [xml]((Invoke-Shell "cat '$file'") -join "`n")
        foreach ($block in $xml.SelectNodes("//privapp-permissions[@package='$package']")) {
            foreach ($child in $block.ChildNodes) { if ($child.name) { $listed[$child.name] = $child.LocalName } }
        }
    }
    return $listed
}
function Get-Requested([string]$package) {
    $requested = @(); $inside = $false
    foreach ($line in (Invoke-Shell "dumpsys package $package")) {
        if ($line -match '^Hidden system packages:') { break }  # only the current version
        if ($line -match '^    requested permissions:') { $inside = $true; continue }
        if ($inside) {
            if ($line -match '^      ([\w.]+)') { $requested += $Matches[1] } else { break }
        }
    }
    return $requested
}
foreach ($t in $targets) {
    $t | Add-Member privappMissing @()
    if ($t.ImageDir -notmatch '^/([^/]+)/priv-app/') { continue }
    $listed = Get-Allowlisted $Matches[1] $t.Package
    $t.privappMissing = @(Get-Requested $t.Package | Where-Object { $privileged[$_] -and -not $listed.ContainsKey($_) } | Sort-Object -Unique)
}

# --- Pull -------------------------------------------------------------------------------------
if ($DryRun) {
    foreach ($t in $targets | Sort-Object Package) {
        Write-Host ("{0,-45} {1} -> {2} (image {3})" -f $t.Package, $t.Current.VersionName, $t.ImageDir, $t.ImageVersion)
        if ($t.privappMissing) { Write-Host ("    not in the allowlist: " + ($t.privappMissing -join ', ')) }
    }
    return
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$folder = Join-Path $Out $stamp
New-Item -ItemType Directory -Force $folder | Out-Null
$report = @()
foreach ($t in $targets | Sort-Object Package) {
    $dir = $t.Current.CodePath
    $target = Join-Path $folder $t.Package
    New-Item -ItemType Directory -Force $target | Out-Null
    $files = @()
    $apks = Invoke-Shell "cd '$dir' && ls -1 *.apk"
    $libs = Invoke-Shell "cd '$dir' && if [ -d lib ]; then find lib -type f -name '*.so'; fi"
    foreach ($relative in @($apks) + @($libs)) {
        if (-not $relative) { continue }
        $local = Join-Path $target ($relative -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path $local) | Out-Null
        Invoke-Adb pull "$dir/$relative" $local | Out-Null
        $files += [ordered]@{ name = $relative; size = (Get-Item $local).Length; sha256 = (Get-FileHash $local -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $entry = [ordered]@{
        package      = $t.Package
        versionCode  = [int64]$t.Current.VersionCode
        versionName  = $t.Current.VersionName
        imageDir     = $t.ImageDir
        imageVersion = [int64]$t.ImageVersion
        privappMissing = $t.privappMissing
        files        = $files
    }
    $report += $entry
    Write-Host ("{0,-45} {1,-28} {2,3} files  -> {3}{4}" -f $t.Package, $t.Current.VersionName, $files.Count, $t.ImageDir,
        $(if ($t.privappMissing) { "  (+$($t.privappMissing.Count) privileged permissions)" }))
}

$device = [ordered]@{
    fingerprint = (Invoke-Shell 'getprop ro.build.fingerprint')
    abis        = (Invoke-Shell 'getprop ro.product.cpu.abilist')
    density     = (Invoke-Shell 'getprop ro.sf.lcd_density')
    languages   = $languages
}
[ordered]@{ collected = (Get-Date).ToString('o'); device = $device; apps = $report } |
    ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $folder 'play-apps.json')
Write-Host "Collected $($report.Count) apps in $folder"
