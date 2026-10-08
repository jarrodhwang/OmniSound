param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$probe = Join-Path $root 'build\windows-x64\Release\OmniSoundProbe.exe'
$evidence = Join-Path $root 'build\validation\lite-desktop.json'
$restoreFile = Join-Path $root 'build\validation\lite-desktop-restore.json'
$fixtureFile = Join-Path $root 'build\validation\lite-desktop-fixture.json'
function ProbeJson([string[]]$Arguments) {
    $reply = & $probe @Arguments
    if ($LASTEXITCODE) { throw "Probe failed: $($Arguments -join ' ')" }
    return ($reply | ConvertFrom-Json)
}
function Wait-Group([string]$Id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $state = (ProbeJson @('--lite-status')).engine
        $active = @($state.groups | Where-Object id -eq $Id)
        if ($state.activeGroupId -eq $Id -and $state.routerCount -eq 1 -and
            $active.Count -eq 1 -and $active[0].state -in @('starting','buffering','playing')) { return $state }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Group did not become the sole ready router: $Id"
}
function Import-Config($Configuration,[string]$File) {
    $Configuration | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $File -Encoding utf8
    return (ProbeJson @('--lite-import',$File)).config
}
$original = (ProbeJson @('--lite-config')).config
$original | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $restoreFile -Encoding utf8
$initial = (ProbeJson @('--lite-status')).engine
$startEnabled = $initial.startWithWindows
$sourceId = @($initial.groups | Where-Object sourceEndpointId)[0].sourceEndpointId
$sourceVolume = @(ProbeJson @('--volume-status') | Where-Object endpointId -eq $sourceId)[0].percent
if ([Math]::Abs($sourceVolume - 50) -gt 0.5) { throw 'This parity acceptance fixture expects the existing source to be at 50%; it does not change it.' }
$outputs = @((ProbeJson @('--enumerate')).render | Where-Object { $_.active -and !$_.groupId -and $_.id -ne $sourceId })
$dell = @($outputs | Where-Object name -like 'DELL S2725DS*')[0]
$other = @($outputs | Where-Object id -ne $dell.id)[0]
if (!$dell -or !$other) { throw 'This fixture needs a second DELL output and another active physical output.' }
$firstId = [Guid]::NewGuid().ToString()
$secondId = [Guid]::NewGuid().ToString()
$fixture = [ordered]@{
    version=1; revision=$original.revision; activeGroupId=$firstId
    groups=@(
        @{id=$firstId; name='Desktop acceptance A'; safetyMs=200; members=@(
            @{endpointId=$dell.id; gain=1.0; delayMs=0; followReferenceVolume=$true},
            @{endpointId=$other.id; gain=1.0; delayMs=0; followReferenceVolume=$true})},
        @{id=$secondId; name='Desktop acceptance B'; safetyMs=200; members=@(
            @{endpointId=$dell.id; gain=1.0; delayMs=0; followReferenceVolume=$false})}
    )
}
$result = [ordered]@{acousticMeasurement=$false; microphoneUsed=$false; sourcePercent=$sourceVolume; tests=@()}
try {
    Import-Config $fixture $fixtureFile | Out-Null
    $result.firstGroup = Wait-Group $firstId
    $inactive = @($result.firstGroup.groups | Where-Object id -eq $secondId)[0]
    if ($inactive.state -ne 'inactive' -or @($inactive.members).Count -ne 0) { throw 'Inactive group opened outputs.' }
    $result.follow50 = ProbeJson @('--mirror-smoke')
    $parity = @($result.follow50.outputs | Where-Object endpointId -eq $dell.id)[0].ratioToReference
    if (!$result.follow50.passed -or [Math]::Abs($parity - 1) -gt 0.025) { throw "Source/destination 50% parity failed: $parity" }
    $result.tests += '50% source / 50% destination without duplicate attenuation'
    $levels = @(ProbeJson @('--volume-status'))
    foreach ($id in @($dell.id,$other.id)) {
        if ([Math]::Abs(@($levels | Where-Object endpointId -eq $id)[0].percent - 50) -gt 0.5) { throw 'Follow did not set Windows 50%.' }
    }
    ProbeJson @('--activate',$secondId) | Out-Null
    $result.secondGroup = Wait-Group $secondId
    $inactive = @($result.secondGroup.groups | Where-Object id -eq $firstId)[0]
    if ($inactive.state -ne 'inactive' -or @($inactive.members).Count -ne 0) { throw 'Previous group kept output streams.' }
    $levels = @(ProbeJson @('--volume-status'))
    if ([Math]::Abs(@($levels | Where-Object endpointId -eq $dell.id)[0].percent - 100) -gt 0.5) { throw 'Second group did not apply its own Windows 100%.' }
    $result.independent100 = ProbeJson @('--mirror-smoke')
    $result.tests += 'Group switch closes previous router and applies shared-device volume'
    ProbeJson @('--stop-lite') | Out-Null
    $engine = Get-Process OmniSoundEngine -ErrorAction SilentlyContinue | Where-Object Path -eq (Join-Path $env:LOCALAPPDATA 'Programs\OmniSoundLite\OmniSoundEngine.exe')
    foreach ($process in $engine) { if (!$process.WaitForExit(5000)) { throw 'Engine did not shut down.' } }
    Start-ScheduledTask -TaskName 'OmniSound Lite Engine'
    Start-Sleep -Milliseconds 500
    $result.afterRestart = Wait-Group $secondId
    $result.tests += 'Active group survives installed login-task restart'
    ProbeJson @('--activate',$firstId) | Out-Null
    $result.returnedGroup = Wait-Group $firstId
    $result.returnedFollow50 = ProbeJson @('--mirror-smoke')
    $parity = @($result.returnedFollow50.outputs | Where-Object endpointId -eq $dell.id)[0].ratioToReference
    if ([Math]::Abs($parity - 1) -gt 0.025) { throw 'Returning group did not restore the shared-device 50% setting.' }
    $result.tests += 'Returning to a group restores its destination volume'
    $result.startupOff = ProbeJson @('--startup','off')
    if ($result.startupOff.enabled -or $result.startupOff.launchCommand -or (ProbeJson @('--lite-status')).engine.startWithWindows) { throw 'Startup disable failed.' }
    $result.startupOn = ProbeJson @('--startup','on')
    if (!$result.startupOn.enabled -or $result.startupOn.launchCommand -notlike '*OmniSound.exe*--background*' -or !(ProbeJson @('--lite-status')).engine.startWithWindows) { throw 'Startup enable failed.' }
    $result.tests += 'Startup preference and background UI registration'
    $process = @(Get-Process OmniSoundEngine | Where-Object Path -eq (Join-Path $env:LOCALAPPDATA 'Programs\OmniSoundLite\OmniSoundEngine.exe'))[0]
    $cpuBefore = $process.TotalProcessorTime.TotalMilliseconds
    $start = [DateTime]::UtcNow
    Start-Sleep -Seconds 3
    $process.Refresh()
    $elapsed = ([DateTime]::UtcNow - $start).TotalMilliseconds
    $result.shortSample = @{elapsedMs=$elapsed; cpuPercentOfOneCore=100*($process.TotalProcessorTime.TotalMilliseconds-$cpuBefore)/$elapsed; workingSetBytes=$process.WorkingSet64; longRun=$false}
    $result.passed = $true
} finally {
    ProbeJson @('--lite-import',$restoreFile) | Out-Null
    ProbeJson @('--startup',$(if ($startEnabled) {'on'} else {'off'})) | Out-Null
    $result.restoredUserConfiguration = $true
    $result | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $evidence -Encoding utf8
}
$result.tests | ForEach-Object { "PASS $_" }
"Evidence: $evidence"
