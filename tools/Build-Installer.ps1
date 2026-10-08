param(
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    [switch]$Release,
    [switch]$LiteOnly,
    [string]$AuthenticodeThumbprint,
    [string]$TimestampUrl = 'https://timestamp.digicert.com'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$package = [IO.Path]::GetFullPath($PackageDirectory)
$compiler = Join-Path $PSScriptRoot 'cache\inno\7.1.0\compiler\ISCC.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw 'Run tools\Restore-InstallerTools.ps1 for the pinned portable compiler.' }
$signature = Get-AuthenticodeSignature -LiteralPath $compiler
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Pyrsys B.V.,') { throw 'Unexpected installer compiler publisher.' }
$manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $package $file.path))
    if (!$path.StartsWith($package + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $path).Hash -ne $file.sha256) { throw "Package verification failed: $($file.path)" }
}
$parameters = @('/Qp',('/DPayloadDir=' + $package),('/DOutputDirectory=' + (Join-Path $root 'dist')),('/DProductVersion=' + $manifest.version))
if ($Release) {
    if (!$AuthenticodeThumbprint -or $manifest.flavor -notmatch 'release$') { throw 'A signed release package and publisher thumbprint are required.' }
    foreach ($name in @('OmniSound.exe','OmniSoundEngine.exe','OmniSoundService.exe','OmniSoundProbe.exe','OmniSoundSetup.exe')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $package $name)
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $AuthenticodeThumbprint -or
            $signature.SignerCertificate.Subject -match 'Local Test|Non-Production' -or
            $signature.SignerCertificate.Subject -eq $signature.SignerCertificate.Issuer) { throw "Release signature mismatch: $name" }
    }
    if (!$LiteOnly) {
        $catalog = Join-Path $package 'driver\OmniSound.cat'
        $signature = Get-AuthenticodeSignature -LiteralPath $catalog
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Windows') { throw 'Pro requires a Microsoft-signed driver catalog.' }
        $parameters += '/DEnablePro=1'
    }
    $signTool = Join-Path $PSScriptRoot 'cache\sdk\c\bin\10.0.28000.0\x64\signtool.exe'
    if (!(Test-Path -LiteralPath $signTool)) { throw 'Restore SDK signing tools first.' }
    if (!$LiteOnly) {
        & $signTool verify /kp /v /c $catalog (Join-Path $package 'driver\OmniSoundAudio.sys')
        if ($LASTEXITCODE) { throw 'Kernel driver signing policy failed.' }
    }
    if ($AuthenticodeThumbprint -notmatch '^[0-9a-fA-F]{40}$' -or $TimestampUrl -notmatch '^https://[a-zA-Z0-9./_-]+$') { throw 'Invalid signing parameters.' }
    $parameters += '/DSignedBuild=1'
    $parameters += '/Somnisound="' + $signTool + '" sign /sha1 ' + $AuthenticodeThumbprint + ' /fd SHA256 /tr ' + $TimestampUrl + ' /td SHA256 $f'
}
& $compiler @parameters (Join-Path $root 'installer\OmniSound.iss')
if ($LASTEXITCODE) { throw 'Inno Setup compilation failed.' }
$output = Join-Path $root ('dist\OmniSound-' + $manifest.version + '-Setup.exe')
if ($Release) {
    $signature = Get-AuthenticodeSignature -LiteralPath $output
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $AuthenticodeThumbprint) { throw 'Final setup publisher verification failed.' }
}
Copy-Item -LiteralPath $output -Destination (Join-Path $root 'dist\OmniSound-Setup.exe') -Force
Write-Output $output
