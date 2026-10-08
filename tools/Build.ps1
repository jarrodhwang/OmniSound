param([switch]$Test)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio 2026 Desktop development with C++.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
Push-Location $root
try {
    $env:NuGetPackageRoot = Join-Path $root 'tools\cache\nuget\'
    if (!(Test-Path -LiteralPath (Join-Path $env:NuGetPackageRoot 'microsoft.windowsappsdk\1.8.260804001\build\native\Microsoft.WindowsAppSDK.targets'))) {
        throw 'Restore the pinned WinUI packages first with tools\Restore-WinUI.ps1.'
    }
    & $cmake --preset windows-x64
    if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    & $cmake --build --preset release --parallel
    if ($LASTEXITCODE) { throw 'Build failed.' }
    & $msbuild (Join-Path $root 'src\ui\winui\OmniSound.vcxproj') /m /p:Configuration=Release /p:Platform=x64 /t:Build /v:quiet /clp:ErrorsOnly
    if ($LASTEXITCODE) { throw 'WinUI 3 application build failed.' }
    if ($Test) {
        & $ctest --preset release --output-on-failure
        if ($LASTEXITCODE) { throw 'Tests failed.' }
        if (!('OmniSoundWindowProbe' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OmniSoundWindowProbe {
    private delegate bool EnumProc(IntPtr hwnd, IntPtr state);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr state);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    public static IntPtr FindForProcess(int target) {
        IntPtr result = IntPtr.Zero;
        EnumProc callback = delegate(IntPtr hwnd, IntPtr state) {
            uint processId;
            GetWindowThreadProcessId(hwnd, out processId);
            if (processId == target) { result = hwnd; return false; }
            return true;
        };
        EnumWindows(callback, IntPtr.Zero);
        return result;
    }
}
'@
        }
        $output = Join-Path $root 'build\windows-x64\Release'
        $smokeParent = [IO.Path]::GetFullPath((Join-Path $root 'build\winui-smoke'))
        $smoke = Get-ChildItem -LiteralPath $smokeParent -Directory -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if (!$smoke) {
            $smoke = Join-Path $smokeParent 'deployment'
            New-Item -ItemType Directory -Force -Path $smoke | Out-Null
        }
        $payload = @('OmniSound.exe','OmniSoundEngine.exe','OmniSoundService.exe','OmniSoundProbe.exe','OmniSoundSetup.exe',
            'Microsoft.WindowsAppRuntime.Bootstrap.dll','App.xbf','MainWindow.xbf','Microsoft.UI.pri',
            'Microsoft.UI.Xaml.Controls.pri','Microsoft.WindowsAppRuntime.pri','OmniSound.winmd')
        foreach ($file in $payload) {
            $source = Join-Path $output $file
            if (!(Test-Path -LiteralPath $source)) { throw "WinUI deployment file is missing: $file" }
            Copy-Item -LiteralPath $source -Destination $smoke
        }
        $ui = $null
        try {
            $ui = Start-Process -FilePath (Join-Path $smoke 'OmniSound.exe') -ArgumentList '--ui-deployment-smoke' -WindowStyle Hidden -PassThru
            $deadline = [DateTime]::UtcNow.AddSeconds(8)
            do {
                Start-Sleep -Milliseconds 200
                $ui.Refresh()
                if ($ui.HasExited) { throw "WinUI 3 application exited during startup (exit code $($ui.ExitCode))." }
                $window = [OmniSoundWindowProbe]::FindForProcess($ui.Id)
            } while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
            if ($window -eq [IntPtr]::Zero -or ![OmniSoundWindowProbe]::IsWindow($window)) {
                throw 'WinUI 3 process started but did not create its main window.'
            }
            [void][OmniSoundWindowProbe]::ShowWindow($window, 0)
            Write-Output "WinUI 3 deployment smoke passed ($($payload.Count) files; main window handle $window)."
        } finally {
            if ($ui) {
                $ui.Refresh()
                if (!$ui.HasExited) { Stop-Process -Id $ui.Id -Force }
            }
        }
        $inputSmoke = Start-Process -FilePath (Join-Path $smoke 'OmniSound.exe') -ArgumentList '--ui-input-smoke' -WindowStyle Hidden -PassThru
        if (!$inputSmoke.WaitForExit(15000)) {
            Stop-Process -Id $inputSmoke.Id -Force
            throw 'WinUI input preservation smoke timed out.'
        }
        if ($inputSmoke.ExitCode -ne 0) { throw "WinUI input preservation smoke failed ($($inputSmoke.ExitCode))." }
        Write-Output 'WinUI input smoke passed (Lite picker/drafts/volume, Pro routing/position/zero reserve, French/Japanese switching without renaming groups).'
        $desktopSmoke = Start-Process -FilePath (Join-Path $smoke 'OmniSound.exe') -ArgumentList '--ui-desktop-smoke' -WindowStyle Hidden -PassThru
        if (!$desktopSmoke.WaitForExit(15000)) { Stop-Process -Id $desktopSmoke.Id -Force; throw 'Tray lifecycle smoke timed out.' }
        if ($desktopSmoke.ExitCode -ne 0) { throw "Tray lifecycle smoke failed ($($desktopSmoke.ExitCode))." }
        Write-Output 'Tray lifecycle smoke passed (icon creation, close to tray, restore, removal).'
        $uiSnapshot = Join-Path $root 'build\validation\ui\main.png'
        $visualSmoke = Start-Process -FilePath (Join-Path $smoke 'OmniSound.exe') -ArgumentList @('--ui-visual-smoke',('"'+$uiSnapshot+'"')) -WindowStyle Hidden -PassThru
        if (!$visualSmoke.WaitForExit(20000)) { Stop-Process -Id $visualSmoke.Id -Force; throw 'UI snapshot timed out.' }
        if ($visualSmoke.ExitCode -ne 0) { throw "UI snapshot failed ($($visualSmoke.ExitCode))." }
        Write-Output 'WinUI Lite system/light/dark/narrow and Pro five-language layout snapshots captured.'
        foreach ($document in @('LICENSE','THIRD_PARTY_NOTICES.md')) {
            Copy-Item -LiteralPath (Join-Path $root $document) -Destination $smoke -Force
        }
        $snapshots = Join-Path $root 'build\validation\setup'
        $setupSmoke = Start-Process -FilePath (Join-Path $smoke 'OmniSoundSetup.exe') -ArgumentList @('--wizard-smoke','--snapshots',('"' + $snapshots + '"')) -WindowStyle Hidden -PassThru
        if (!$setupSmoke.WaitForExit(15000)) {
            Stop-Process -Id $setupSmoke.Id -Force
            throw 'Installer wizard smoke timed out.'
        }
        if ($setupSmoke.ExitCode -ne 0) { throw "Installer wizard smoke failed ($($setupSmoke.ExitCode))." }
        Write-Output 'Installer wizard smoke passed (agreement gating, path retention, update/retry pages, protected progress, screenshots).'
    }
} finally { Pop-Location }
