# Does a voice installed while a program already has the engine loaded speak with its own voice?
#
#   powershell -ExecutionPolicy Bypass -File sapi\test_live_install.ps1
#
# In one process: register voice A for the current user from the development copy (dist\SamsungTTS), speak with
# it through SAPI, then add voice B and register again without restarting, and speak with B. Each result is
# compared with what the engine's command-line driver gives for that voice. The per-user registration is removed
# again at the end. Needs the voice packs under work\voice and no administrator rights.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$dll = Join-Path $root "dist\SamsungTTS\samsungtts_sapi.dll"
$data = Join-Path $root "dist\SamsungTTS\data\voice"
$A = "en_GB_l02"; $B = "en_US_l04"
$text = "The quick brown fox jumps over the lazy dog."
$out = Join-Path $root "work\live_install"
New-Item -ItemType Directory -Force $out, $data | Out-Null
Set-Content -Path "$out\text.txt" -Value $text -Encoding ASCII

function Register { $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$dll`"" -Wait -PassThru; if ($p.ExitCode) { throw "regsvr32 failed" } }
function Speak($code, $file) {
    $tok = New-Object -ComObject SAPI.SpObjectToken
    $tok.SetId("HKEY_CURRENT_USER\SOFTWARE\Microsoft\Speech\Voices\Tokens\SamsungTTS_$code")
    $script:voice.Voice = $tok
    $fs = New-Object -ComObject SAPI.SpFileStream
    $fs.Format.Type = 26
    $fs.Open($file, 3, $false)
    $script:voice.AudioOutputStream = $fs
    [void]$script:voice.Speak($text)
    $fs.Close()
}
function Reference($code) {
    $d = Join-Path $out "ref_$code"
    New-Item -ItemType Directory -Force $d | Out-Null
    # through cmd, so that the driver's progress on stderr is not taken for an error
    cmd /c "`"$root\work\engine\engine.exe`" `"$root\work\voice`" `"$out\text.txt`" `"$d`" --fast --voice $code --end-silence 100 >nul 2>nul"
    return [IO.File]::ReadAllBytes("$d\00000.pcm")
}
function Same($wav, $pcm) {
    # the samples are the end of the WAV file; SAPI's header is not always 44 bytes
    $w = [IO.File]::ReadAllBytes($wav)
    $off = $w.Length - $pcm.Length
    if ($off -lt 44 -or $off -gt 128) { return $false }
    for ($i = 0; $i -lt $pcm.Length; $i++) { if ($w[$i + $off] -ne $pcm[$i]) { return $false } }
    return $true
}

try {
    Copy-Item "$root\work\voice\$A" $data -Recurse -Force
    Register
    $script:voice = New-Object -ComObject SAPI.SpVoice
    Speak $A "$out\a.wav"
    "voice A ($A) through SAPI matches the engine's own output for A: " + (Same "$out\a.wav" (Reference $A))

    Copy-Item "$root\work\voice\$B" $data -Recurse -Force      # install B while this process keeps the engine loaded
    Register
    Speak $B "$out\b.wav"
    $refB = Reference $B
    "voice B ($B), installed without restarting, matches the engine's own output for B: " + (Same "$out\b.wav" $refB)
    $wa = [IO.File]::ReadAllBytes("$out\a.wav"); $wb = [IO.File]::ReadAllBytes("$out\b.wav")
    "voice B's file is the same as voice A's (the bug would look like this): " + ([Convert]::ToBase64String($wa) -eq [Convert]::ToBase64String($wb))
} finally {
    Start-Process regsvr32.exe -ArgumentList '/s', '/u', "`"$dll`"" -Wait | Out-Null
    foreach ($c in $A, $B) { if (Test-Path "$data\$c") { [IO.Directory]::Delete("$data\$c", $true) } }
}
