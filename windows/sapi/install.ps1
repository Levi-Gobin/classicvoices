# Installs the Classic Voices SAPI 5 engine for every program on this PC:
# cvsapi64.dll for 64-bit programs and cvsapi32.dll for 32-bit ones, copied to
# Program Files\Classic Voices and registered with regsvr32 (as administrator).
# On Windows on Arm the x64 DLL serves x64 programs and ARM64EC ones (NVDA).
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-DataDir <folder>]
#
# DataDir is the folder with Voices\, Downloads\ and Native\ (the NVDA add-on's
# layout). Default: the NVDA add-on's own data, if you have it, else
# %ProgramData%\Classic Voices. Run it again after adding voices: the voice list
# is read when the engine is registered.
param([string]$DataDir = "")
$ErrorActionPreference = "Stop"

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "Run this from an administrator PowerShell (it registers the voices for all programs)." }

if (-not $DataDir) {
    $nvda = Join-Path $env:APPDATA "nvda\classicvoices"
    $DataDir = if (Test-Path (Join-Path $nvda "Voices")) { $nvda } else { Join-Path $env:ProgramData "Classic Voices" }
}
$DataDir = [IO.Path]::GetFullPath($DataDir)
if (-not (Test-Path (Join-Path $DataDir "Voices"))) {
    throw "No Voices folder in $DataDir. Import firmware with the NVDA add-on first, or pass -DataDir."
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dest = Join-Path $env:ProgramFiles "Classic Voices"
New-Item -ItemType Directory -Force $dest | Out-Null

$windir = $env:SystemRoot
$is64 = [Environment]::Is64BitOperatingSystem
# (file to copy, name to install it as, regsvr32 to register it with)
$files = @()
if ($is64) {
    $files += , @("cvsapi64.dll", "cvsapi64.dll", (Join-Path $windir "System32\regsvr32.exe"))
}
$files += , @("cvsapi32.dll", "cvsapi32.dll", (Join-Path $windir ($(if ($is64) { "SysWOW64" } else { "System32" }) + "\regsvr32.exe")))
foreach ($f in $files) {
    $from, $name, $regsvr = $f
    $target = Join-Path $dest $name
    # A running program may hold the old DLL: move it aside rather than fail.
    if (Test-Path $target) {
        try { Remove-Item $target -Force } catch { Move-Item $target "$target.old-$(Get-Random)" -Force }
    }
    Copy-Item (Join-Path $here $from) $target
}
foreach ($f in $files) {
    $from, $name, $regsvr = $f
    if (-not $regsvr) { continue }
    $target = Join-Path $dest $name
    $p = Start-Process -PassThru -Wait $regsvr -ArgumentList "/s", "/n", "/i:`"$DataDir`"", "`"$target`""
    if ($p.ExitCode -ne 0) { throw "regsvr32 failed for $name (exit $($p.ExitCode))" }
}
Get-ChildItem $dest -Filter "*.old-*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

$count = (Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens" | Where-Object PSChildName -like "ClassicVoices_*").Count
Write-Host "Classic Voices: $count voices registered for SAPI programs, from $DataDir."
if ($count -eq 0) { Write-Host "No voices were found there; check the folder and run this again." }
