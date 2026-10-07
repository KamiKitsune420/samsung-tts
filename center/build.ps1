# Build the voice centre into dist\SamsungTTS\SamsungVoices.exe, beside the SAPI DLL it manages.
#
#   powershell -ExecutionPolicy Bypass -File center\build.ps1
#
# Needs Visual Studio's clang-cl; set VS to the Build Tools folder if vswhere does not find it.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$vs = $env:VS
if (-not $vs) {
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vw -latest -products * -property installationPath
}
$clang = Join-Path $vs "VC\Tools\Llvm\x64\bin\clang-cl.exe"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$out = Join-Path $root "dist\SamsungTTS"
New-Item -ItemType Directory -Force $out | Out-Null
$obj = Join-Path $root "work\center_obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$cmd = "`"$vcvars`" >nul 2>nul && `"$clang`" /nologo /O2 /W3 /MT /EHsc /std:c++17 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS --target=x86_64-pc-windows-msvc " +
       "`"$here\voice_center.cpp`" /Fo`"$obj\\`" /Fe`"$out\SamsungVoices.exe`" /link /SUBSYSTEM:WINDOWS"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"built $out\SamsungVoices.exe"
