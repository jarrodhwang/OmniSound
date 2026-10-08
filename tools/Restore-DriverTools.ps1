# Retrieves Microsoft's version-pinned SDK/WDK into this workspace only.
# It does not install or load a driver, import certificates, or change boot settings.
param()
$ErrorActionPreference = 'Stop'
$cache = Join-Path $PSScriptRoot 'cache'
New-Item -ItemType Directory -Force $cache | Out-Null
$version = '10.0.28000.2526'
$packages = @{
    'wdk' = 'microsoft.windows.wdk.x64'
    'sdk' = 'microsoft.windows.sdk.cpp'
    'sdklibs' = 'microsoft.windows.sdk.cpp.x64'
}
foreach ($name in $packages.Keys) {
    $package = $packages[$name]
    $destination = Join-Path $cache $name
    if (Test-Path (Join-Path $destination "$package.nuspec")) { continue }
    $zip = Join-Path $cache "$name.zip"
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$package/$version/$package.$version.nupkg" -OutFile $zip
    Expand-Archive -LiteralPath $zip -DestinationPath $destination -Force
}
Write-Output "SDK/WDK $version restored to $cache"
