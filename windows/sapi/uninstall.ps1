# Removes the Classic Voices SAPI 5 engine: unregisters both DLLs (their voices
# disappear from every program) and deletes Program Files\Classic Voices. The
# voice data folder is left alone.
#
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1
$ErrorActionPreference = "Stop"
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "Run this from an administrator PowerShell." }

$dest = Join-Path $env:ProgramFiles "Classic Voices"
$windir = $env:SystemRoot
$jobs = if ([Environment]::Is64BitOperatingSystem) {
    @(@("cvsapi64.dll", "System32"), @("cvsapi32.dll", "SysWOW64"))
} else {
    @(, @("cvsapi32.dll", "System32"))
}
foreach ($j in $jobs) {
    $dll, $sys = $j
    $target = Join-Path $dest $dll
    if (Test-Path $target) {
        Start-Process -Wait (Join-Path $windir "$sys\regsvr32.exe") -ArgumentList "/s", "/u", "`"$target`""
    }
}
Remove-Item -Recurse -Force "HKLM:\SOFTWARE\Classic Voices" -ErrorAction SilentlyContinue
# A program still using a voice keeps its DLL open until it exits.
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
if (Test-Path $dest) { Write-Host "Some files are in use; they go once the programs using them close (or after a restart)." }
Write-Host "Classic Voices SAPI voices removed."
