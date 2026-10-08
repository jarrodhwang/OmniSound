param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$packagesRoot = Join-Path $root 'tools\cache\nuget'
$versions = [ordered]@{
  'Microsoft.Windows.CppWinRT'='2.0.250303.1'
  'Microsoft.WindowsAppSDK'='1.8.260804001'
  'Microsoft.WindowsAppSDK.Base'='1.8.251216001'
  'Microsoft.WindowsAppSDK.Foundation'='1.8.260803002'
  'Microsoft.WindowsAppSDK.InteractiveExperiences'='1.8.260708001'
  'Microsoft.WindowsAppSDK.WinUI'='1.8.260803003'
  'Microsoft.WindowsAppSDK.DWrite'='1.8.25122902'
  'Microsoft.WindowsAppSDK.Widgets'='1.8.251231004'
  'Microsoft.WindowsAppSDK.AI'='1.8.79'
  'Microsoft.WindowsAppSDK.ML'='1.8.2197'
  'Microsoft.WindowsAppSDK.Runtime'='1.8.260804001'
  'Microsoft.Web.WebView2'='1.0.3179.45'
}
New-Item -ItemType Directory -Force $packagesRoot | Out-Null
foreach ($entry in $versions.GetEnumerator()) {
    $id = $entry.Key.ToLowerInvariant()
    $version = $entry.Value
    $destination = Join-Path $packagesRoot "$id\$version"
    if (Test-Path -LiteralPath $destination) { continue }
    $zip = Join-Path $env:TEMP "$id.$version.nupkg"
    $url = "https://api.nuget.org/v3-flatcontainer/$id/$version/$id.$version.nupkg"
    Invoke-WebRequest -Uri $url -OutFile $zip
    New-Item -ItemType Directory -Force $destination | Out-Null
    Expand-Archive -LiteralPath $zip -DestinationPath $destination -Force
    Remove-Item -LiteralPath $zip -Force
}
$runtimeVersion = '1.8.260804001'
$redistributable = Join-Path $PSScriptRoot "cache\redistributables\$runtimeVersion\WindowsAppRuntimeInstall-x64.exe"
if (!(Test-Path -LiteralPath $redistributable)) {
    New-Item -ItemType Directory -Force (Split-Path $redistributable -Parent) | Out-Null
    Invoke-WebRequest -Uri "https://aka.ms/windowsappsdk/1.8/$runtimeVersion/windowsappruntimeinstall-x64.exe" -OutFile $redistributable
}
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $redistributable
if ($runtimeSignature.Status -ne 'Valid' -or $runtimeSignature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
    throw 'The pinned Windows App Runtime installer is not signed by Microsoft.'
}
$projection = Join-Path $packagesRoot 'winrt-projection'
New-Item -ItemType Directory -Force $projection | Out-Null
$cppwinrt = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\cppwinrt.exe'
if (!(Test-Path -LiteralPath $cppwinrt)) { throw 'Install the Windows 10.0.26100 SDK C++/WinRT tools in Visual Studio.' }
$union = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\UnionMetadata\10.0.26100.0'
$metadata = @(
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.winui\1.8.260803003\metadata\Microsoft.UI.Xaml.winmd'),
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.winui\1.8.260803003\metadata\Microsoft.UI.Text.winmd'),
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.interactiveexperiences\1.8.260708001\metadata\10.0.18362.0\Microsoft.UI.winmd'),
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.interactiveexperiences\1.8.260708001\metadata\10.0.18362.0\Microsoft.Graphics.winmd'),
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.interactiveexperiences\1.8.260708001\metadata\10.0.18362.0\Microsoft.Foundation.winmd'),
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.foundation\1.8.260803002\metadata\Microsoft.Windows.Foundation.winmd'),
    (Join-Path $packagesRoot 'microsoft.windowsappsdk.foundation\1.8.260803002\metadata\Microsoft.Windows.ApplicationModel.Resources.winmd'),
    (Join-Path $packagesRoot 'microsoft.web.webview2\1.0.3179.45\lib\Microsoft.Web.WebView2.Core.winmd'),
    (Join-Path $union 'Windows.winmd'), (Join-Path $union 'Facade\windows.winmd'))
foreach ($file in $metadata) { if (!(Test-Path -LiteralPath $file)) { throw "WinUI metadata missing: $file" } }
$metadata += Get-ChildItem (Join-Path $packagesRoot 'microsoft.windowsappsdk.interactiveexperiences\1.8.260708001\metadata\10.0.18362.0') -Filter '*.winmd' | ForEach-Object FullName
$metadata = $metadata | Where-Object { Test-Path -LiteralPath $_ }
& $cppwinrt -input $metadata -output $projection -include Microsoft.UI.Xaml -include Microsoft.UI.Dispatching -base -overwrite
if ($LASTEXITCODE) { throw 'Failed to generate the pinned C++/WinRT WinUI projections.' }
$props = Join-Path $packagesRoot 'NuGet.Config'
@"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources><clear /><add key="omnisound-local" value="$packagesRoot" /></packageSources>
</configuration>
"@ | Set-Content -LiteralPath $props -Encoding utf8
Write-Output "Pinned Windows App SDK 1.8 packages and Microsoft-signed x64 runtime installer restored."
