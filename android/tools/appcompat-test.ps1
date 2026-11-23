# WSA-Next © 2026 Marek Wesołowski - wesmar
<#
.SYNOPSIS
    Launch installed Android apps on WSA one by one and report how each one runs.

.DESCRIPTION
    For every package (all third-party packages by default, or -Packages) the script
    force-stops the app, launches it through its launcher activity, waits, and records:
      * whether the process is still alive and a Windows window for it is visible,
      * Java crashes (FATAL EXCEPTION) and native crashes (Fatal signal) during the run,
      * the app's primary ABI and how its native code runs (native x86, Google
        ndk_translation, Houdini).
    The result is printed as a Markdown table and saved next to the script's logs.
    Needs adb with root (userdebug build) to read /proc/<pid>/maps.

.EXAMPLE
    .\appcompat-test.ps1 -WaitSec 20
    .\appcompat-test.ps1 -Packages com.microsoft.emmx,com.whatsapp
#>
[CmdletBinding()]
param(
    [string[]] $Packages,
    [int] $WaitSec = 15,
    [string] $Serial = '127.0.0.1:58526',
    [string] $OutDir = 'C:\backup\wsa-appcompat'
)
$ErrorActionPreference = 'Stop'

function Find-Adb {
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
    if (Test-Path $sdk) { return $sdk }
    throw 'adb not found'
}
$adb = Find-Adb
function Invoke-Shell([string] $cmd) { (& $adb -s $Serial shell $cmd 2>$null) -join "`n" }

Add-Type -Namespace WsaNext -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, System.IntPtr l);
public delegate bool EnumWindowsProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint p);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
'@ -ErrorAction SilentlyContinue

function Test-WindowsWindow([string] $package) {
    $ids = @(Get-Process WsaClient -ErrorAction SilentlyContinue | ForEach-Object Id)
    $script:found = $false
    $cb = [WsaNext.Win+EnumWindowsProc] {
        param($h, $l)
        $p = 0; [void][WsaNext.Win]::GetWindowThreadProcessId($h, [ref]$p)
        if ($ids -contains $p -and [WsaNext.Win]::IsWindowVisible($h)) {
            $cn = New-Object System.Text.StringBuilder 256
            [void][WsaNext.Win]::GetClassName($h, $cn, 256)
            if ($cn.ToString() -eq $package) { $script:found = $true; return $false }
        }
        $true
    }
    [void][WsaNext.Win]::EnumWindows($cb, [IntPtr]::Zero)
    $script:found
}

& $adb connect $Serial *> $null
& $adb -s $Serial root *> $null
Start-Sleep 2
& $adb connect $Serial *> $null

if (-not $Packages) {
    $Packages = (Invoke-Shell 'pm list packages -3') -split "`n" |
        ForEach-Object { $_.Trim() -replace '^package:', '' } | Where-Object { $_ }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logDir = Join-Path $OutDir $stamp
New-Item -ItemType Directory -Force $logDir | Out-Null

$rows = foreach ($pkg in $Packages) {
    $abi = (Invoke-Shell "dumpsys package $pkg | grep -m1 primaryCpuAbi") -replace '.*=', ''
    $ver = (Invoke-Shell "dumpsys package $pkg | grep -m1 versionName") -replace '.*=', ''
    Invoke-Shell "am force-stop $pkg; logcat -c; logcat -b crash -c" | Out-Null
    Invoke-Shell "monkey -p $pkg -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1" | Out-Null
    Start-Sleep -Seconds $WaitSec
    $procPid = (Invoke-Shell "pidof $pkg").Trim()
    $maps = if ($procPid) { Invoke-Shell "cat /proc/$($procPid.Split(' ')[0])/maps" } else { '' }
    # Every app process maps the native bridge library at fork time, so the ABI decides
    # first; for ARM apps the guest linker (arm64 or Houdini's arm) shows which one runs.
    $runs = if ($abi -match 'x86') { 'native' }
            elseif ($abi -match 'null|^$') { 'Java only' }
            elseif ($maps -match '/system/bin/arm64/linker64|/system/lib64/arm64/') { 'ndk_translation' }
            elseif ($maps -match 'houdini|/arm/') { 'Houdini' }
            elseif (-not $procPid) { 'not running' } else { '?' }
    $javaCrash = (Invoke-Shell "logcat -d -b crash | grep -c 'FATAL EXCEPTION'").Trim()
    $nativeCrash = (Invoke-Shell "logcat -d | grep -c 'Fatal signal'").Trim()
    $window = Test-WindowsWindow $pkg
    Invoke-Shell 'logcat -d' | Out-File -Encoding utf8 (Join-Path $logDir "$pkg.log")
    $state = if ($procPid -and $window -and $javaCrash -eq '0' -and $nativeCrash -eq '0') { 'works' }
             elseif ($procPid -and $javaCrash -eq '0' -and $nativeCrash -eq '0') { 'runs, no window' }
             else { 'crashes' }
    [pscustomobject]@{ Package = $pkg; Version = $ver; ABI = $abi; Runs = $runs; State = $state
                       JavaCrashes = $javaCrash; NativeCrashes = $nativeCrash }
    Invoke-Shell "am force-stop $pkg" | Out-Null
}

$md = @('| Package | Version | ABI | Native code | State |', '|---|---|---|---|---|')
$md += $rows | ForEach-Object { "| $($_.Package) | $($_.Version) | $($_.ABI) | $($_.Runs) | $($_.State) |" }
$md | Set-Content -Encoding utf8 (Join-Path $logDir 'appcompat.md')
$rows | Format-Table -AutoSize | Out-String -Width 220
"Report: $(Join-Path $logDir 'appcompat.md')"
