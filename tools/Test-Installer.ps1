param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$identity = [Guid]::NewGuid().ToString('N')
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA "Programs\OmniSound-Installer-Test\$identity"))
$fixtureData = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA "OmniSound\installer-tests\$identity"))
$fixtureKey = "HKCU:\SOFTWARE\OmniSound\InstallerTests\$identity"
$fixtureTask = "OmniSound Installer Test $identity"
$fixtureShortcut = Join-Path $fixtureData 'Start Menu.lnk'
$scriptText = Get-Content -LiteralPath (Join-Path $root 'installer\Install.ps1') -Raw
function Set-TestDefault([string]$Variable,[string]$Value) {
    $replacement = '$' + $Variable + " = '" + $Value.Replace("'","''") + "'"
    $script:scriptText = [Text.RegularExpressions.Regex]::Replace($script:scriptText,
        ('(?m)^\$' + $Variable + ' = .*\r?$'), [Text.RegularExpressions.MatchEvaluator]{ param($match) $replacement })
}
# Substitute only deployment destinations, leaving the production backend's
# preflight, file replacement, backup and rollback code intact. All mutations
# belong to this fixture. Never start its engine against real user settings.
Set-TestDefault target $fixtureRoot
Set-TestDefault data $fixtureData
Set-TestDefault uninstallKey ($fixtureKey + '\Uninstall')
Set-TestDefault taskName $fixtureTask
Set-TestDefault shortcut $fixtureShortcut
Set-TestDefault runKey ($fixtureKey + '\Run')
Set-TestDefault startupPreferenceKey ($fixtureKey + '\Desktop')
# The native runner uses a dedicated PowerShell process. In this harness an
# uninstall returns to its assertions instead of exiting the test process.
$scriptText = $scriptText.Replace('    exit 0','    return')
function Start-ScheduledTask { param([string]$TaskName) }
$backend = [ScriptBlock]::Create($scriptText)
function Invoke-Fixture { & $backend -Source $PackageDirectory -Mode Lite -InstallDirectory $fixtureRoot | Out-Null }
function Hashes {
    $result = @{}
    foreach ($file in Get-ChildItem -LiteralPath $fixtureRoot -Recurse -File) {
        $result[$file.FullName.Substring($fixtureRoot.Length)] = (Get-FileHash -LiteralPath $file.FullName).Hash
    }
    return $result
}
$results = @()
try {
    Invoke-Fixture
    if (!(Test-Path -LiteralPath (Join-Path $fixtureRoot 'OmniSound.exe'))) { throw 'Fresh installation did not deploy the app.' }
    if ((Get-ItemProperty -LiteralPath ($fixtureKey + '\Uninstall')).InstallLocation -ne $fixtureRoot) { throw 'Custom installation location was not recorded.' }
    if ((Get-ItemProperty -LiteralPath ($fixtureKey + '\Run')).$fixtureTask -notlike '*OmniSound.exe*--background*') { throw 'Fresh install did not register the background UI.' }
    if ((Get-ScheduledTask -TaskName $fixtureTask).Actions.Arguments -ne '--lite --login') { throw 'Login task does not honor the startup preference.' }
    $results += 'fresh install and custom path'
    Invoke-Fixture
    $results += 'repeat update'
    New-Item -Path ($fixtureKey + '\Desktop') -Force | Out-Null
    New-ItemProperty -LiteralPath ($fixtureKey + '\Desktop') -Name StartWithWindows -Value 0 -PropertyType DWord -Force | Out-Null
    Invoke-Fixture
    if ((Get-ItemProperty -LiteralPath ($fixtureKey + '\Run')).$fixtureTask) { throw 'Update reenabled disabled login startup.' }
    New-ItemProperty -LiteralPath ($fixtureKey + '\Desktop') -Name StartWithWindows -Value 1 -PropertyType DWord -Force | Out-Null
    Invoke-Fixture
    if ((Get-ItemProperty -LiteralPath ($fixtureKey + '\Run')).$fixtureTask -notlike '*OmniSound.exe*--background*') { throw 'Enabled startup was not registered after update.' }
    $results += 'update preserves login preference'

    # Model a different predecessor executable. Force a later resource-file
    # replacement failure and prove that already-replaced files are restored.
    $priorSetup = Join-Path $fixtureRoot 'OmniSoundSetup.exe'
    $stream = [IO.File]::Open($priorSetup,[IO.FileMode]::Append,[IO.FileAccess]::Write)
    try { $stream.WriteByte(7) } finally { $stream.Dispose() }
    $before = Hashes
    $lock = [IO.File]::Open((Join-Path $fixtureRoot 'App.xbf'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $failure = $null
    try { Invoke-Fixture } catch { $failure = $_.Exception.Message } finally { $lock.Dispose() }
    if (!$failure -or $failure -notlike '*Cannot replace*' -or $failure -like '*Rollback also failed*') { throw "Unexpected replacement failure result: $failure" }
    $after = Hashes
    foreach ($name in $before.Keys) { if ($after[$name] -ne $before[$name]) { throw "Rollback hash mismatch: $name" } }
    $results += 'locked resource rejection and predecessor rollback'
    Invoke-Fixture
    $results += 'recovery after failed update'
    $retainedConfig = Join-Path $fixtureData 'lite-config.json'
    Set-Content -LiteralPath $retainedConfig -Value 'fixture configuration retention marker' -Encoding utf8
    $retainedHash = (Get-FileHash -LiteralPath $retainedConfig).Hash
    & $backend -Source $PackageDirectory -Mode Lite -InstallDirectory $fixtureRoot -Uninstall | Out-Null
    if ((Test-Path -LiteralPath (Join-Path $fixtureRoot 'OmniSound.exe')) -or (Test-Path -LiteralPath ($fixtureKey + '\Uninstall'))) { throw 'Uninstall left the app or registration behind.' }
    if ((Get-FileHash -LiteralPath $retainedConfig).Hash -ne $retainedHash) { throw 'Uninstall changed retained configuration.' }
    $results += 'uninstall retains configuration'
    Invoke-Fixture
    if (!(Test-Path -LiteralPath (Join-Path $fixtureRoot 'OmniSound.exe'))) { throw 'Reinstallation after uninstall failed.' }
    $results += 'reinstall with retained uninstaller file'
    @{ tests=$results; passed=$true; fixtureOnly=$true; nativeDriverInstall=$false } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'build\validation\installer-backend.json') -Encoding utf8
    $results | ForEach-Object { Write-Output "PASS $_" }
} finally {
    Unregister-ScheduledTask -TaskName $fixtureTask -Confirm:$false -ErrorAction SilentlyContinue
    if ($fixtureKey -notmatch '^HKCU:\\SOFTWARE\\OmniSound\\InstallerTests\\[0-9a-f]{32}$') { throw 'Unsafe test registry cleanup path.' }
    Remove-Item -LiteralPath $fixtureKey -Recurse -Force -ErrorAction SilentlyContinue
    $allowedTargets = @(
        [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\OmniSound-Installer-Test')),
        [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'OmniSound\installer-tests'))
    )
    $targets = @($fixtureRoot,$fixtureData)
    for ($i=0;$i -lt $targets.Count;$i++) {
        $resolved = [IO.Path]::GetFullPath($targets[$i])
        if (!$resolved.StartsWith($allowedTargets[$i]+'\',[StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolved) -ne $identity) { throw 'Unsafe installer-test cleanup path.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
}
