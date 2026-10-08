# Inno Setup's official portable mode: no machine installation, associations,
# shortcuts or uninstaller registration. See upstream is-7_1_0/setup.iss.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cache = Join-Path $PSScriptRoot 'cache\inno\7.1.0'
$download = Join-Path $cache 'innosetup-7.1.0-x64.exe'
$compilerDirectory = Join-Path $cache 'compiler'
$expectedHash = '0362A383ED217D4C4239B5933866DD96D3EB2102737DA92F80F6057A4B40DF2F'
New-Item -ItemType Directory -Path $cache -Force | Out-Null
if (!(Test-Path -LiteralPath $download)) {
    Invoke-WebRequest -Uri 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe' -OutFile $download
}
if ((Get-FileHash -LiteralPath $download).Hash -ne $expectedHash) { throw 'Pinned compiler package hash mismatch.' }
$signature = Get-AuthenticodeSignature -LiteralPath $download
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Pyrsys B.V.,') {
    throw 'Compiler package is not signed by the expected Inno Setup publisher.'
}
if (!(Test-Path -LiteralPath (Join-Path $compilerDirectory 'ISCC.exe'))) {
    $parameters = @('/PORTABLE=1','/CURRENTUSER','/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="' + $compilerDirectory + '"'))
    $process = Start-Process -FilePath $download -ArgumentList $parameters -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(30000) -or $process.ExitCode -ne 0) { throw 'Portable compiler extraction failed.' }
}
foreach ($name in @('ISCC.exe','ISCmplr.dll','ISPP.dll')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $compilerDirectory $name)
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Pyrsys B.V.,') {
        throw "Portable compiler signature mismatch: $name"
    }
}
Write-Output (Join-Path $compilerDirectory 'ISCC.exe')
