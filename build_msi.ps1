# WSA-Next © 2026 Marek Wesołowski - wesmar
#Requires -Version 7.0
<#
.SYNOPSIS
    Builds WSA-Next Setup: bin\WSA-Next.msi, and bin\WSA-Next.zip with the same files for those who
    prefer no installer.

.DESCRIPTION
    Windows' own tools only - makecab.exe and the Windows Installer COM API - so that anyone with
    Windows can rebuild the package from these sources and compare it with a release.

    WSA-Next.msi:
      * per machine, x64, in %ProgramFiles%\WSA-Next. The folder is fixed: the background host
        starts at logon with the highest rights and must run from a folder only administrators
        can change, and WSA-Next looks for itself there;
      * a setup wizard: welcome, the MIT License, ready (with a desktop shortcut), progress, finish
        (start WSA-Next). Run again: repair, or remove - optionally with the Android apps and their
        data;
      * in every language WSA-Next speaks: one transform per language embedded in the package
        (installer\msi-strings.json), which Windows applies by the user's language. Windows
        Installer keeps text in an ANSI code page; Hindi has none and stays English;
      * WSA-Next's own work around it: its background host is stopped before files change and
        started again after an update, and removing WSA-Next runs "WSA-Next.exe uninstall" first,
        so the Windows Subsystem for Android, its disks, its registration and the host's logon
        task go too (WSA-Next.exe elevates itself for that when needed);
      * major upgrades (one UpgradeCode, component IDs derived from the file names), no downgrade,
        an Apps & Features entry with the icon and the project's links.

.PARAMETER SignScript
    Signs WSA-Next.msi with the publisher's certificate (a private script, not in this repository).

.PARAMETER AllowUnsigned
    Test builds: accept an unsigned bin\WSA-Next.exe. Official packages carry a signed executable.

.EXAMPLE
    .\build.ps1; <sign bin\WSA-Next.exe>; .\build_msi.ps1 -SignScript C:\path\to\sign.ps1
#>
[CmdletBinding()]
param(
    [string]$SignScript,
    [switch]$AllowUnsigned
)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$bin = Join-Path $root 'bin'
$exePath = Join-Path $bin 'WSA-Next.exe'
$langDir = Join-Path $bin 'language'
$iconPath = Join-Path $root 'src\resources\icon.ico'
$licensePath = Join-Path $root 'LICENSE.md'
$stringsPath = Join-Path $root 'installer\msi-strings.json'
$msiPath = Join-Path $bin 'WSA-Next.msi'
$zipPath = Join-Path $bin 'WSA-Next.zip'

# The identity of the product across all its versions. Never change it: Windows finds earlier
# versions by it and replaces them.
$upgradeCode = '{8F4A9E21-6C3D-4B7A-9E12-5D8A1C3F4E90}'

# --- Inputs -------------------------------------------------------------------------------------
foreach ($p in $exePath, $iconPath, $licensePath, $stringsPath) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Missing: $p (run .\build.ps1 first for bin\)." }
}
$bytes = [IO.File]::ReadAllBytes($exePath)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'bin\WSA-Next.exe is not an x64 executable.' }
$exeSignature = Get-AuthenticodeSignature -LiteralPath $exePath
if ($exeSignature.Status -ne 'Valid' -and -not $AllowUnsigned) {
    throw "bin\WSA-Next.exe is not signed ($($exeSignature.Status)). Sign it first, or pass -AllowUnsigned for a test build."
}
$fileVersion = [version](Get-Item -LiteralPath $exePath).VersionInfo.FileVersion
$productVersion = '{0}.{1}.{2}' -f $fileVersion.Major, $fileVersion.Minor, $fileVersion.Build  # MSI compares three fields
$lngFiles = @(Get-ChildItem -LiteralPath $langDir -Filter '*.lng' -File -ErrorAction SilentlyContinue | Sort-Object Name)
$text = Get-Content -LiteralPath $stringsPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
$en = $text.strings.en
[Text.Encoding]::RegisterProvider([Text.CodePagesEncodingProvider]::Instance)

Write-Host "WSA-Next Setup  |  v$productVersion  |  WSA-Next.exe $fileVersion ($($exeSignature.Status))  |  $($lngFiles.Count) language files  |  $($text.languages.Count + 1) setup languages" -ForegroundColor Cyan

# --- Helpers ------------------------------------------------------------------------------------
# A stable GUID from a name (SHA-1, version 5 layout): the same file keeps the same component ID
# in every version, as Windows Installer's component rules want.
function NameGuid([string]$name) {
    $hash = [Security.Cryptography.SHA1]::HashData([byte[]]([guid]$upgradeCode).ToByteArray() + [Text.Encoding]::UTF8.GetBytes($name))
    $hash[6] = ($hash[6] -band 0x0F) -bor 0x50
    $hash[8] = ($hash[8] -band 0x3F) -bor 0x80
    '{' + ([guid]::new([byte[]]$hash[0..15])).ToString().ToUpperInvariant() + '}'
}
function NewGuid { '{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}' }

$installer = New-Object -ComObject WindowsInstaller.Installer
$Method = [Reflection.BindingFlags]::InvokeMethod
$Get = [Reflection.BindingFlags]::GetProperty
$Set = [Reflection.BindingFlags]::SetProperty
function Inv($obj, [string]$name, [Reflection.BindingFlags]$kind, [object[]]$params) {
    # IDispatch takes plain values: PowerShell's wrappers (PSObject) are taken off.
    if ($null -ne $params) { $params = [object[]]@(foreach ($p in $params) { if ($p -is [psobject]) { $p.psobject.BaseObject } else { $p } }) }
    $obj.GetType().InvokeMember($name, $kind, $null, $obj, $params)
}
function Free($obj) { if ($obj) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($obj) } }

# Runs a query with ? parameters: integers as integers, FileInfo as a stream, the rest as text,
# $null as a null field.
function Sql($db, [string]$query, [object[]]$values) {
    $view = Inv $db 'OpenView' $Method @($query)
    $record = $null
    try {
        if ($values) {
            $record = Inv $installer 'CreateRecord' $Method @([int]$values.Count)
            for ($i = 0; $i -lt $values.Count; $i++) {
                $v = $values[$i]
                if ($null -eq $v) { continue }
                if ($v -is [int] -or $v -is [long]) { [void](Inv $record 'IntegerData' $Set @([int]($i + 1), [int]$v)) }
                elseif ($v -is [IO.FileInfo]) { [void](Inv $record 'SetStream' $Method @([int]($i + 1), $v.FullName)) }
                else { [void](Inv $record 'StringData' $Set @([int]($i + 1), [string]$v)) }
            }
            [void](Inv $view 'Execute' $Method @($record))
        } else {
            [void](Inv $view 'Execute' $Method $null)
        }
    } finally {
        [void](Inv $view 'Close' $Method $null)
        Free $record
        Free $view
    }
}
function Insert($db, [string]$table, [string[]]$columns, [object[]]$values) {
    if ($values.Count -ne $columns.Count) { throw "$table`: $($columns.Count) columns, $($values.Count) values ($($values -join ' | '))." }
    $names = ($columns | ForEach-Object { "``$_``" }) -join ','
    $marks = (1..$columns.Count | ForEach-Object { '?' }) -join ','
    Sql $db "INSERT INTO ``$table`` ($names) VALUES ($marks)" $values
}

# Text as the language's ANSI code page can hold it: precomposed letters it lacks are written as
# a base letter and combining marks (Vietnamese), and the language's own replacements apply
# (Persian yeh -> Arabic yeh, which cp1256 has). Anything else is an error, never a silent "?".
function ForCodePage([string]$value, [int]$codePage, $replace, [string]$what) {
    if ($replace) { foreach ($k in $replace.Keys) { $value = $value.Replace($k, $replace[$k]) } }
    $encoding = [Text.Encoding]::GetEncoding($codePage, [Text.EncoderFallback]::ExceptionFallback, [Text.DecoderFallback]::ExceptionFallback)
    $fits = { param($s) try { [void]$encoding.GetBytes([string]$s); $true } catch { $false } }
    $out = [Text.StringBuilder]::new()
    $elements = [Globalization.StringInfo]::GetTextElementEnumerator($value.Normalize([Text.NormalizationForm]::FormC))
    while ($elements.MoveNext()) {
        $element = [string]$elements.Current
        if (& $fits $element) { [void]$out.Append($element); continue }
        # A base letter with some of its marks composed (the code page's own letters, such as
        # Vietnamese "ă"), the other marks after it as combining characters ("ặ" = "ă" + dot below).
        $parts = $element.Normalize([Text.NormalizationForm]::FormD)
        $base = $parts.Substring(0, 1)
        $marks = $parts.Substring(1).ToCharArray()
        $written = $false
        for ($mask = (1 -shl $marks.Count) - 1; $mask -ge 0 -and -not $written; $mask--) {
            $composed = $base; $rest = ''
            for ($m = 0; $m -lt $marks.Count; $m++) { if ($mask -band (1 -shl $m)) { $composed += $marks[$m] } else { $rest += $marks[$m] } }
            $head = $composed.Normalize([Text.NormalizationForm]::FormC)
            if ($head.Length -eq 1 -and (& $fits $head) -and ($rest -eq '' -or (& $fits $rest))) { [void]$out.Append($head + $rest); $written = $true }
        }
        if (-not $written) { throw "$what`: '$element' does not exist in code page $codePage." }
    }
    $out.ToString()
}

# --- Staging: cabinet, bitmaps, license ----------------------------------------------------------
$stage = Join-Path ([IO.Path]::GetTempPath()) "wsa-next-msi-$PID"
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    # Files: WSA-Next.exe and every language file, one component each (its key path).
    $files = [Collections.Generic.List[object]]::new()
    $files.Add([pscustomobject]@{ Key = 'WsaNextExe'; Component = 'CompExe'; Directory = 'INSTALLDIR'; Name = 'WSA-Next.exe'; Source = $exePath; Version = $fileVersion.ToString() })
    foreach ($lng in $lngFiles) {
        $id = ($lng.BaseName -replace '[^A-Za-z0-9]', '_')
        $files.Add([pscustomobject]@{ Key = "Lng_$id"; Component = "CompLng_$id"; Directory = 'LANGDIR'; Name = $lng.Name; Source = $lng.FullName; Version = $null })
    }
    $cabFiles = Join-Path $stage 'cab'
    New-Item -ItemType Directory -Path $cabFiles -Force | Out-Null
    $ddf = [Collections.Generic.List[string]]::new()
    foreach ($line in '.OPTION EXPLICIT', '.Set CabinetNameTemplate=data.cab', ".Set DiskDirectoryTemplate=$stage", '.Set Cabinet=on', '.Set Compress=on',
                      '.Set CompressionType=LZX', '.Set CompressionMemory=21', '.Set MaxDiskSize=0', '.Set InfFileName=NUL', '.Set RptFileName=NUL') { $ddf.Add($line) }
    foreach ($f in $files) {
        $staged = Join-Path $cabFiles $f.Key
        Copy-Item -LiteralPath $f.Source -Destination $staged -Force
        $ddf.Add("`"$staged`" $($f.Key)")
    }
    Set-Content -LiteralPath (Join-Path $stage 'data.ddf') -Value $ddf -Encoding ASCII
    Push-Location $stage
    try {
        & makecab.exe /F (Join-Path $stage 'data.ddf') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "makecab.exe failed ($LASTEXITCODE)." }
    } finally { Pop-Location }
    $cabPath = Join-Path $stage 'data.cab'

    # Bitmaps at Windows Installer's own scale (370 x 234 units = 493 x 312 pixels at 96 DPI), so
    # the 48-pixel icon is drawn 1:1: the welcome / finish picture (a dark panel with the icon on
    # the left, white behind the text) and the banner of the other pages.
    Add-Type -AssemblyName System.Drawing
    $logo = [Drawing.Icon]::new($iconPath, 48, 48).ToBitmap()
    $dialogBmp = Join-Path $stage 'dialog.bmp'
    $bannerBmp = Join-Path $stage 'banner.bmp'
    $bmp = [Drawing.Bitmap]::new(493, 312, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::White)
    $panel = [Drawing.Rectangle]::new(0, 0, 164, 312)
    $gradient = [Drawing.Drawing2D.LinearGradientBrush]::new($panel, [Drawing.Color]::FromArgb(0x0B, 0x12, 0x20), [Drawing.Color]::FromArgb(0x0E, 0x3B, 0x2A), 90.0)
    $g.FillRectangle($gradient, $panel)
    $g.FillRectangle([Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(0x3D, 0xDC, 0x84)), 164, 0, 2, 312)
    $g.DrawImage($logo, 58, 96, 48, 48)
    $g.Dispose(); $gradient.Dispose()
    $bmp.Save($dialogBmp, [Drawing.Imaging.ImageFormat]::Bmp); $bmp.Dispose()
    $bmp = [Drawing.Bitmap]::new(493, 58, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::White)
    $g.DrawImage($logo, 493 - 48 - 12, 5, 48, 48)
    $g.Dispose()
    $bmp.Save($bannerBmp, [Drawing.Imaging.ImageFormat]::Bmp); $bmp.Dispose(); $logo.Dispose()

    # The license page shows LICENSE.md as RTF (paragraphs; non-ASCII as \uN escapes).
    $paragraphs = (Get-Content -LiteralPath $licensePath -Raw -Encoding UTF8) -split '(\r?\n){2,}' | ForEach-Object { ($_ -replace '\r?\n', ' ').Trim() } | Where-Object { $_ }
    $escape = {
        param([string]$s)
        $sb = [Text.StringBuilder]::new()
        foreach ($ch in $s.ToCharArray()) {
            if ('\{}'.Contains($ch)) { [void]$sb.Append('\').Append($ch) }
            elseif ([int]$ch -gt 127) { [void]$sb.Append("\u$([int]$ch)?") }
            else { [void]$sb.Append($ch) }
        }
        $sb.ToString()
    }
    $rtf = '{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fswiss\fcharset0 Segoe UI;}}\viewkind4\uc1\pard\sa120\f0\fs18 '
    $rtf += '\b ' + (& $escape $paragraphs[0]) + '\b0\par '
    $rtf += (($paragraphs | Select-Object -Skip 1 | ForEach-Object { & $escape $_ }) -join '\par ') + '\par}'

    # --- The database ---------------------------------------------------------------------------
    if (Test-Path -LiteralPath $msiPath) { Remove-Item -LiteralPath $msiPath -Force }
    $db = Inv $installer 'OpenDatabase' $Method @($msiPath, [int]3)  # msiOpenDatabaseModeCreate
    $schema = @(
        'CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)'
        'CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)'
        'CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `ComponentId` CHAR(38), `Directory_` CHAR(72) NOT NULL, `Attributes` SHORT NOT NULL, `Condition` CHAR(255), `KeyPath` CHAR(72) PRIMARY KEY `Component`)'
        'CREATE TABLE `Feature` (`Feature` CHAR(38) NOT NULL, `Feature_Parent` CHAR(38), `Title` CHAR(64) LOCALIZABLE, `Description` CHAR(255) LOCALIZABLE, `Display` SHORT, `Level` SHORT NOT NULL, `Directory_` CHAR(72), `Attributes` SHORT NOT NULL PRIMARY KEY `Feature`)'
        'CREATE TABLE `FeatureComponents` (`Feature_` CHAR(38) NOT NULL, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Feature_`, `Component_`)'
        'CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, `Version` CHAR(72), `Language` CHAR(20), `Attributes` SHORT, `Sequence` SHORT NOT NULL PRIMARY KEY `File`)'
        'CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `DiskPrompt` CHAR(64) LOCALIZABLE, `Cabinet` CHAR(255), `VolumeLabel` CHAR(32), `Source` CHAR(72) PRIMARY KEY `DiskId`)'
        'CREATE TABLE `Icon` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)'
        'CREATE TABLE `Binary` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)'
        'CREATE TABLE `Shortcut` (`Shortcut` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL, `Name` CHAR(128) NOT NULL LOCALIZABLE, `Component_` CHAR(72) NOT NULL, `Target` CHAR(72) NOT NULL, `Arguments` CHAR(255), `Description` CHAR(255) LOCALIZABLE, `Hotkey` SHORT, `Icon_` CHAR(72), `IconIndex` SHORT, `ShowCmd` SHORT, `WkDir` CHAR(72) PRIMARY KEY `Shortcut`)'
        'CREATE TABLE `Registry` (`Registry` CHAR(72) NOT NULL, `Root` SHORT NOT NULL, `Key` CHAR(255) NOT NULL LOCALIZABLE, `Name` CHAR(255) LOCALIZABLE, `Value` LONGCHAR LOCALIZABLE, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Registry`)'
        'CREATE TABLE `Upgrade` (`UpgradeCode` CHAR(38) NOT NULL, `VersionMin` CHAR(20), `VersionMax` CHAR(20), `Language` CHAR(255), `Attributes` LONG NOT NULL, `Remove` CHAR(255), `ActionProperty` CHAR(72) NOT NULL PRIMARY KEY `UpgradeCode`, `VersionMin`, `VersionMax`, `Language`, `Attributes`)'
        'CREATE TABLE `LaunchCondition` (`Condition` CHAR(255) NOT NULL, `Description` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Condition`)'
        'CREATE TABLE `AppSearch` (`Property` CHAR(72) NOT NULL, `Signature_` CHAR(72) NOT NULL PRIMARY KEY `Property`, `Signature_`)'
        'CREATE TABLE `Signature` (`Signature` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL, `MinVersion` CHAR(20), `MaxVersion` CHAR(20), `MinSize` LONG, `MaxSize` LONG, `MinDate` LONG, `MaxDate` LONG, `Languages` CHAR(255) PRIMARY KEY `Signature`)'
        'CREATE TABLE `DrLocator` (`Signature_` CHAR(72) NOT NULL, `Parent` CHAR(72), `Path` CHAR(255), `Depth` SHORT PRIMARY KEY `Signature_`, `Parent`, `Path`)'
        'CREATE TABLE `CustomAction` (`Action` CHAR(72) NOT NULL, `Type` SHORT NOT NULL, `Source` CHAR(72), `Target` CHAR(255), `ExtendedType` LONG PRIMARY KEY `Action`)'
        'CREATE TABLE `ActionText` (`Action` CHAR(72) NOT NULL, `Description` CHAR(64) LOCALIZABLE, `Template` CHAR(128) LOCALIZABLE PRIMARY KEY `Action`)'
        'CREATE TABLE `InstallExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)'
        'CREATE TABLE `InstallUISequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)'
        'CREATE TABLE `CheckBox` (`Property` CHAR(72) NOT NULL, `Value` CHAR(64) PRIMARY KEY `Property`)'
        'CREATE TABLE `TextStyle` (`TextStyle` CHAR(72) NOT NULL, `FaceName` CHAR(32) NOT NULL, `Size` SHORT NOT NULL, `Color` LONG, `StyleBits` SHORT PRIMARY KEY `TextStyle`)'
        'CREATE TABLE `Dialog` (`Dialog` CHAR(72) NOT NULL, `HCentering` SHORT NOT NULL, `VCentering` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, `Title` CHAR(128) LOCALIZABLE, `Control_First` CHAR(50) NOT NULL, `Control_Default` CHAR(50), `Control_Cancel` CHAR(50) PRIMARY KEY `Dialog`)'
        'CREATE TABLE `Control` (`Dialog_` CHAR(72) NOT NULL, `Control` CHAR(50) NOT NULL, `Type` CHAR(20) NOT NULL, `X` SHORT NOT NULL, `Y` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, `Property` CHAR(50), `Text` LONGCHAR LOCALIZABLE, `Control_Next` CHAR(50), `Help` CHAR(50) LOCALIZABLE PRIMARY KEY `Dialog_`, `Control`)'
        'CREATE TABLE `ControlEvent` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Event` CHAR(50) NOT NULL, `Argument` CHAR(255) NOT NULL, `Condition` CHAR(255), `Ordering` SHORT PRIMARY KEY `Dialog_`, `Control_`, `Event`, `Argument`, `Condition`)'
        'CREATE TABLE `ControlCondition` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Action` CHAR(50) NOT NULL, `Condition` CHAR(255) NOT NULL PRIMARY KEY `Dialog_`, `Control_`, `Action`, `Condition`)'
        'CREATE TABLE `EventMapping` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, `Event` CHAR(50) NOT NULL, `Attribute` CHAR(50) NOT NULL PRIMARY KEY `Dialog_`, `Control_`, `Event`)'
    )
    foreach ($q in $schema) { Sql $db $q $null }

    # Properties. Manufacturer in ASCII here: the English database uses code page 1252, which has no
    # "ł"; the Polish and Czech transforms carry the full name.
    $manufacturerAscii = 'Marek Wesolowski - wesmar'
    $manufacturer = 'Marek Wesołowski - wesmar'
    $properties = [ordered]@{
        ProductCode            = NewGuid
        ProductName            = 'WSA-Next'
        ProductVersion         = $productVersion
        Manufacturer           = $manufacturerAscii
        ProductLanguage        = '1033'
        UpgradeCode            = $upgradeCode
        ALLUSERS               = '1'
        ARPPRODUCTICON         = 'WsaNextIcon.ico'
        ARPURLINFOABOUT        = 'https://kvc.pl/wsa-next'
        ARPHELPLINK            = 'https://github.com/wesmar/wsa-next/issues'
        ARPURLUPDATEINFO       = 'https://github.com/wesmar/wsa-next/releases'
        ARPCOMMENTS            = $en.ShortcutDesc
        ARPNOMODIFY            = '1'
        REINSTALLMODE          = 'amus'
        DESKTOPSHORTCUT        = '1'
        LAUNCHAPP              = '1'
        DefaultUIFont          = 'FontNormal'
        ErrorDialog            = 'ErrorDlg'
        SecureCustomProperties = 'OLDERVERSIONBEINGUPGRADED;NEWERVERSIONDETECTED;DESKTOPSHORTCUT;REMOVEDATA;EXISTINGEXE'
        # The host is stopped by its own event (StopHosts); no Restart Manager dialogs.
        MSIRESTARTMANAGERCONTROL = 'Disable'
    }
    foreach ($kv in $properties.GetEnumerator()) { Insert $db 'Property' @('Property', 'Value') @($kv.Key, $kv.Value) }

    # Same or older versions are replaced (512: VersionMax inclusive); a newer one stops setup.
    $upgradeColumns = @('UpgradeCode', 'VersionMin', 'VersionMax', 'Language', 'Attributes', 'Remove', 'ActionProperty')
    Insert $db 'Upgrade' $upgradeColumns @($upgradeCode, $null, $productVersion, $null, 512, $null, 'OLDERVERSIONBEINGUPGRADED')
    Insert $db 'Upgrade' $upgradeColumns @($upgradeCode, $productVersion, $null, $null, 2, $null, 'NEWERVERSIONDETECTED')  # 2: detect only
    Insert $db 'LaunchCondition' @('Condition', 'Description') @('VersionNT64', $en.Need64)
    Insert $db 'LaunchCondition' @('Condition', 'Description') @('NOT NEWERVERSIONDETECTED OR Installed', $en.NewerInstalled)

    # Folders: %ProgramFiles%\WSA-Next\language, the Start menu, the desktop.
    $dirColumns = @('Directory', 'Directory_Parent', 'DefaultDir')
    Insert $db 'Directory' $dirColumns @('TARGETDIR', $null, 'SourceDir')
    Insert $db 'Directory' $dirColumns @('ProgramFiles64Folder', 'TARGETDIR', '.')
    Insert $db 'Directory' $dirColumns @('INSTALLDIR', 'ProgramFiles64Folder', 'WSA-Next')
    Insert $db 'Directory' $dirColumns @('LANGDIR', 'INSTALLDIR', 'language')
    Insert $db 'Directory' $dirColumns @('ProgramMenuFolder', 'TARGETDIR', '.')
    Insert $db 'Directory' $dirColumns @('DesktopFolder', 'TARGETDIR', '.')

    Insert $db 'Feature' @('Feature', 'Feature_Parent', 'Title', 'Description', 'Display', 'Level', 'Directory_', 'Attributes') @('MainFeature', $null, 'WSA-Next', $en.ShortcutDesc, 0, 1, 'INSTALLDIR', 0)

    $componentColumns = @('Component', 'ComponentId', 'Directory_', 'Attributes', 'Condition', 'KeyPath')
    $fileColumns = @('File', 'Component_', 'FileName', 'FileSize', 'Version', 'Language', 'Attributes', 'Sequence')
    $sequence = 0
    foreach ($f in $files) {
        $sequence++
        Insert $db 'Component' $componentColumns @($f.Component, (NameGuid "file:$($f.Name)"), $f.Directory, 256, $null, $f.Key)  # 256: 64-bit
        Insert $db 'FeatureComponents' @('Feature_', 'Component_') @('MainFeature', $f.Component)
        Insert $db 'File' $fileColumns @($f.Key, $f.Component, $f.Name, [int](Get-Item -LiteralPath $f.Source).Length, $f.Version, $null, 512, $sequence)  # 512: vital
    }
    Insert $db 'Media' @('DiskId', 'LastSequence', 'DiskPrompt', 'Cabinet', 'VolumeLabel', 'Source') @(1, $sequence, $null, '#data.cab', $null, $null)

    # Shortcuts, each in a component of its own with a registry value as key path (per machine).
    # The desktop one only when chosen on the "Ready" page.
    $registryKey = 'Software\KVC\WSA-Next Setup'
    Insert $db 'Component' $componentColumns @('CompStartMenu', (NameGuid 'shortcut:startmenu'), 'ProgramMenuFolder', 260, $null, 'RegStartMenu')  # 4: registry key path
    Insert $db 'Component' $componentColumns @('CompDesktop', (NameGuid 'shortcut:desktop'), 'DesktopFolder', 260, 'DESKTOPSHORTCUT = "1"', 'RegDesktop')
    foreach ($c in 'CompStartMenu', 'CompDesktop') { Insert $db 'FeatureComponents' @('Feature_', 'Component_') @('MainFeature', $c) }
    Insert $db 'Registry' @('Registry', 'Root', 'Key', 'Name', 'Value', 'Component_') @('RegStartMenu', -1, $registryKey, 'StartMenuShortcut', '1', 'CompStartMenu')
    Insert $db 'Registry' @('Registry', 'Root', 'Key', 'Name', 'Value', 'Component_') @('RegDesktop', -1, $registryKey, 'DesktopShortcut', '1', 'CompDesktop')
    $shortcutColumns = @('Shortcut', 'Directory_', 'Name', 'Component_', 'Target', 'Arguments', 'Description', 'Hotkey', 'Icon_', 'IconIndex', 'ShowCmd', 'WkDir')
    Insert $db 'Shortcut' $shortcutColumns @('StartMenuShortcut', 'ProgramMenuFolder', 'WSA-Next', 'CompStartMenu', '[#WsaNextExe]', $null, $en.ShortcutDesc, $null, 'WsaNextIcon.ico', 0, 1, 'INSTALLDIR')
    Insert $db 'Shortcut' $shortcutColumns @('DesktopShortcut', 'DesktopFolder', 'WSA-Next', 'CompDesktop', '[#WsaNextExe]', $null, $en.ShortcutDesc, $null, 'WsaNextIcon.ico', 0, 1, 'INSTALLDIR')

    Insert $db 'Icon' @('Name', 'Data') @('WsaNextIcon.ico', [IO.FileInfo]$iconPath)
    Insert $db 'Binary' @('Name', 'Data') @('DialogBmp', [IO.FileInfo]$dialogBmp)
    Insert $db 'Binary' @('Name', 'Data') @('BannerBmp', [IO.FileInfo]$bannerBmp)

    # The WSA-Next.exe already installed, if any (earlier version or a copy written by WSA-Next itself).
    Insert $db 'AppSearch' @('Property', 'Signature_') @('EXISTINGEXE', 'ExistingExe')
    Insert $db 'Signature' @('Signature', 'FileName', 'MinVersion', 'MaxVersion', 'MinSize', 'MaxSize', 'MinDate', 'MaxDate', 'Languages') @('ExistingExe', 'WSA-Next.exe', $null, $null, $null, $null, $null, $null, $null)
    Insert $db 'DrLocator' @('Signature_', 'Parent', 'Path', 'Depth') @('ExistingExe', $null, '[ProgramFiles64Folder]WSA-Next', 0)

    # Custom actions. Type bits: 34 an exe with a working folder, 51 set a property; +64 ignore the
    # exit code, +192 do not wait, +1024 deferred (in the script), +512 commit (after success),
    # +2048 as LocalSystem. Impersonated actions get the user's limited token when Setup was started
    # without elevation, so WSA-Next's own work runs as LocalSystem (app/SetupActions.h).
    $caColumns = @('Action', 'Type', 'Source', 'Target', 'ExtendedType')
    Insert $db 'CustomAction' $caColumns @('SetArpInstallLocation', 51, 'ARPINSTALLLOCATION', '[INSTALLDIR]', $null)
    # The background host of every signed-in user holds WSA-Next.exe open: it stops first.
    Insert $db 'CustomAction' $caColumns @('StopHosts', (34 + 64 + 1024 + 2048), 'INSTALLDIR', '"[EXISTINGEXE]" msi stop-hosts', $null)
    # Removing Setup removes WSA-Next: the WSA package, its disks and the host's logon task go, as
    # the user who removes Setup (that user's full token, no second UAC prompt). WSA-Next.exe and its
    # language files stay for Windows Installer to remove.
    Insert $db 'CustomAction' $caColumns @('RemoveWsaNext', (34 + 64 + 1024 + 2048), 'INSTALLDIR', '"[EXISTINGEXE]" msi uninstall [UserSID]', $null)
    Insert $db 'CustomAction' $caColumns @('RemoveWsaNextAll', (34 + 64 + 1024 + 2048), 'INSTALLDIR', '"[EXISTINGEXE]" msi uninstall [UserSID] --all', $null)
    # After an install, repair or update: the host's logon task (made by WSA-Next) starts it again.
    Insert $db 'CustomAction' $caColumns @('StartHost', (34 + 64 + 512 + 1024 + 2048), 'INSTALLDIR', '"[System64Folder]schtasks.exe" /Run /TN "WSA-Next"', $null)
    Insert $db 'CustomAction' $caColumns @('LaunchApp', (34 + 192), 'INSTALLDIR', '"[INSTALLDIR]WSA-Next.exe"', $null)
    # What the progress page says while they run.
    $actionTexts = [ordered]@{ StopHosts = 'ActionStopHosts'; RemoveWsaNext = 'ActionRemoveWsa'; RemoveWsaNextAll = 'ActionRemoveWsa' }
    foreach ($kv in $actionTexts.GetEnumerator()) { Insert $db 'ActionText' @('Action', 'Description', 'Template') @($kv.Key, $en[$kv.Value], $null) }

    $removing = 'REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE AND EXISTINGEXE'
    $execute = @(
        @('LaunchConditions', $null, 100), @('FindRelatedProducts', $null, 200), @('AppSearch', $null, 400),
        @('CostInitialize', $null, 800), @('FileCost', $null, 900), @('CostFinalize', $null, 1000),
        @('SetArpInstallLocation', $null, 1010),
        @('InstallValidate', $null, 1400), @('InstallInitialize', $null, 1500),
        @('RemoveExistingProducts', $null, 1505),
        @('StopHosts', 'EXISTINGEXE', 1510),
        @('RemoveWsaNext', "$removing AND NOT REMOVEDATA", 1520),
        @('RemoveWsaNextAll', "$removing AND REMOVEDATA", 1521),
        @('ProcessComponents', $null, 1600), @('UnpublishFeatures', $null, 1800), @('RemoveRegistryValues', $null, 2600),
        @('RemoveShortcuts', $null, 3200), @('RemoveFiles', $null, 3500), @('RemoveFolders', $null, 3600), @('CreateFolders', $null, 3700),
        @('InstallFiles', $null, 4000), @('CreateShortcuts', $null, 4500), @('WriteRegistryValues', $null, 5000),
        @('RegisterUser', $null, 6000), @('RegisterProduct', $null, 6100), @('PublishFeatures', $null, 6300), @('PublishProduct', $null, 6400),
        @('StartHost', 'NOT REMOVE~="ALL"', 6590), @('InstallFinalize', $null, 6600)
    )
    foreach ($a in $execute) { Insert $db 'InstallExecuteSequence' @('Action', 'Condition', 'Sequence') @($a[0], $a[1], $a[2]) }
    $ui = @(
        @('LaunchConditions', $null, 100), @('FindRelatedProducts', $null, 200), @('AppSearch', $null, 400),
        @('CostInitialize', $null, 800), @('FileCost', $null, 900), @('CostFinalize', $null, 1000),
        @('MaintenanceDlg', 'Installed AND NOT RESUME AND NOT Preselected', 1250),
        @('WelcomeDlg', 'NOT Installed', 1260),
        @('ProgressDlg', $null, 1280), @('ExecuteAction', $null, 1300),
        @('ExitDlg', $null, -1), @('UserExitDlg', $null, -2), @('FatalErrorDlg', $null, -3)
    )
    foreach ($a in $ui) { Insert $db 'InstallUISequence' @('Action', 'Condition', 'Sequence') @($a[0], $a[1], $a[2]) }

    # --- The wizard -----------------------------------------------------------------------------
    # 370 x 270 units; welcome / finish pages use the full picture, the others the banner.
    $V = 1; $E = 2; $Sunken = 4; $Transparent = 0x10000; $NoPrefix = 0x20000; $Shield = 0x800000; $Progress95 = 0x10000
    $label = $V + $E + $Transparent + $NoPrefix     # visible text on a picture
    $hiddenLabel = $E + $Transparent + $NoPrefix    # shown by a ControlCondition
    $controls = [Collections.Generic.List[object]]::new()   # Key: the string in msi-strings.json
    $events = [Collections.Generic.List[object]]::new()
    $conditions = [Collections.Generic.List[object]]::new()
    $mappings = [Collections.Generic.List[object]]::new()
    function Ctl($dialog, $name, $type, $x, $y, $w, $h, $attributes, $property, $key, $prefix, $literal, $next) {
        $controls.Add([pscustomobject]@{ Dialog = $dialog; Name = $name; Type = $type; X = $x; Y = $y; W = $w; H = $h; A = $attributes; Property = $property; Key = $key; Prefix = $prefix; Literal = $literal; Next = $next })
    }
    function Evt($dialog, $control, $event, $argument, $condition, $order) { $events.Add(@($dialog, $control, $event, $argument, $condition, $order)) }
    function Cnd($dialog, $control, $action, $condition) { $conditions.Add(@($dialog, $control, $action, $condition)) }
    function Banner($dialog, $titleKey, $descriptionKey) {
        Ctl $dialog 'BannerBitmap' 'Bitmap' 0 0 370 44 $V $null $null $null 'BannerBmp' $null
        Ctl $dialog 'BannerLine' 'Line' 0 44 370 0 $V $null $null $null $null $null
        if ($titleKey) { Ctl $dialog 'Title' 'Text' 15 8 290 15 $label $null $titleKey '{\FontTitle}' $null $null }
        if ($descriptionKey) { Ctl $dialog 'Description' 'Text' 25 24 280 16 $label $null $descriptionKey $null $null $null }
    }
    function Bottom($dialog) { Ctl $dialog 'BottomLine' 'Line' 0 234 370 0 $V $null $null $null $null $null }

    # Welcome
    Ctl 'WelcomeDlg' 'Bitmap' 'Bitmap' 0 0 370 234 $V $null $null $null 'DialogBmp' $null
    Ctl 'WelcomeDlg' 'Title' 'Text' 135 20 220 40 $label $null 'WelcomeTitle' '{\FontHero}' $null $null
    Ctl 'WelcomeDlg' 'Description' 'Text' 135 70 220 140 $label $null 'WelcomeText' $null $null $null
    Bottom 'WelcomeDlg'
    Ctl 'WelcomeDlg' 'Back' 'PushButton' 180 243 56 17 $V $null 'Back' $null $null $null
    Ctl 'WelcomeDlg' 'Next' 'PushButton' 236 243 56 17 ($V + $E) $null 'Next' $null $null 'Cancel'
    Ctl 'WelcomeDlg' 'Cancel' 'PushButton' 304 243 56 17 ($V + $E) $null 'Cancel' $null $null 'Next'
    Evt 'WelcomeDlg' 'Next' 'NewDialog' 'LicenseDlg' '1' 1
    Evt 'WelcomeDlg' 'Cancel' 'SpawnDialog' 'CancelDlg' '1' 1

    # License
    Banner 'LicenseDlg' 'LicenseTitle' 'LicenseDesc'
    Ctl 'LicenseDlg' 'LicenseText' 'ScrollableText' 20 52 330 150 ($V + $E + $Sunken) $null $null $null $rtf 'Accept'
    Ctl 'LicenseDlg' 'Accept' 'CheckBox' 20 208 330 15 ($V + $E) 'LicenseAccepted' 'LicenseAccept' $null $null 'Back'
    Bottom 'LicenseDlg'
    Ctl 'LicenseDlg' 'Back' 'PushButton' 180 243 56 17 ($V + $E) $null 'Back' $null $null 'Next'
    Ctl 'LicenseDlg' 'Next' 'PushButton' 236 243 56 17 ($V + $E) $null 'Next' $null $null 'Cancel'
    Ctl 'LicenseDlg' 'Cancel' 'PushButton' 304 243 56 17 ($V + $E) $null 'Cancel' $null $null 'LicenseText'
    Evt 'LicenseDlg' 'Back' 'NewDialog' 'WelcomeDlg' '1' 1
    Evt 'LicenseDlg' 'Next' 'NewDialog' 'ReadyDlg' 'LicenseAccepted = "1"' 1
    Evt 'LicenseDlg' 'Cancel' 'SpawnDialog' 'CancelDlg' '1' 1
    Cnd 'LicenseDlg' 'Next' 'Disable' 'LicenseAccepted <> "1"'
    Cnd 'LicenseDlg' 'Next' 'Enable' 'LicenseAccepted = "1"'

    # Ready
    Banner 'ReadyDlg' 'ReadyTitle' 'ReadyDesc'
    Ctl 'ReadyDlg' 'Text' 'Text' 25 58 320 70 ($V + $E + $NoPrefix) $null 'ReadyText' $null $null $null
    Ctl 'ReadyDlg' 'Desktop' 'CheckBox' 25 140 320 15 ($V + $E) 'DESKTOPSHORTCUT' 'DesktopShortcut' $null $null 'Back'
    Bottom 'ReadyDlg'
    Ctl 'ReadyDlg' 'Back' 'PushButton' 180 243 56 17 ($V + $E) $null 'Back' $null $null 'Install'
    Ctl 'ReadyDlg' 'Install' 'PushButton' 236 243 56 17 ($V + $E + $Shield) $null 'Install' $null $null 'Cancel'
    Ctl 'ReadyDlg' 'Cancel' 'PushButton' 304 243 56 17 ($V + $E) $null 'Cancel' $null $null 'Desktop'
    Evt 'ReadyDlg' 'Back' 'NewDialog' 'LicenseDlg' '1' 1
    Evt 'ReadyDlg' 'Install' 'EndDialog' 'Return' '1' 1
    Evt 'ReadyDlg' 'Cancel' 'SpawnDialog' 'CancelDlg' '1' 1

    # Progress (modeless): the title for install, repair or removal
    Banner 'ProgressDlg' $null 'ProgressText'
    Ctl 'ProgressDlg' 'TitleInstall' 'Text' 15 8 290 15 $hiddenLabel $null 'ProgressInstall' '{\FontTitle}' $null $null
    Ctl 'ProgressDlg' 'TitleRepair' 'Text' 15 8 290 15 $hiddenLabel $null 'ProgressRepair' '{\FontTitle}' $null $null
    Ctl 'ProgressDlg' 'TitleRemove' 'Text' 15 8 290 15 $hiddenLabel $null 'ProgressRemove' '{\FontTitle}' $null $null
    Ctl 'ProgressDlg' 'StatusLabel' 'Text' 25 68 60 10 ($V + $E + $NoPrefix) $null 'StatusLabel' $null $null $null
    Ctl 'ProgressDlg' 'ActionText' 'Text' 85 68 265 10 ($V + $E + $NoPrefix) $null $null $null $null $null
    Ctl 'ProgressDlg' 'ProgressBar' 'ProgressBar' 25 84 325 12 ($V + $Progress95) $null $null $null 'Progress' $null
    Bottom 'ProgressDlg'
    Ctl 'ProgressDlg' 'Back' 'PushButton' 180 243 56 17 $V $null 'Back' $null $null $null
    Ctl 'ProgressDlg' 'Next' 'PushButton' 236 243 56 17 $V $null 'Next' $null $null $null
    Ctl 'ProgressDlg' 'Cancel' 'PushButton' 304 243 56 17 ($V + $E) $null 'Cancel' $null $null $null
    Evt 'ProgressDlg' 'Cancel' 'SpawnDialog' 'CancelDlg' '1' 1
    Cnd 'ProgressDlg' 'TitleInstall' 'Show' 'NOT Installed'
    Cnd 'ProgressDlg' 'TitleRepair' 'Show' 'Installed AND NOT REMOVE~="ALL"'
    Cnd 'ProgressDlg' 'TitleRemove' 'Show' 'REMOVE~="ALL"'
    $mappings.Add(@('ProgressDlg', 'ActionText', 'ActionText', 'Text'))
    $mappings.Add(@('ProgressDlg', 'ProgressBar', 'SetProgress', 'Progress'))

    # Finish, interrupted, failed: the full picture; "start WSA-Next" sits next to the buttons
    foreach ($d in 'ExitDlg', 'UserExitDlg', 'FatalErrorDlg') {
        Ctl $d 'Bitmap' 'Bitmap' 0 0 370 234 $V $null $null $null 'DialogBmp' $null
        Bottom $d
        Ctl $d 'Back' 'PushButton' 180 243 56 17 $V $null 'Back' $null $null $null
        Ctl $d 'Cancel' 'PushButton' 304 243 56 17 $V $null 'Cancel' $null $null $null
    }
    Ctl 'ExitDlg' 'TitleInstall' 'Text' 135 20 220 40 $hiddenLabel $null 'ExitInstall' '{\FontHero}' $null $null
    Ctl 'ExitDlg' 'TitleRepair' 'Text' 135 20 220 40 $hiddenLabel $null 'ExitRepair' '{\FontHero}' $null $null
    Ctl 'ExitDlg' 'TitleRemove' 'Text' 135 20 220 40 $hiddenLabel $null 'ExitRemove' '{\FontHero}' $null $null
    Ctl 'ExitDlg' 'Description' 'Text' 135 70 220 100 $hiddenLabel $null 'ExitText' $null $null $null
    Ctl 'ExitDlg' 'Launch' 'CheckBox' 12 244 160 15 $E 'LAUNCHAPP' 'LaunchApp' $null $null 'Finish'
    Ctl 'ExitDlg' 'Finish' 'PushButton' 236 243 56 17 ($V + $E) $null 'Finish' $null $null 'Launch'
    Cnd 'ExitDlg' 'TitleInstall' 'Show' 'NOT Installed'
    Cnd 'ExitDlg' 'TitleRepair' 'Show' 'Installed AND NOT REMOVE~="ALL"'
    Cnd 'ExitDlg' 'TitleRemove' 'Show' 'REMOVE~="ALL"'
    Cnd 'ExitDlg' 'Description' 'Show' 'NOT Installed'
    Cnd 'ExitDlg' 'Launch' 'Show' 'NOT Installed'
    Evt 'ExitDlg' 'Finish' 'DoAction' 'LaunchApp' 'LAUNCHAPP = "1" AND NOT Installed' 1
    Evt 'ExitDlg' 'Finish' 'EndDialog' 'Return' '1' 2
    Ctl 'UserExitDlg' 'Title' 'Text' 135 20 220 40 $label $null 'UserExitTitle' '{\FontHero}' $null $null
    Ctl 'UserExitDlg' 'Description' 'Text' 135 70 220 100 $label $null 'UserExitText' $null $null $null
    Ctl 'UserExitDlg' 'Finish' 'PushButton' 236 243 56 17 ($V + $E) $null 'Finish' $null $null $null
    Evt 'UserExitDlg' 'Finish' 'EndDialog' 'Exit' '1' 1
    Ctl 'FatalErrorDlg' 'Title' 'Text' 135 20 220 40 $label $null 'FatalTitle' '{\FontHero}' $null $null
    Ctl 'FatalErrorDlg' 'Description' 'Text' 135 70 220 100 $label $null 'FatalText' $null $null $null
    Ctl 'FatalErrorDlg' 'Finish' 'PushButton' 236 243 56 17 ($V + $E) $null 'Finish' $null $null $null
    Evt 'FatalErrorDlg' 'Finish' 'EndDialog' 'Exit' '1' 1

    # Maintenance: repair, or remove (optionally with the Android apps and their data)
    Banner 'MaintenanceDlg' 'MaintTitle' 'MaintDesc'
    Ctl 'MaintenanceDlg' 'Repair' 'PushButton' 25 62 80 17 ($V + $E + $Shield) $null 'Repair' $null $null 'Remove'
    Ctl 'MaintenanceDlg' 'RepairText' 'Text' 115 62 235 30 ($V + $E + $NoPrefix) $null 'RepairText' $null $null $null
    Ctl 'MaintenanceDlg' 'Remove' 'PushButton' 25 108 80 17 ($V + $E + $Shield) $null 'Remove' $null $null 'RemoveData'
    Ctl 'MaintenanceDlg' 'RemoveText' 'Text' 115 108 235 40 ($V + $E + $NoPrefix) $null 'RemoveText' $null $null $null
    Ctl 'MaintenanceDlg' 'RemoveData' 'CheckBox' 115 152 235 15 ($V + $E) 'REMOVEDATA' 'RemoveData' $null $null 'Cancel'
    Bottom 'MaintenanceDlg'
    Ctl 'MaintenanceDlg' 'Back' 'PushButton' 180 243 56 17 $V $null 'Back' $null $null $null
    Ctl 'MaintenanceDlg' 'Cancel' 'PushButton' 304 243 56 17 ($V + $E) $null 'Cancel' $null $null 'Repair'
    Evt 'MaintenanceDlg' 'Repair' 'ReinstallMode' 'ecmus' '1' 1
    Evt 'MaintenanceDlg' 'Repair' 'Reinstall' 'ALL' '1' 2
    Evt 'MaintenanceDlg' 'Repair' 'EndDialog' 'Return' '1' 3
    Evt 'MaintenanceDlg' 'Remove' 'Remove' 'ALL' '1' 1
    Evt 'MaintenanceDlg' 'Remove' 'EndDialog' 'Return' '1' 2
    Evt 'MaintenanceDlg' 'Cancel' 'SpawnDialog' 'CancelDlg' '1' 1

    # Cancel confirmation
    Ctl 'CancelDlg' 'Text' 'Text' 20 15 220 30 ($V + $E + $NoPrefix) $null 'CancelText' $null $null $null
    Ctl 'CancelDlg' 'Yes' 'PushButton' 72 57 56 17 ($V + $E) $null 'Yes' $null $null 'No'
    Ctl 'CancelDlg' 'No' 'PushButton' 132 57 56 17 ($V + $E) $null 'No' $null $null 'Yes'
    Evt 'CancelDlg' 'Yes' 'EndDialog' 'Exit' '1' 1
    Evt 'CancelDlg' 'No' 'EndDialog' 'Return' '1' 1

    # Windows Installer's error messages (ErrorDialog): the text and the buttons it asks for
    Ctl 'ErrorDlg' 'ErrorText' 'Text' 20 10 230 60 ($V + $E + $NoPrefix) $null $null $null $null $null
    $errorButtons = @(@('A', 'Abort', 'ErrorAbort'), @('C', 'Cancel', 'ErrorCancel'), @('I', 'Ignore', 'ErrorIgnore'), @('N', 'No', 'ErrorNo'),
                      @('O', 'OK', 'ErrorOk'), @('R', 'Retry', 'ErrorRetry'), @('Y', 'Yes', 'ErrorYes'))
    for ($i = 0; $i -lt $errorButtons.Count; $i++) {
        $b = $errorButtons[$i]
        Ctl 'ErrorDlg' $b[0] 'PushButton' 100 80 56 17 ($V + $E) $null $b[1] $null $null $null
        Evt 'ErrorDlg' $b[0] 'EndDialog' $b[2] '1' 1
    }

    $dialogs = @(
        @('WelcomeDlg', 370, 270, 3, 'Next', 'Next', 'Cancel'),
        @('LicenseDlg', 370, 270, 3, 'LicenseText', 'Next', 'Cancel'),
        @('ReadyDlg', 370, 270, 3, 'Install', 'Install', 'Cancel'),
        @('ProgressDlg', 370, 270, 1, 'Cancel', 'Cancel', 'Cancel'),
        @('ExitDlg', 370, 270, 3, 'Finish', 'Finish', 'Finish'),
        @('UserExitDlg', 370, 270, 3, 'Finish', 'Finish', 'Finish'),
        @('FatalErrorDlg', 370, 270, 3, 'Finish', 'Finish', 'Finish'),
        @('MaintenanceDlg', 370, 270, 3, 'Repair', 'Repair', 'Cancel'),
        @('CancelDlg', 260, 85, 3, 'No', 'No', 'No'),
        @('ErrorDlg', 270, 105, 65539, 'O', $null, $null)  # 65536: the error dialog
    )
    foreach ($d in $dialogs) {
        Insert $db 'Dialog' @('Dialog', 'HCentering', 'VCentering', 'Width', 'Height', 'Attributes', 'Title', 'Control_First', 'Control_Default', 'Control_Cancel') @($d[0], 50, 50, $d[1], $d[2], $d[3], $en.SetupTitle, $d[4], $d[5], $d[6])
    }
    foreach ($c in $controls) {
        $value = if ($c.Key) { [string]$c.Prefix + $en[$c.Key] } else { $c.Literal }
        Insert $db 'Control' @('Dialog_', 'Control', 'Type', 'X', 'Y', 'Width', 'Height', 'Attributes', 'Property', 'Text', 'Control_Next', 'Help') @($c.Dialog, $c.Name, $c.Type, $c.X, $c.Y, $c.W, $c.H, $c.A, $c.Property, $value, $c.Next, $null)
    }
    foreach ($e in $events) { Insert $db 'ControlEvent' @('Dialog_', 'Control_', 'Event', 'Argument', 'Condition', 'Ordering') $e }
    foreach ($c in $conditions) { Insert $db 'ControlCondition' @('Dialog_', 'Control_', 'Action', 'Condition') $c }
    foreach ($m in $mappings) { Insert $db 'EventMapping' @('Dialog_', 'Control_', 'Event', 'Attribute') $m }
    foreach ($p in 'LicenseAccepted', 'DESKTOPSHORTCUT', 'LAUNCHAPP', 'REMOVEDATA') { Insert $db 'CheckBox' @('Property', 'Value') @($p, '1') }
    Insert $db 'TextStyle' @('TextStyle', 'FaceName', 'Size', 'Color', 'StyleBits') @('FontNormal', 'Segoe UI', 9, $null, 0)
    Insert $db 'TextStyle' @('TextStyle', 'FaceName', 'Size', 'Color', 'StyleBits') @('FontTitle', 'Segoe UI', 9, $null, 1)
    Insert $db 'TextStyle' @('TextStyle', 'FaceName', 'Size', 'Color', 'StyleBits') @('FontHero', 'Segoe UI', 13, $null, 1)

    # The cabinet and the summary
    Sql $db 'INSERT INTO `_Streams` (`Name`, `Data`) VALUES (?, ?)' @('data.cab', [IO.FileInfo]$cabPath)
    $summary = Inv $db 'SummaryInformation' $Get @([int]20)
    $summaryValues = [ordered]@{
        1 = [int]1252; 2 = 'Installation Database'; 3 = 'WSA-Next'; 4 = $manufacturerAscii; 5 = 'Installer, WSA-Next, Windows Subsystem for Android, Android 15'
        6 = 'WSA-Next: Android 15 for the Windows Subsystem for Android. https://github.com/wesmar/wsa-next'; 7 = 'x64;1033'; 9 = (NewGuid)
        12 = [datetime]::Now; 13 = [datetime]::Now; 14 = [int]500; 15 = [int]2; 18 = 'WSA-Next build_msi.ps1 (Windows Installer COM)'; 19 = [int]2
    }
    foreach ($kv in $summaryValues.GetEnumerator()) { [void](Inv $summary 'Property' $Set @([int]$kv.Key, $kv.Value)) }
    [void](Inv $summary 'Persist' $Method $null)
    Free $summary
    [void](Inv $db 'Commit' $Method $null)
    Free $db; $db = $null

    # --- One transform per language, embedded --------------------------------------------------
    $templateLanguages = [Collections.Generic.List[int]]::new()
    $templateLanguages.Add(1033)
    $localizedControls = $controls | Where-Object { $_.Key }
    foreach ($lang in ($text.languages.Keys | Sort-Object)) {
        $info = $text.languages[$lang]
        $strings = $text.strings[$lang]
        if (-not $strings) { throw "No strings for $lang in msi-strings.json." }
        $codePage = [int]$info.codepage
        $s = { param([string]$key) ForCodePage $strings[$key] $codePage $info.replace "$lang/$key" }
        $copy = Join-Path $stage "$lang.msi"
        Copy-Item -LiteralPath $msiPath -Destination $copy -Force
        $ldb = Inv $installer 'OpenDatabase' $Method @($copy, [int]1)  # transacted
        Set-Content -LiteralPath (Join-Path $stage 'codepage.idt') -Value "`r`n`r`n$codePage`t_ForceCodepage`r`n" -NoNewline -Encoding ASCII
        [void](Inv $ldb 'Import' $Method @($stage, 'codepage.idt'))
        foreach ($c in $localizedControls) {
            Sql $ldb 'UPDATE `Control` SET `Text` = ? WHERE `Dialog_` = ? AND `Control` = ?' @(([string]$c.Prefix + (& $s $c.Key)), $c.Dialog, $c.Name)
        }
        foreach ($d in $dialogs) { Sql $ldb 'UPDATE `Dialog` SET `Title` = ? WHERE `Dialog` = ?' @((& $s 'SetupTitle'), $d[0]) }
        $lcid = [int]$info.lcids[0]
        $localManufacturer = try { ForCodePage $manufacturer $codePage $null 'Manufacturer' } catch { $manufacturerAscii }
        Sql $ldb 'UPDATE `Property` SET `Value` = ? WHERE `Property` = ?' @("$lcid", 'ProductLanguage')
        Sql $ldb 'UPDATE `Property` SET `Value` = ? WHERE `Property` = ?' @($localManufacturer, 'Manufacturer')
        Sql $ldb 'UPDATE `Property` SET `Value` = ? WHERE `Property` = ?' @((& $s 'ShortcutDesc'), 'ARPCOMMENTS')
        Sql $ldb 'UPDATE `LaunchCondition` SET `Description` = ? WHERE `Condition` = ?' @((& $s 'Need64'), 'VersionNT64')
        Sql $ldb 'UPDATE `LaunchCondition` SET `Description` = ? WHERE `Condition` = ?' @((& $s 'NewerInstalled'), 'NOT NEWERVERSIONDETECTED OR Installed')
        foreach ($sc in 'StartMenuShortcut', 'DesktopShortcut') { Sql $ldb 'UPDATE `Shortcut` SET `Description` = ? WHERE `Shortcut` = ?' @((& $s 'ShortcutDesc'), $sc) }
        foreach ($kv in $actionTexts.GetEnumerator()) { Sql $ldb 'UPDATE `ActionText` SET `Description` = ? WHERE `Action` = ?' @((& $s $kv.Value), $kv.Key) }
        if ($info.rtl) {
            # Right to left: text, check boxes and the dialogs themselves (32 + 64 + 128 per control,
            # 128 + 256 + 512 per dialog). The license stays English, left to right.
            foreach ($c in ($controls | Where-Object { $_.Type -in 'Text', 'CheckBox' })) {
                Sql $ldb 'UPDATE `Control` SET `Attributes` = ? WHERE `Dialog_` = ? AND `Control` = ?' @([int]($c.A -bor 0xE0), $c.Dialog, $c.Name)
            }
            foreach ($d in $dialogs) { Sql $ldb 'UPDATE `Dialog` SET `Attributes` = ? WHERE `Dialog` = ?' @([int]($d[3] -bor 0x380), $d[0]) }
        }
        [void](Inv $ldb 'Commit' $Method $null)
        $reference = Inv $installer 'OpenDatabase' $Method @($msiPath, [int]0)
        $mst = Join-Path $stage "$lang.mst"
        [void](Inv $ldb 'GenerateTransform' $Method @($reference, $mst))
        [void](Inv $ldb 'CreateTransformSummaryInfo' $Method @($reference, $mst, [int]0, [int]0))
        Free $reference; Free $ldb
        $base = Inv $installer 'OpenDatabase' $Method @($msiPath, [int]1)
        foreach ($id in $info.lcids) {
            Sql $base 'INSERT INTO `_Storages` (`Name`, `Data`) VALUES (?, ?)' @("$id", [IO.FileInfo]$mst)
            $templateLanguages.Add([int]$id)
        }
        [void](Inv $base 'Commit' $Method $null)
        Free $base
        Write-Host ("  {0,-6} code page {1,-4} -> {2}" -f $lang, $codePage, (($info.lcids | ForEach-Object { $_ }) -join ', '))
    }
    $base = Inv $installer 'OpenDatabase' $Method @($msiPath, [int]1)
    $summary = Inv $base 'SummaryInformation' $Get @([int]1)
    [void](Inv $summary 'Property' $Set @([int]7, ('x64;' + ($templateLanguages -join ','))))
    [void](Inv $summary 'Persist' $Method $null)
    Free $summary
    [void](Inv $base 'Commit' $Method $null)
    Free $base
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()

    # --- Signature, the zip, the result ---------------------------------------------------------
    if ($SignScript) {
        if (-not (Test-Path -LiteralPath $SignScript)) { throw "SignScript not found: $SignScript" }
        & $SignScript -File $msiPath | Out-Host
    }
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    $zipStage = Join-Path $stage 'zip'
    New-Item -ItemType Directory -Path (Join-Path $zipStage 'language') -Force | Out-Null
    Copy-Item -LiteralPath $exePath, $licensePath -Destination $zipStage
    foreach ($lng in $lngFiles) { Copy-Item -LiteralPath $lng.FullName -Destination (Join-Path $zipStage 'language') }
    Compress-Archive -Path (Join-Path $zipStage '*') -DestinationPath $zipPath -CompressionLevel Optimal

    foreach ($out in $msiPath, $zipPath) {
        $item = Get-Item -LiteralPath $out
        $signature = if ($out -eq $msiPath) { (Get-AuthenticodeSignature -LiteralPath $out).Status } else { '-' }
        Write-Host ('{0,-14} {1,8:N2} MB  SHA-256 {2}  signature {3}' -f $item.Name, ($item.Length / 1MB), (Get-FileHash -LiteralPath $out).Hash, $signature) -ForegroundColor Green
    }
}
finally {
    if ($db) { Free $db }
    Free $installer
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
