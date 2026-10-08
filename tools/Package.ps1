param([switch]$Release, [switch]$LiteOnly, [string]$SignedDriverDirectory, [string]$AuthenticodeThumbprint)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$binaries = Join-Path $root 'build\windows-x64\Release'
$runtimeInstaller = Join-Path $root 'tools\cache\redistributables\1.8.260804001\WindowsAppRuntimeInstall-x64.exe'
$driver = if ($SignedDriverDirectory) { [IO.Path]::GetFullPath($SignedDriverDirectory) } else { Join-Path $root 'build\driver\Release' }
$names = @('OmniSound.exe','OmniSoundEngine.exe','OmniSoundService.exe','OmniSoundProbe.exe','OmniSoundSetup.exe')
$bootstrapper = Join-Path $binaries 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
$uiResources = @('App.xbf','MainWindow.xbf','Microsoft.UI.pri','Microsoft.UI.Xaml.Controls.pri','Microsoft.WindowsAppRuntime.pri','OmniSound.winmd')
foreach ($name in $names) { if (!(Test-Path -LiteralPath (Join-Path $binaries $name))) { throw "Build the application first: $name is missing." } }
if (!(Test-Path -LiteralPath $bootstrapper)) { throw 'WinUI output is missing Microsoft.WindowsAppRuntime.Bootstrap.dll. Build with tools\Build.ps1.' }
foreach ($name in $uiResources) { if (!(Test-Path -LiteralPath (Join-Path $binaries $name))) { throw "WinUI output is missing a required XAML resource: $name" } }
if (!(Test-Path -LiteralPath $runtimeInstaller)) { throw 'Run tools\Restore-WinUI.ps1 to fetch the pinned Microsoft Windows App Runtime installer.' }
$bootstrapSignature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $runtimeInstaller
foreach ($entry in @(@{Name='Windows App SDK bootstrapper';Signature=$bootstrapSignature},@{Name='Windows App Runtime installer';Signature=$runtimeSignature})) {
    if ($entry.Signature.Status -ne 'Valid' -or $entry.Signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
        throw "$($entry.Name) does not have a valid Microsoft Authenticode signature."
    }
}
if ($LiteOnly -and $SignedDriverDirectory) { throw '-SignedDriverDirectory is not used with -LiteOnly.' }
if (!$LiteOnly) {
    foreach ($name in @('OmniSound.inf','OmniSoundAudio.sys','OmniSound.cat')) {
        if (!(Test-Path -LiteralPath (Join-Path $driver $name))) { throw "Build the driver first: $name is missing." }
    }
}
if ($Release) {
    if (!$AuthenticodeThumbprint) { throw '-Release requires -AuthenticodeThumbprint for the expected application publisher.' }
    foreach ($name in $names) {
        $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $binaries $name)
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $AuthenticodeThumbprint -or
            $sig.SignerCertificate.Subject -match 'Local Test|Non-Production' -or
            $sig.SignerCertificate.Subject -eq $sig.SignerCertificate.Issuer) { throw "Release file has an invalid or unexpected Authenticode signature: $name" }
    }
    if (!$LiteOnly) {
        if (!$SignedDriverDirectory) { throw '-Release requires -SignedDriverDirectory with a Microsoft-signed package, unless -LiteOnly is selected.' }
        $signtool = Join-Path $PSScriptRoot 'cache\sdk\c\bin\10.0.28000.0\x64\signtool.exe'
        if (!(Test-Path -LiteralPath $signtool)) { throw 'Restore the SDK tools first.' }
        & $signtool verify /kp /v /c (Join-Path $driver 'OmniSound.cat') (Join-Path $driver 'OmniSoundAudio.sys')
        if ($LASTEXITCODE) { throw 'Driver does not pass kernel signing policy.' }
        $catalogSignature = Get-AuthenticodeSignature -LiteralPath (Join-Path $driver 'OmniSound.cat')
        if ($catalogSignature.Status -ne 'Valid' -or $catalogSignature.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows') { throw 'Driver catalog is not signed by Microsoft.' }
    }
}
$flavor = if ($LiteOnly) { if ($Release) { 'lite-release' } else { 'lite-development-unsigned' } } elseif ($Release) { 'release' } else { 'development-unsigned' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$destination = Join-Path $root "dist\OmniSound-0.2.0-$flavor-$stamp"
$packageDirectories = @($destination,(Join-Path $destination 'licenses'),(Join-Path $destination 'prerequisites'))
if (!$LiteOnly) { $packageDirectories += Join-Path $destination 'driver' }
New-Item -ItemType Directory -Force $packageDirectories | Out-Null
foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $destination }
Copy-Item -LiteralPath $bootstrapper -Destination $destination
foreach ($name in $uiResources) { Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $destination }
Copy-Item -LiteralPath $runtimeInstaller -Destination (Join-Path $destination 'prerequisites')
if (!$LiteOnly) {
    foreach ($name in @('OmniSound.inf','OmniSoundAudio.sys','OmniSound.cat')) { Copy-Item -LiteralPath (Join-Path $driver $name) -Destination (Join-Path $destination 'driver') }
}
foreach ($name in @('README.md','LICENSE','THIRD_PARTY_NOTICES.md')) { Copy-Item -LiteralPath (Join-Path $root $name) -Destination $destination }
Copy-Item -LiteralPath (Join-Path $root 'tools\cache\nuget\microsoft.windowsappsdk.runtime\1.8.260804001\license.txt') -Destination (Join-Path $destination 'licenses\WindowsAppSDK-License.txt')
if (!$LiteOnly) { Copy-Item -LiteralPath (Join-Path $root 'driver\sysvad\LICENSE') -Destination (Join-Path $destination 'licenses\SysVAD-MIT.txt') }
Copy-Item -LiteralPath (Join-Path $root 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $destination 'licenses\nlohmann-json-MIT.txt')
Copy-Item -LiteralPath (Join-Path $root 'installer\Agreement.txt') -Destination (Join-Path $destination 'Agreement.txt')
$innoLicense = Join-Path $PSScriptRoot 'cache\inno\7.1.0\compiler\license.txt'
if (Test-Path -LiteralPath $innoLicense) { Copy-Item -LiteralPath $innoLicense -Destination (Join-Path $destination 'licenses\InnoSetup-License.txt') }
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $destination -Recurse
if (!$Release -and (Test-Path -LiteralPath (Join-Path $root 'build\validation\host-smoke.json'))) {
    $evidence = Join-Path $destination 'development-evidence'
    New-Item -ItemType Directory -Path $evidence | Out-Null
    foreach ($name in @('host-smoke.json','release-gates.json','installer-backend.json','lite-desktop.json',
        'audio-quality-acceptance.json','audio-quality-release-check-60s.json','audio-quality-release-check-normal.json',
        'audio-quality-render-recovery.json','audio-quality-release-check-performance.json')) {
        $file = Join-Path $root "build\validation\$name"
        if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination $evidence }
    }
    $snapshot = Join-Path $root 'build\windows-x64\ui-smoke.png'
    if (Test-Path -LiteralPath $snapshot) { Copy-Item -LiteralPath $snapshot -Destination $evidence }
    $setupSnapshots = Join-Path $root 'build\validation\setup'
    if (Test-Path -LiteralPath $setupSnapshots) { Copy-Item -LiteralPath $setupSnapshots -Destination (Join-Path $evidence 'setup') -Recurse }
    $uiSnapshots = Join-Path $root 'build\validation\ui'
    if (Test-Path -LiteralPath $uiSnapshots) { Copy-Item -LiteralPath $uiSnapshots -Destination (Join-Path $evidence 'ui') -Recurse }
}
$manifest = Get-ChildItem $destination -Recurse -File | ForEach-Object { [ordered]@{path=$_.FullName.Substring($destination.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash} }
@{version='0.2.0';flavor=$flavor;createdUtc=[DateTime]::UtcNow.ToString('o');files=$manifest} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding utf8
Compress-Archive -LiteralPath $destination -DestinationPath "$destination.zip"
Write-Output "$destination.zip"
