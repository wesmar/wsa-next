# WSA-Next © 2026 Marek Wesołowski - wesmar
<#
.SYNOPSIS
    Deploy freshly built system images into an installed (registered) WSA 2407 package
    for a test boot, keeping every replaced file.

.DESCRIPTION
    1. Stops WSA.
    2. Moves the current system/system_ext/product/vendor disks, kernel and initrd of the
       package into a time-stamped backup folder (nothing is deleted).
    3. Copies the new images (and optionally a vendor disk, kernel and initrd) in.
    4. With -FreshData, renames the per-user data disks in LocalCache so Android starts
       with empty userdata (the renamed files stay next to the originals).
    5. Starts WSA through the wsa:// protocol and waits for adb to report boot completed.

.EXAMPLE
    .\deploy-test.ps1 -ImagesDir \\wsl.localhost\Ubuntu-26.04\home\me\aosp15\out\target\product\wsa_x86_64 `
        -VendorVhdx C:\Projekty\WSA-Next-inputs\wsa-2407-a13-pkg\vendor.vhdx -FreshData
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $ImagesDir,
    [string] $WsaDir = 'C:\wsa',
    [string] $BackupRoot = 'C:\backup\wsa-deploy',
    [string] $VendorVhdx,
    [string] $Kernel,
    [string] $Initrd,
    [switch] $FreshData,
    [int] $BootTimeoutSec = 180
)
$ErrorActionPreference = 'Stop'

$pkgFamily = 'MicrosoftCorporationII.WindowsSubsystemForAndroid_8wekyb3d8bbwe'
$localCache = Join-Path $env:LOCALAPPDATA "Packages\$pkgFamily\LocalCache"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $BackupRoot $stamp

function Stop-Wsa {
    Get-Process WsaClient -ErrorAction SilentlyContinue | Stop-Process -Force
    Stop-Service WsaService -Force -ErrorAction SilentlyContinue
    # The VM can outlive the service briefly; wait until its worker process is gone.
    for ($i = 0; $i -lt 30 -and (Get-Process vmmemWSA -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Seconds 1
    }
}

function Find-Adb {
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
    if (Test-Path $sdk) { return $sdk }
    throw 'adb not found'
}

Write-Host "Stopping WSA"
Stop-Wsa

New-Item -ItemType Directory -Force $backup | Out-Null
$plan = @(
    @{ Src = Join-Path $ImagesDir 'system.vhdx';     Dst = Join-Path $WsaDir 'system.vhdx' },
    @{ Src = Join-Path $ImagesDir 'system_ext.vhdx'; Dst = Join-Path $WsaDir 'system_ext.vhdx' },
    @{ Src = Join-Path $ImagesDir 'product.vhdx';    Dst = Join-Path $WsaDir 'product.vhdx' }
)
if ($VendorVhdx) { $plan += @{ Src = $VendorVhdx; Dst = Join-Path $WsaDir 'vendor.vhdx' } }
if ($Kernel)     { $plan += @{ Src = $Kernel;     Dst = Join-Path $WsaDir 'Tools\kernel' } }
if ($Initrd)     { $plan += @{ Src = $Initrd;     Dst = Join-Path $WsaDir 'Tools\initrd.img' } }

foreach ($item in $plan) {
    if (-not (Test-Path $item.Src)) { throw "Missing input: $($item.Src)" }
}
foreach ($item in $plan) {
    if (Test-Path $item.Dst) {
        Move-Item $item.Dst (Join-Path $backup (Split-Path $item.Dst -Leaf))
    }
    Copy-Item $item.Src $item.Dst
    Write-Host ("  {0,-16} <- {1}" -f (Split-Path $item.Dst -Leaf), $item.Src)
}
Write-Host "Previous files saved in $backup"

if ($FreshData) {
    foreach ($name in 'userdata.2.vhdx', 'metadata.2.vhdx') {
        $path = Join-Path $localCache $name
        if (Test-Path $path) {
            Rename-Item $path "$name.before-$stamp"
            Write-Host "  LocalCache\$name -> $name.before-$stamp"
        }
    }
}

Write-Host "Starting WSA"
Start-Process 'wsa://com.android.settings'

$adb = Find-Adb
$serial = '127.0.0.1:58526'
$deadline = (Get-Date).AddSeconds($BootTimeoutSec)
$booted = $false
while ((Get-Date) -lt $deadline) {
    & $adb connect $serial *> $null
    $state = (& $adb -s $serial shell getprop sys.boot_completed 2>$null) -join ''
    if ($state.Trim() -eq '1') { $booted = $true; break }
    Start-Sleep -Seconds 3
}

$logDir = Join-Path $backup 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
if ($booted) {
    Write-Host "Boot completed"
    & $adb -s $serial shell getprop | Out-File -Encoding utf8 (Join-Path $logDir 'getprop.txt')
    & $adb -s $serial logcat -d -b all | Out-File -Encoding utf8 (Join-Path $logDir 'logcat.txt')
    & $adb -s $serial shell dmesg 2>$null | Out-File -Encoding utf8 (Join-Path $logDir 'dmesg.txt')
    & $adb -s $serial shell getprop ro.build.fingerprint

    # Health summary: one line per WSA integration point, plus crash and SELinux counts.
    $log = Get-Content (Join-Path $logDir 'logcat.txt') -ErrorAction SilentlyContinue
    $dmesg = Get-Content (Join-Path $logDir 'dmesg.txt') -ErrorAction SilentlyContinue
    $checks = [ordered]@{
        'fstab SCSI addresses resolved'   = 'Resolved SCSI address'
        'WsaSystemService started'        = 'WSA System Service started'
        'IWsaSystemServer registered'     = 'WsaSystemServerService service added'
        'WsaLaunchParamsModifier active'  = 'Registering WsaLaunchParamsModifier'
        'WSA display area policy'         = 'Creating WsaDisplayArea'
        'WSA SystemUI initializer'        = 'WsaSystemUIInitializer: Initializing'
        'Notification redirection ready'  = 'WsaSystemUIInitializer.*Initialize complete'
        'WsaStatusBar registered'         = 'StartServices com.microsoft.windows.statusbar.WsaStatusBar'
    }
    Write-Host ''
    Write-Host 'Health summary'
    foreach ($name in $checks.Keys) {
        $hit = @($log + $dmesg | Select-String -Pattern $checks[$name] -Quiet)
        Write-Host ("  [{0}] {1}" -f ($(if ($hit -contains $true) { 'ok' } else { '--' })), $name)
    }
    $fatal = @($log | Select-String 'FATAL EXCEPTION').Count
    $avc = @($log + $dmesg | Select-String 'avc:  denied').Count
    $enforce = (& $adb -s $serial shell getenforce 2>$null) -join ''
    Write-Host ("  FATAL EXCEPTION: {0}   avc denied: {1}   SELinux: {2}" -f $fatal, $avc, $enforce.Trim())
} else {
    Write-Warning "No boot_completed within $BootTimeoutSec s"
    $diag = Join-Path $env:LOCALAPPDATA "Packages\$pkgFamily\LocalState\diagnostics"
    if (Test-Path $diag) { Copy-Item $diag $logDir -Recurse -Force }
}
Write-Host "Logs: $logDir"
