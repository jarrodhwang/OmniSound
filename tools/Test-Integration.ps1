param([switch]$SilentAudio, [switch]$Snapshot)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root 'build\windows-x64\Release'
$probe = Join-Path $bin 'OmniSoundProbe.exe'
$evidence = Join-Path $root 'build\validation'
New-Item -ItemType Directory -Force $evidence | Out-Null
if (Get-Process -Name OmniSoundEngine -ErrorAction SilentlyContinue) {
    throw 'An OmniSound engine is already running. Leave it intact; close it before running this lifecycle test.'
}
$result = [ordered]@{ utc=[DateTime]::UtcNow.ToString('o'); engine=$null; silentAudio=@(); uiSnapshot=$null }
$process = Start-Process -FilePath (Join-Path $bin 'OmniSoundEngine.exe') -WindowStyle Hidden -PassThru
try {
    $status = (& $probe --status | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $process.HasExited) { throw 'Owned engine failed to expose its control pipe.' }
    $result.engine = $status
    & $probe --stop-engine | Out-Null
    if ($LASTEXITCODE -ne 0 -or !$process.WaitForExit(7000) -or $process.ExitCode -ne 0) {
        throw 'Engine did not complete a clean control-pipe shutdown.'
    }
} finally {
    if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.Dispose()
}
if ($SilentAudio) {
    $devices = (& $probe --enumerate | ConvertFrom-Json)
    if ($LASTEXITCODE) { throw 'Endpoint enumeration failed.' }
    foreach ($device in $devices.render | Where-Object { $_.active -and !$_.groupId }) {
        $output = (& $probe --loopback-smoke $device.id | ConvertFrom-Json)
        if ($LASTEXITCODE) { throw "Silent loopback or resume failed: $($device.name)" }
        $result.silentAudio += $output
    }
}
if ($Snapshot) {
    $ui = Start-Process -FilePath (Join-Path $bin 'OmniSound.exe') -ArgumentList '--smoke-test','--snapshot' -WindowStyle Hidden -PassThru
    if (!$ui.WaitForExit(10000)) { Stop-Process -Id $ui.Id -Force; throw 'UI smoke test timed out.' }
    if ($ui.ExitCode) { throw 'UI smoke test failed.' }
    $result.uiSnapshot = 'build/windows-x64/ui-smoke.png (demo data, not a physical sync measurement)'
    $ui.Dispose()
}
$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $evidence 'host-smoke.json') -Encoding utf8
Write-Output ($result | ConvertTo-Json -Depth 12)
