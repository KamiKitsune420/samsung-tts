# Build everything into dist\: the SAPI voice and the voice centre (dist\SamsungTTS), the driver DLL
# (dist\SamsungTTS-Driver) and the engine's command-line program (work\engine\engine.exe).
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Jobs 3] [-Translate] [-Installer] [-Package]
#
# Needs, under work\ (which git ignores, because it is Samsung's):
#   work\l03            voice pack com.samsung.SMT.lang_en_us_l03 3.1.25.4, unpacked (its libsamsungtts.so is translated)
#   work\sys\libm.so    an arm64 Android libm.so (from a device or emulator image: /apex/com.android.runtime/lib64/bionic)
# and Python 3 with capstone, and Visual Studio's clang-cl. Both files go into the programs whole, beside their
# translated code, so nothing built here needs them at run time. -Translate runs the translation again (it runs
# by itself the first time). -Jobs is the number of compilers at once: a generated file can take a compiler past
# 1 GB. -Installer also compiles installer\samsungtts.iss with Inno Setup into dist\SamsungTTS-Setup-<version>.exe.
# -Package also makes dist\SamsungTTS-Driver-<version>.zip: the driver DLL, its header, documentation and example,
# and the source as of the last commit.
param([int]$Jobs = 3, [switch]$Translate, [switch]$Installer, [switch]$Package)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$so = "work\l03\lib\arm64-v8a\libsamsungtts.so"; $libm = "work\sys\libm.so"
foreach ($f in $so, $libm) { if (-not (Test-Path $f)) { throw "missing $f (see the top of build.ps1)" } }
function Run { & python @args; if ($LASTEXITCODE -ne 0) { throw "failed: python $args" } }

if ($Translate -or -not (Test-Path "work\engine\gen_images.c")) { Run tools\translate_engine.py }
$dist = "dist\SamsungTTS"; $driver = "dist\SamsungTTS-Driver"
New-Item -ItemType Directory -Force "$dist\data\voice", $driver | Out-Null
# the programs share the objects of the generated code in work\engine\obj
$engine = "engine\sstts.c", "engine\fast.c", "engine\guards.c"
Run tools\a2c\build.py work\engine work\engine\engine.exe engine\main.c @engine --stubs --jobs $Jobs
Run tools\a2c\build.py work\engine "$dist\samsungtts_sapi.dll" @engine sapi\samsung_sapi.cpp --stubs --jobs $Jobs
Run tools\a2c\build.py work\engine "$driver\samsungtts.dll" @engine driver\samsungtts.c --stubs --jobs $Jobs
Copy-Item driver\samsungtts.h, driver\README.md, driver\example.py, licenses\bionic-libm-NOTICE.txt $driver -Force
& powershell -ExecutionPolicy Bypass -File center\build.ps1
if ($LASTEXITCODE -ne 0) { throw "the voice centre did not build" }
if ($Installer) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "D:\Program Files\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found" }
    & $iscc /Q installer\samsungtts.iss
    if ($LASTEXITCODE -ne 0) { throw "the installer did not compile" }
}
if ($Package) {
    $version = (Select-String -Path installer\samsungtts.iss -Pattern '#define AppVersion "(.+)"').Matches[0].Groups[1].Value
    $stage = "work\package"
    if (Test-Path $stage) { [IO.Directory]::Delete((Resolve-Path $stage), $true) }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item "$driver\samsungtts.dll", "$driver\samsungtts.lib", "$driver\samsungtts.h", "$driver\README.md", "$driver\example.py", "$driver\bionic-libm-NOTICE.txt" $stage
    git archive --format=zip -o "$stage\src.zip" HEAD
    if ($LASTEXITCODE -ne 0) { throw "git archive failed: the source goes in as of the last commit, so there has to be one" }
    $zip = "dist\SamsungTTS-Driver-$version.zip"
    if (Test-Path $zip) { [IO.File]::Delete((Resolve-Path $zip)) }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    "packaged $zip"
}
"built $dist and $driver"
