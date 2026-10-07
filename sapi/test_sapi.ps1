# Speak a corpus through the registered Samsung SAPI voice into WAV files, one per line, so the result can be
# compared with the engine's own output (tools\compare_runs.py reads the WAVs).
#
#   powershell -ExecutionPolicy Bypass -File sapi\test_sapi.ps1 <corpus.txt> <outdir> [-Voice Stephanie] [-Rate 0] [-StopAfterMs 0]
#
# -Voice picks one of the registered Samsung voices by name (Stephanie, Julia, Lisa, "John Lano", "Amy Green").
# -StopAfterMs speaks asynchronously to the sound card and purges after that long, reporting how long the stop took.
param([string]$Corpus, [string]$OutDir, [int]$Rate = 0, [int]$StopAfterMs = 0, [string]$Voice = "Stephanie")
$ErrorActionPreference = "Stop"
$v = New-Object -ComObject SAPI.SpVoice
$tok = @($v.GetVoices()) | Where-Object { $_.GetDescription() -like "Samsung $Voice*" } | Select-Object -First 1
if (-not $tok) { throw "no SAPI voice named Samsung $Voice; register the voices first (sapi\install.ps1)" }
$v.Voice = $tok
$v.Rate = $Rate
if ($StopAfterMs -gt 0) {
    $text = (Get-Content $Corpus -Encoding UTF8 | Where-Object { $_.Trim() } | Select-Object -First 1)
    [void]$v.Speak($text, 1)                      # SVSFlagsAsync
    Start-Sleep -Milliseconds $StopAfterMs
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [void]$v.Speak("", 3)                         # async + purge before speak
    [void]$v.WaitUntilDone(5000)
    "stopped in {0:N0} ms" -f $sw.Elapsed.TotalMilliseconds
    return
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$n = 0
$total = [Diagnostics.Stopwatch]::StartNew()
foreach ($line in (Get-Content $Corpus -Encoding UTF8)) {
    if (-not $line.Trim()) { continue }
    $fs = New-Object -ComObject SAPI.SpFileStream
    $fs.Format.Type = 26                          # SAFT24kHz16BitMono: the engine's own format, so nothing is resampled
    $path = Join-Path (Resolve-Path $OutDir) ("{0:D5}.wav" -f $n)
    $fs.Open($path, 3, $false)
    $v.AudioOutputStream = $fs
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [void]$v.Speak($line)
    $fs.Close()
    "{0}: {1:N2} s" -f $n, $sw.Elapsed.TotalSeconds
    $n++
}
"{0} utterances in {1:N2} s" -f $n, $total.Elapsed.TotalSeconds
