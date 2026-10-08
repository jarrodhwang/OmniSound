param([switch]$Build)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$kitVersion = '10.0.28000.0'
$wdk = Join-Path $PSScriptRoot 'cache\wdk\c'
$sdk = Join-Path $PSScriptRoot 'cache\sdk\c'
$release = Join-Path $root 'build\driver\Release'
$submission = Join-Path $root 'dist\submission-0.2.0'
$inf2cat = Join-Path $wdk "bin\$kitVersion\x86\Inf2Cat.exe"
$infverif = Join-Path $wdk "tools\$kitVersion\x64\InfVerif.exe"
$signtool = Join-Path $sdk "bin\$kitVersion\x64\signtool.exe"
foreach ($file in @("$wdk\Include\$kitVersion\km\ntddk.h",$inf2cat,$infverif,$signtool)) {
    if (!(Test-Path -LiteralPath $file)) { throw "Missing pinned WDK/SDK tool: $file. Run tools\Restore-DriverTools.ps1 first." }
}
if ($Build) { & (Join-Path $PSScriptRoot 'Build-Driver.ps1') }
foreach ($name in @('OmniSound.inf','OmniSoundAudio.sys','OmniSoundAudio.pdb')) {
    if (!(Test-Path -LiteralPath (Join-Path $release $name))) { throw "Driver build output is missing: $name. Run tools\Build-Driver.ps1." }
}
if (Test-Path -LiteralPath $submission) { throw "Submission directory already exists; preserve it and choose a new path: $submission" }
New-Item -ItemType Directory -Force $submission | Out-Null
foreach ($name in @('OmniSound.inf','OmniSoundAudio.sys','OmniSoundAudio.pdb')) {
    Copy-Item -LiteralPath (Join-Path $release $name) -Destination $submission
}
& $infverif /w (Join-Path $submission 'OmniSound.inf')
if ($LASTEXITCODE) { throw 'InfVerif rejected the submission INF.' }
& $inf2cat "/driver:$submission" /os:10_NI_X64 /uselocaltime
if ($LASTEXITCODE) { throw 'Inf2Cat rejected the submission package.' }
$cab = Join-Path $root 'dist\OmniSound-0.2.0-driver-submission.cab'
if (Test-Path -LiteralPath $cab) { throw "Submission CAB already exists; preserve it: $cab" }
$makecab = Join-Path $env:WINDIR 'System32\makecab.exe'
$ddf = Join-Path $submission 'submission.ddf'
@(".Set CabinetNameTemplate=$(Split-Path $cab -Leaf)",'.Set Cabinet=ON','.Set Compress=ON',
  ".Set DiskDirectoryTemplate=$root\dist", ".Set SourceDir=$submission",
  'OmniSound.inf', 'OmniSound.cat', 'OmniSoundAudio.sys', 'OmniSoundAudio.pdb') |
    Set-Content -LiteralPath $ddf -Encoding ascii
Push-Location $submission
try {
    & $makecab /F $ddf
    if ($LASTEXITCODE) { throw 'Failed to create the Hardware Dev Center CAB.' }
} finally { Pop-Location }
foreach ($intermediate in @($ddf, (Join-Path $submission 'setup.inf'), (Join-Path $submission 'setup.rpt'))) {
    Remove-Item -LiteralPath $intermediate -Force -ErrorAction SilentlyContinue
}
Write-Output "Microsoft Hardware Dev Center CAB: $cab"
Write-Output "Submission driver package: $submission"
Write-Output 'Contains SYS, PDB, INF and unsigned CAT. Sign the CAB with your registered EV certificate before a Partner Center test-signing submission. A certified general release also needs HLK/WHCP validation and submission; this CAB does not certify the product. See docs/PRO_RELEASE.md.'
