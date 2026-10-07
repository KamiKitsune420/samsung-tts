# Build the oracle for an arm64 Android device (or an x86_64 emulator image that translates arm64).
#
#   powershell -ExecutionPolicy Bypass -File tools\oracle\build.ps1
#
# Needs the Android NDK; set ANDROID_NDK_HOME to pick a particular one. Writes tools\oracle\oracle.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) {
    $sdk = $env:ANDROID_HOME
    if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk" }
    $root = Join-Path $sdk "ndk"
    if (-not (Test-Path $root)) { throw "no NDK: install one, or set ANDROID_NDK_HOME" }
    $ndk = (Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
}
$clang = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android28-clang.cmd"
if (-not (Test-Path $clang)) { throw "not found: $clang" }
& $clang -O2 -Wall "-Wl,--export-dynamic" (Join-Path $here "oracle.c") -o (Join-Path $here "oracle") -ldl
if ($LASTEXITCODE -ne 0) { throw "clang failed" }
Write-Output "built: $(Join-Path $here 'oracle')"
