# Build the stress tests into work\engine.
#
#   powershell -ExecutionPolicy Bypass -File tests\build.ps1
#
# Run build.ps1 in the repository root first: the soak programs link the engine's objects, and sapi_stress.exe
# loads dist\SamsungTTS\samsungtts_sapi.dll. Then, with voice packs unpacked under work\voice, for example:
#
#   the engine, for half an hour, three voices:
#     work\engine\soak.exe work\voice tests\corpus\soak.txt --minutes 30 --rates --voices en_US_l03,en_GB_l02,ko_KR_l01
#   every line of a corpus once with each voice:
#     work\engine\soak.exe work\voice tests\corpus\tokens.txt --sweep --voices hi_IN_f00,it_IT_l01
#   the SAPI DLL, three callers at once:
#     work\engine\sapi_stress.exe dist\SamsungTTS\samsungtts_sapi.dll work\voice tests\corpus\soak.txt --minutes 30 --threads 3 --voices en_US_l03,en_GB_l02,ko_KR_l01
#   the engine faulting on purpose (its two known bugs with their guards off; a host stack overflow):
#     work\engine\soak_guards_off.exe work\voice tests\corpus\faults.txt --minutes 5 --switch 3 --voices hi_IN_f00,en_US_l03,it_IT_l01
#     work\engine\soak_overflow.exe work\voice tests\corpus\plain.txt --count 60 --stops 0 --switch 2 --voices it_IT_l01,en_US_l03
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
foreach ($t in @("soak", "engine\guards.c"), @("soak_guards_off", "tests\guards_off.c"), @("soak_overflow", "tests\guards_overflow.c")) {
    python tools\a2c\build.py work\engine "work\engine\$($t[0]).exe" tests\soak.c engine\sstts.c engine\fast.c $t[1] --stubs --jobs 3
    if ($LASTEXITCODE -ne 0) { throw "$($t[0]).exe did not build" }
}
$vs = $env:VS
if (-not $vs) {
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vw -latest -products * -property installationPath
}
$clang = Join-Path $vs "VC\Tools\Llvm\x64\bin\clang-cl.exe"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$cmd = "`"$vcvars`" >nul 2>nul && `"$clang`" /nologo /O2 /W3 /MT /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS --target=x86_64-pc-windows-msvc " +
       "tests\sapi_stress.cpp /Fowork\engine\obj\sapi_stress.obj /Fework\engine\sapi_stress.exe"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "sapi_stress.exe did not build" }
"built soak.exe, soak_guards_off.exe, soak_overflow.exe and sapi_stress.exe in work\engine"
