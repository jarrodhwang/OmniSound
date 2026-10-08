param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$binaries = Join-Path $root 'build\windows-x64\Release'
$driverSource = Join-Path $root 'build\driver\Release'
$runtimeInstaller = Join-Path $PSScriptRoot 'cache\redistributables\1.8.260804001\WindowsAppRuntimeInstall-x64.exe'
$certStore = 'Cert:\CurrentUser\My'
$subject = 'CN=OmniSound Local Test, O=Non-Production'
$cert = Get-ChildItem -Path $certStore | Where-Object {
    $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date)
} | Sort-Object NotAfter -Descending | Select-Object -First 1
if (!$cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -FriendlyName 'OmniSound local driver test only' -CertStoreLocation $certStore -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)
}
$signtool = Join-Path $PSScriptRoot 'cache\sdk\c\bin\10.0.28000.0\x64\signtool.exe'
if (!(Test-Path -LiteralPath $signtool)) { throw 'Run tools\Restore-DriverTools.ps1 first.' }
$appNames = @('OmniSound.exe','OmniSoundEngine.exe','OmniSoundService.exe','OmniSoundProbe.exe','OmniSoundSetup.exe')
$uiResources = @('App.xbf','MainWindow.xbf','Microsoft.UI.pri','Microsoft.UI.Xaml.Controls.pri','Microsoft.WindowsAppRuntime.pri','OmniSound.winmd')
$driverNames = @('OmniSound.inf','OmniSoundAudio.sys','OmniSound.cat')
function Test-TestSignature($Signature, [string]$Thumbprint) {
    $untrustedRoot = $Signature.Status -eq 'UnknownError' -and
        $Signature.StatusMessage -match 'chain processed.*root certificate which is not trusted'
    return $Signature.SignerCertificate -and $Signature.SignerCertificate.Thumbprint -eq $Thumbprint -and
        ($Signature.Status -eq 'Valid' -or $Signature.Status -eq 'NotTrusted' -or $untrustedRoot)
}
foreach ($name in $appNames) { if (!(Test-Path -LiteralPath (Join-Path $binaries $name))) { throw "Build the app first: $name" } }
foreach ($file in @((Join-Path $binaries 'Microsoft.WindowsAppRuntime.Bootstrap.dll'),$runtimeInstaller)) {
    if (!(Test-Path -LiteralPath $file)) { throw "WinUI runtime prerequisite is missing: $file. Run tools\Restore-WinUI.ps1 and tools\Build.ps1." }
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
        throw "Microsoft runtime prerequisite signature is invalid: $file"
    }
}
foreach ($name in $uiResources) { if (!(Test-Path -LiteralPath (Join-Path $binaries $name))) { throw "WinUI XAML resource is missing: $name. Build with tools\Build.ps1." } }
foreach ($name in $driverNames) { if (!(Test-Path -LiteralPath (Join-Path $driverSource $name))) { throw "Build the driver first: $name" } }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$destination = Join-Path $root "dist\OmniSound-0.2.0-local-test-$stamp"
New-Item -ItemType Directory -Force -Path $destination,(Join-Path $destination 'driver'),(Join-Path $destination 'licenses'),(Join-Path $destination 'prerequisites') | Out-Null
foreach ($name in $appNames) { Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $destination }
Copy-Item -LiteralPath (Join-Path $binaries 'Microsoft.WindowsAppRuntime.Bootstrap.dll') -Destination $destination
foreach ($name in $uiResources) { Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $destination }
Copy-Item -LiteralPath $runtimeInstaller -Destination (Join-Path $destination 'prerequisites')
foreach ($name in $driverNames) { Copy-Item -LiteralPath (Join-Path $driverSource $name) -Destination (Join-Path $destination 'driver') }
foreach ($name in @('README.md','README.ko.md','LICENSE','LICENSES.md','THIRD_PARTY_NOTICES.md')) { Copy-Item -LiteralPath (Join-Path $root $name) -Destination $destination }
Copy-Item -LiteralPath (Join-Path $root 'tools\cache\nuget\microsoft.windowsappsdk.runtime\1.8.260804001\license.txt') -Destination (Join-Path $destination 'licenses\WindowsAppSDK-License.txt')
Copy-Item -LiteralPath (Join-Path $root 'driver\sysvad\LICENSE') -Destination (Join-Path $destination 'licenses\SysVAD-MS-PL.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party\nlohmann\LICENSE.MIT') -Destination (Join-Path $destination 'licenses\nlohmann-json-MIT.txt')
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $destination -Recurse
Export-Certificate -Cert $cert -FilePath (Join-Path $destination 'licenses\OmniSoundLocalTest.cer') -Type CERT | Out-Null
foreach ($name in $appNames) {
    & $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint /v (Join-Path $destination $name)
    if ($LASTEXITCODE) { throw "Test signing failed: $name" }
}
& $signtool sign /fd SHA256 /s My /sha1 $cert.Thumbprint /v (Join-Path $destination 'driver\OmniSound.cat')
if ($LASTEXITCODE) { throw 'Test signing the driver catalog failed.' }
foreach ($name in $appNames + @('driver\OmniSound.cat')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $destination $name)
    if (!(Test-TestSignature $signature $cert.Thumbprint)) {
        throw "Test signature verification failed: $name ($($signature.Status))"
    }
}
$manifest = Get-ChildItem -LiteralPath $destination -Recurse -File | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($destination.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
}
@{version='0.2.0';flavor='single-machine-local-test';certificateThumbprint=$cert.Thumbprint;note='LOCAL TEST ONLY. Requires Secure Boot disabled and Windows TESTSIGNING enabled. Do not redistribute.';files=$manifest} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'test-manifest.json') -Encoding utf8
$zip = "$destination.zip"
Compress-Archive -LiteralPath $destination -DestinationPath $zip
Write-Output "Local test package: $zip"
Write-Output "Test certificate thumbprint: $($cert.Thumbprint)"
