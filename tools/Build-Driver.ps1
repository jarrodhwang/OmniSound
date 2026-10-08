# Standalone WDK compilation, also invokable through driver/Build.proj (MSBuild).
param([switch]$Restore)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Restore) { & (Join-Path $PSScriptRoot 'Restore-DriverTools.ps1') }
$kitVersion = '10.0.28000.0'
$wdk = Join-Path $PSScriptRoot 'cache\wdk\c'
$sdk = Join-Path $PSScriptRoot 'cache\sdk\c'
if (!(Test-Path "$wdk\Include\$kitVersion\km\ntddk.h")) { throw 'Run tools\Restore-DriverTools.ps1 first.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$toolVersion = (Get-Content (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt')).Trim()
$vc = Join-Path $vs "VC\Tools\MSVC\$toolVersion"
$compiler = Join-Path $vc 'bin\Hostx64\x64\cl.exe'
$linker = Join-Path $vc 'bin\Hostx64\x64\link.exe'
$out = Join-Path $root 'build\driver\Release'
$obj = Join-Path $root 'build\driver\obj\Release'
New-Item -ItemType Directory -Force $out,$obj | Out-Null
$includes = @("$root\driver", "$root\driver\sysvad", "$root\driver\sysvad\EndpointsCommon", "$wdk\Include\$kitVersion\km", "$wdk\Include\$kitVersion\km\crt", "$wdk\Include\wdf\kmdf\1.15", "$sdk\Include\$kitVersion\shared", "$sdk\Include\$kitVersion\um", "$sdk\Include\$kitVersion\ucrt", "$vc\include")
$flags = @('/nologo','/c','/std:c++17','/kernel','/O2','/Oi','/GS','/Qspectre','/GR-','/EHs-c-','/Zl','/Zp8','/W4','/WX','/wd4324','/Zi','/FC','/utf-8','/Zc:wchar_t-')
$definitions = @('_AMD64_','AMD64','_WIN64','_WIN32','WINNT=1','WINVER=0x0A00','_WIN32_WINNT=0x0A00','NTDDI_VERSION=0x0A00000C','UNICODE','_UNICODE','PC_IMPLEMENTATION','_USE_WAVERT_','_USE_IPortClsRuntimePower','_NEW_DELETE_OPERATORS_','DEBUG_LEVEL=DEBUGLVL_TERSE','KMDF_VERSION_MAJOR=1','KMDF_VERSION_MINOR=15')
$flags += $includes | ForEach-Object { "/I$_" }
$flags += $definitions | ForEach-Object { "/D$_" }
$sources = @('adapter','basetopo','common','hw','kshelper','savedata','ToneGenerator') | ForEach-Object { "$root\driver\sysvad\$_.cpp" }
$sources += @('AudioModuleHelper','MiniportAudioEngineNode','MiniportStreamAudioEngineNode','mintopo','minwavert','minwavertstream','NewDelete') | ForEach-Object { "$root\driver\sysvad\EndpointsCommon\$_.cpp" }
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $obj (([IO.Path]::GetFileNameWithoutExtension($source)) + '.obj')
    & $compiler @flags "/Fo$object" "/Fd$obj\compiler.pdb" $source
    if ($LASTEXITCODE) { throw "Driver compilation failed: $source" }
    $objects += $object
}
$libraries = @("/LIBPATH:$wdk\Lib\$kitVersion\km\x64", "/LIBPATH:$wdk\Lib\wdf\kmdf\x64\1.15", 'ntoskrnl.lib','hal.lib','wmilib.lib','portcls.lib','stdunk.lib','libcntpr.lib','BufferOverflowK.lib','WdfDriverEntry.lib','WdfLdr.lib')
& $linker /NOLOGO /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry /MACHINE:X64 /NODEFAULTLIB /DYNAMICBASE /NXCOMPAT /INTEGRITYCHECK /DEBUG "/OUT:$out\OmniSoundAudio.sys" "/PDB:$out\OmniSoundAudio.pdb" @objects @libraries
if ($LASTEXITCODE) { throw 'Driver link failed.' }
Copy-Item (Join-Path $root 'driver\OmniSound.inf') $out -Force
$infverif = Join-Path $wdk "tools\$kitVersion\x64\InfVerif.exe"
if (!(Test-Path $infverif)) { $infverif = (Get-ChildItem $wdk -Recurse -Filter InfVerif.exe | Where-Object FullName -Match '\\x64\\' | Select-Object -First 1).FullName }
if ($infverif) { & $infverif /w "$out\OmniSound.inf"; if ($LASTEXITCODE) { throw 'INF validation failed.' } }
$inf2cat = Join-Path $wdk 'bin\10.0.28000.0\x86\Inf2Cat.exe'
if (!(Test-Path -LiteralPath $inf2cat)) { $inf2cat = (Get-ChildItem $wdk -Recurse -Filter Inf2Cat.exe | Select-Object -First 1).FullName }
if ($inf2cat) { & $inf2cat "/driver:$out" /os:10_NI_X64 /uselocaltime; if ($LASTEXITCODE) { throw 'Catalog generation failed.' } }
Write-Output "Unsigned development driver built in $out. Microsoft signing is required before normal installation."
