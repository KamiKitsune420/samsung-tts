# Register the Samsung SAPI voice machine-wide, so that it appears in every program's list of SAPI voices.
# Needs administrator rights; if this window is not elevated it asks for them.
#
#   powershell -ExecutionPolicy Bypass -File sapi\install.ps1            register
#   powershell -ExecutionPolicy Bypass -File sapi\install.ps1 -Remove    unregister
#
# The DLL is dist\SamsungTTS\samsungtts_sapi.dll, with the voice packs in dist\SamsungTTS\data\voice beside it.
# The voice is 64-bit: 32-bit programs will not list it.
param([switch]$Remove)
$dll = Join-Path (Split-Path -Parent $PSScriptRoot) "dist\SamsungTTS\samsungtts_sapi.dll"
if (-not (Test-Path $dll)) { throw "not built: $dll" }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$regArgs = @("/s")
if ($Remove) { $regArgs += "/u" }
$regArgs += "`"$dll`""
if ($admin) {
    $p = Start-Process regsvr32.exe -ArgumentList $regArgs -Wait -PassThru
} else {
    $p = Start-Process regsvr32.exe -ArgumentList $regArgs -Wait -PassThru -Verb RunAs
}
if ($p.ExitCode -ne 0) { throw "regsvr32 failed with $($p.ExitCode)" }
if ($Remove) { "Samsung voice unregistered." } else { "Samsung voice registered. Restart programs that should see it." }
