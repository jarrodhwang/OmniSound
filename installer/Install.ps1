param([Parameter(Mandatory=$true)][string]$Source, [ValidateSet('Lite','Driver')][string]$Mode='Driver', [string]$InstallDirectory, [switch]$Uninstall, [switch]$LocalTest)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Source = [IO.Path]::GetFullPath($Source)
$lite = $Mode -eq 'Lite'
$target = if ($lite) { [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs\OmniSoundLite')) } else { [IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'OmniSound')) }
$data = if ($lite) { [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'OmniSound')) } else { [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'OmniSound')) }
$serviceName = 'OmniSound'
$taskName = if ($lite) { 'OmniSound Lite Engine' } else { 'OmniSound Engine' }
$runKey = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$startupPreferenceKey = if ($lite) { 'HKCU:\SOFTWARE\OmniSound\Desktop\Lite' } else { 'HKCU:\SOFTWARE\OmniSound\Desktop\Driver' }
$binaries = @('OmniSound.exe','OmniSoundEngine.exe','OmniSoundService.exe','OmniSoundProbe.exe','OmniSoundSetup.exe')
$driverFiles = @('driver\OmniSound.inf','driver\OmniSoundAudio.sys','driver\OmniSound.cat')
$supportFiles = @('Microsoft.WindowsAppRuntime.Bootstrap.dll')
$uiResources = @('App.xbf','MainWindow.xbf','Microsoft.UI.pri','Microsoft.UI.Xaml.Controls.pri','Microsoft.WindowsAppRuntime.pri','OmniSound.winmd')
$runtimeInstaller = Join-Path $Source 'prerequisites\WindowsAppRuntimeInstall-x64.exe'
$uninstallKey = if ($lite) { 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OmniSoundLite' } else { 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OmniSoundDriver' }
$registeredTarget = (Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue).InstallLocation
if ($registeredTarget) { $target = [IO.Path]::GetFullPath($registeredTarget).TrimEnd('\') }
if ($InstallDirectory) {
    if (![IO.Path]::IsPathRooted($InstallDirectory) -or $InstallDirectory.StartsWith('\\')) { throw 'Choose an absolute local installation path.' }
    $requestedTarget = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
    if ((Test-Path -LiteralPath (Join-Path $target 'OmniSound.exe')) -and $requestedTarget -ine $target) {
        throw 'An existing installation was found. Update it in its current folder, or uninstall it before changing the folder.'
    }
    $target = $requestedTarget
}
$allowedRoot = if ($lite) { Join-Path $env:LOCALAPPDATA 'Programs' } else { $env:ProgramFiles }
if (!$target.StartsWith(([IO.Path]::GetFullPath($allowedRoot).TrimEnd('\') + '\'),[StringComparison]::OrdinalIgnoreCase)) {
    throw "Choose an OmniSound subfolder inside $allowedRoot."
}
if (!$Uninstall -and (Test-Path -LiteralPath $target) -and
    !(Test-Path -LiteralPath (Join-Path $target 'OmniSound.exe')) -and
    (Get-ChildItem -LiteralPath $target -Recurse -File -Force | Where-Object {
        $_.FullName -ine (Join-Path $target 'OmniSoundSetup.exe')
    } | Select-Object -First 1)) {
    throw 'The selected folder contains other files. Choose an empty folder dedicated to OmniSound.'
}
$shortcut = if ($lite) { Join-Path ([Environment]::GetFolderPath('Programs')) 'OmniSound Lite.lnk' } else { Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OmniSound Pro.lnk' }
function Remove-LegacyProShortcut {
    if ($lite) { return }
    $legacy = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OmniSound Driver.lnk'
    if (Test-Path -LiteralPath $legacy) {
        $link = (New-Object -ComObject WScript.Shell).CreateShortcut($legacy)
        if ($link.TargetPath -ieq (Join-Path $target 'OmniSound.exe')) { Remove-Item -LiteralPath $legacy -Force }
    }
}
function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "$Executable failed ($LASTEXITCODE)." }
}
function Test-TestSignature($Signature, [string]$Thumbprint) {
    $untrustedRoot = $Signature.Status -eq 'UnknownError' -and
        $Signature.StatusMessage -match 'chain processed.*root certificate which is not trusted'
    return $Signature.SignerCertificate -and $Signature.SignerCertificate.Thumbprint -eq $Thumbprint -and
        ($Signature.Status -eq 'Valid' -or $Signature.Status -eq 'NotTrusted' -or $untrustedRoot)
}
function Test-UnsignedDevelopmentPackage {
    $manifestPath = Join-Path $Source 'manifest.json'
    if (!(Test-Path -LiteralPath $manifestPath)) { return $false }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.flavor -notin @('development-unsigned','lite-development-unsigned')) { return $false }
    $rootPrefix = $Source.TrimEnd('\') + '\'
    foreach ($entry in $manifest.files) {
        $file = [IO.Path]::GetFullPath((Join-Path $Source $entry.path))
        if (!$file.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath $file)) {
            throw 'The development package manifest contains an invalid or missing path.'
        }
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) {
            throw "Development package hash mismatch: $($entry.path)"
        }
    }
    return $true
}
function Assert-NoReparse([string]$Path) {
    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    while ($item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point is not allowed: $($item.FullName)" }
        $item = $item.Parent
    }
}
function Copy-IfDifferent([string]$From, [string]$To) {
    if (![string]::Equals([IO.Path]::GetFullPath($From),[IO.Path]::GetFullPath($To),[StringComparison]::OrdinalIgnoreCase)) {
        Assert-NoReparse $To
        # Replace the directory entry so a pre-existing hard link is never written through.
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while (Test-Path -LiteralPath $To) {
            try { Remove-Item -LiteralPath $To -Force; break }
            catch {
                if ([DateTime]::UtcNow -ge $deadline) { throw "Cannot replace $To. Close OmniSound and other programs using this file, then retry. $($_.Exception.Message)" }
                Start-Sleep -Milliseconds 250
            }
        }
        Copy-Item -LiteralPath $From -Destination $To -Force
    }
}
function Protect-Directory([string]$Path, [bool]$AllowRead) {
    Assert-NoReparse $Path
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true,$false)
    $acl.SetOwner([Security.Principal.SecurityIdentifier]'S-1-5-32-544')
    foreach ($sid in @('S-1-5-18','S-1-5-32-544')) {
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(([Security.Principal.SecurityIdentifier]$sid),'FullControl','ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    if ($AllowRead) {
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(([Security.Principal.SecurityIdentifier]'S-1-5-32-545'),'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
function Stop-Product {
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $taskName -ErrorAction Stop
    }
    if (!$lite) {
        $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
        if ($service) { Stop-Service -Name $serviceName -Force; $service.WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20)) }
    }
    foreach ($process in (Get-Process -Name OmniSound,OmniSoundEngine -ErrorAction SilentlyContinue | Sort-Object Name)) {
        try { $image = $process.Path } catch { continue }
        if ($image -and [IO.Path]::GetFullPath($image).StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) {
            try {
                if (!$process.HasExited) { $process.Kill() }
                if (!$process.WaitForExit(10000)) { throw "OmniSound process $($process.Id) did not stop." }
            } finally { $process.Dispose() }
        }
    }
}
$mutexName = if ($lite) { 'Local\OmniSound.Install.Lite.' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value } else { 'Global\OmniSound.Install.Driver' }
$installMutex = New-Object Threading.Mutex($false,$mutexName)
$mutexOwned = $false
try { $mutexOwned = $installMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $mutexOwned = $true }
if (!$mutexOwned) { $installMutex.Dispose(); throw 'Another OmniSound installation is running. Wait for it to finish.' }
try {
Write-Output 'OMNISOUND_PHASE|5|설치 패키지와 기존 설치를 확인하는 중'
Assert-NoReparse $target
Assert-NoReparse $data
if ($Uninstall) {
    Write-Output 'OMNISOUND_PHASE|45|실행 중인 앱과 엔진을 종료하는 중'
    Stop-Product
    Write-Output 'OMNISOUND_PHASE|65|프로그램과 자동 실행 구성을 제거하는 중'
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-ItemProperty -LiteralPath $runKey -Name $taskName -ErrorAction SilentlyContinue
    if (!$lite) {
        if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) { Invoke-Checked sc.exe @('delete',$serviceName) }
        Get-PnpDevice | Where-Object InstanceId -Like 'SWD\OMNISOUND\*' | ForEach-Object { Invoke-Checked pnputil.exe @('/remove-device',$_.InstanceId) }
        Get-WindowsDriver -Online | Where-Object {
            $_.ProviderName -eq 'OmniSound' -and [IO.Path]::GetFileName($_.OriginalFileName) -ieq 'OmniSound.inf'
        } | ForEach-Object { Invoke-Checked pnputil.exe @('/delete-driver',$_.Driver,'/uninstall') }
    }
    if ($LocalTest -and !$lite) {
        $testCertificate = Join-Path $target 'licenses\OmniSoundLocalTest.cer'
        if (Test-Path -LiteralPath $testCertificate) {
            $thumbprint = (New-Object Security.Cryptography.X509Certificates.X509Certificate2($testCertificate)).Thumbprint
            foreach ($storeName in @('Root','TrustedPublisher')) {
                Remove-Item -LiteralPath "Cert:\LocalMachine\$storeName\$thumbprint" -Force -ErrorAction SilentlyContinue
            }
        }
    }
    Remove-Item -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $shortcut -ErrorAction SilentlyContinue
    # Delete only the installed file manifest. Configuration and measurements are retained.
    $removeFiles = $binaries + $supportFiles + $uiResources + @('LICENSE','THIRD_PARTY_NOTICES.md','README.md','Agreement.txt','licenses\WindowsAppSDK-License.txt','licenses\nlohmann-json-MIT.txt','licenses\InnoSetup-License.txt','install.mode')
    if (!$lite) { $removeFiles += $driverFiles + @('licenses\SysVAD-MIT.txt','licenses\OmniSoundLocalTest.cer') }
    foreach ($name in $removeFiles) {
        $path = [IO.Path]::GetFullPath((Join-Path $target $name))
        if (!$path.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Uninstall path escaped installation directory.' }
        if ($name -eq 'OmniSoundSetup.exe') { continue }
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    Remove-LegacyProShortcut
    Write-Output "OmniSound $Mode removed. User configuration has been retained for reinstallation."
    Write-Output 'OMNISOUND_PHASE|100|제거 완료'
    exit 0
}
Assert-NoReparse $Source
if (!$LocalTest -and !(Test-Path -LiteralPath (Join-Path $Source 'manifest.json'))) {
    throw 'Extract the full installation ZIP and run OmniSoundSetup.exe from that folder. The installed copy is for uninstallation only.'
}
$driver = Join-Path $Source 'driver'
# Every preflight happens before stopping the existing product or changing installed files.
$unsignedDevLite = $false
if ($LocalTest -and $lite) { throw 'Local test signing applies only to OmniSound Driver.' }
$testCertPath = Join-Path $Source 'licenses\OmniSoundLocalTest.cer'
if ($LocalTest) {
    if (!(Test-Path -LiteralPath $testCertPath)) { throw 'The local test certificate is missing.' }
    $publisher = New-Object Security.Cryptography.X509Certificates.X509Certificate2($testCertPath)
    if ($publisher.Subject -notmatch 'CN=OmniSound Local Test') { throw 'The local certificate has an unexpected identity.' }
    foreach ($name in $binaries) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $Source $name)
        if (!(Test-TestSignature $signature $publisher.Thumbprint)) { throw "Invalid local test signature: $name ($($signature.Status))" }
    }
    $catalog = Get-AuthenticodeSignature -LiteralPath (Join-Path $driver 'OmniSound.cat')
    if (!(Test-TestSignature $catalog $publisher.Thumbprint)) { throw 'The driver catalog is not signed by this local test certificate.' }
    $secureBoot = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction Stop).UEFISecureBootEnabled
    if ($secureBoot -ne 0) { throw 'Secure Boot is still enabled. Do not continue until it has been disabled manually for this test PC.' }
    $boot = bcdedit.exe /enum '{current}'
    if ($LASTEXITCODE -ne 0 -or ($boot -join "`n") -notmatch '(?im)^\s*testsigning\s+Yes\s*$') {
        throw 'Windows TESTSIGNING is not enabled. Set it from an elevated terminal and reboot before installing this test build.'
    }
} elseif ($lite -and (Test-UnsignedDevelopmentPackage)) {
    $unsignedDevLite = $true
} else {
    $publisher = (Get-AuthenticodeSignature -LiteralPath (Join-Path $Source 'OmniSoundSetup.exe')).SignerCertificate
    if (!$publisher) { throw 'The installer is not signed. Obtain a signed release package.' }
    foreach ($name in $binaries) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $Source $name)
        if ($signature.Status -ne 'Valid') { throw "Release executable is not authenticode signed: $name" }
        if ($signature.SignerCertificate.Thumbprint -ne $publisher.Thumbprint) { throw "Release publisher mismatch: $name" }
    }
}
foreach ($name in $supportFiles) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $Source $name)
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
        throw "Microsoft runtime support file is missing or has an invalid signature: $name"
    }
}
foreach ($name in $uiResources) {
    if (!(Test-Path -LiteralPath (Join-Path $Source $name))) { throw "WinUI XAML resource is missing: $name" }
}
if (!(Test-Path -LiteralPath $runtimeInstaller)) { throw 'Microsoft Windows App Runtime installer is missing from the package.' }
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $runtimeInstaller
if ($runtimeSignature.Status -ne 'Valid' -or $runtimeSignature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
    throw 'Microsoft Windows App Runtime installer has an invalid signature.'
}
$requiredDocs = @('LICENSE','THIRD_PARTY_NOTICES.md','README.md','licenses\WindowsAppSDK-License.txt','licenses\nlohmann-json-MIT.txt')
if (!$lite) { $requiredDocs += 'licenses\SysVAD-MIT.txt' }
foreach ($name in $requiredDocs) {
    if (!(Test-Path -LiteralPath (Join-Path $Source $name))) { throw "Package file is missing: $name" }
}
if (!$lite) {
    foreach ($file in $driverFiles) { if (!(Test-Path -LiteralPath (Join-Path $Source $file))) { throw "Driver file is missing: $file" } }
    $catalog = Get-AuthenticodeSignature -LiteralPath (Join-Path $driver 'OmniSound.cat')
    if ($LocalTest) {
        if (!(Test-TestSignature $catalog $publisher.Thumbprint)) { throw 'The development catalog signature is invalid.' }
    } elseif ($catalog.Status -ne 'Valid' -or $catalog.SignerCertificate.Subject -notmatch 'CN=Microsoft Windows') {
        throw 'A Microsoft-signed OmniSound driver catalog is required. Choose Lite to install without the kernel driver.'
    }
}
if ($LocalTest -and !$lite) {
    foreach ($storeName in @('Root','TrustedPublisher')) {
        $storePath = "Cert:\LocalMachine\$storeName\$($publisher.Thumbprint)"
        if (!(Test-Path -LiteralPath $storePath)) {
            Import-Certificate -FilePath $testCertPath -CertStoreLocation "Cert:\LocalMachine\$storeName" | Out-Null
        }
    }
    foreach ($name in $binaries + @('driver\OmniSound.cat')) {
        $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $Source $name)
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $publisher.Thumbprint) {
            throw "Local test signature validation failed after trusting its certificate: $name"
        }
    }
}
if (!(Get-AppxPackage -Name 'Microsoft.WindowsAppRuntime.1.8' -ErrorAction SilentlyContinue)) {
    Write-Output 'OMNISOUND_PHASE|30|Windows App Runtime을 설치하는 중'
    & $runtimeInstaller --quiet
    if ($LASTEXITCODE -notin @(0,3010)) { throw "Windows App Runtime installation failed ($LASTEXITCODE)." }
}
if (!$lite) { Invoke-Checked pnputil.exe @('/add-driver',(Join-Path $driver 'OmniSound.inf'),'/install') }
Write-Output 'OMNISOUND_PHASE|45|실행 중인 앱과 엔진을 종료하는 중'
Stop-Product
# Only SYSTEM and administrators may replace executables or service-owned configuration.
if (!$lite) {
    Protect-Directory $target $true
    Protect-Directory $data $false
    Protect-Directory (Join-Path $target 'driver') $true
    Protect-Directory (Join-Path $target 'licenses') $true
    Protect-Directory (Join-Path $data 'users') $false
}
if (!$lite) { foreach ($config in Get-ChildItem -LiteralPath (Join-Path $data 'users') -Force) {
    Assert-NoReparse $config.FullName
    if ($config.PSIsContainer) { throw 'Unexpected directory in the configuration store.' }
    $acl = Get-Acl -LiteralPath $config.FullName
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-18','S-1-5-32-544')) {
        throw 'Existing configuration has an unexpected owner. Restore the private configuration store before installation.'
    }
    foreach ($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq 'Allow' -and $rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')) {
            throw 'Existing configuration allows access outside SYSTEM/Administrators.'
        }
    }
} }
$backup = Join-Path $data ('previous-install\' + [Guid]::NewGuid().ToString('N'))
if (!$lite) { Protect-Directory $backup $false }
else {
    New-Item -ItemType Directory -Force -Path $target,$backup,(Join-Path $target 'licenses') | Out-Null
    Assert-NoReparse $target
}
$wasInstalled = if ($lite) { Test-Path -LiteralPath (Join-Path $target 'OmniSound.exe') } else { Test-Path -LiteralPath (Join-Path $target 'OmniSoundService.exe') }
$docs = @('LICENSE','THIRD_PARTY_NOTICES.md','README.md','licenses\WindowsAppSDK-License.txt','licenses\nlohmann-json-MIT.txt')
foreach ($optional in @('Agreement.txt','licenses\InnoSetup-License.txt')) {
    if (Test-Path -LiteralPath (Join-Path $Source $optional)) { $docs += $optional }
}
if (!$lite) { $docs += 'licenses\SysVAD-MIT.txt' }
if ($LocalTest -and !$lite) { $docs += 'licenses\OmniSoundLocalTest.cer' }
$managedFiles = $binaries + $supportFiles + $uiResources + $docs + @('install.mode')
if (!$lite) { $managedFiles += $driverFiles }
$previousFiles = @()
$previousTask = if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) { Export-ScheduledTask -TaskName $taskName } else { $null }
$previousRun = (Get-ItemProperty -LiteralPath $runKey -ErrorAction SilentlyContinue).$taskName
$previousRegistry = @{}
$registeredProperties = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
if ($registeredProperties) {
    foreach ($property in $registeredProperties.PSObject.Properties) {
        if ($property.Name -notlike 'PS*') { $previousRegistry[$property.Name] = $property.Value }
    }
}
$wasServiceInstalled = !$lite -and (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)
$previousShortcut = Test-Path -LiteralPath $shortcut
$filesChanged = $false
try {
Write-Output 'OMNISOUND_PHASE|55|이전 설치 파일을 백업하는 중'
foreach ($name in $managedFiles) {
    $old = Join-Path $target $name
    if (Test-Path -LiteralPath $old) {
        $saved = Join-Path $backup $name
        New-Item -ItemType Directory -Force -Path (Split-Path $saved) | Out-Null
        Copy-IfDifferent $old $saved
        $previousFiles += $name
    }
}
if ($previousShortcut) { Copy-IfDifferent $shortcut (Join-Path $backup 'start-menu.lnk') }
    Write-Output 'OMNISOUND_PHASE|65|프로그램 파일을 설치하는 중'
    $filesChanged = $true
    foreach ($name in $binaries) {
        $old = Join-Path $target $name
        Copy-IfDifferent (Join-Path $Source $name) $old
        if (!$unsignedDevLite) {
            $copied = Get-AuthenticodeSignature -LiteralPath $old
            if ($copied.Status -ne 'Valid' -or $copied.SignerCertificate.Thumbprint -ne $publisher.Thumbprint) { throw "Installed signature mismatch: $name" }
        }
    }
    foreach ($name in $supportFiles + $uiResources) {
        $old = Join-Path $target $name
        Copy-IfDifferent (Join-Path $Source $name) $old
        if ($name -in $supportFiles) {
            $copied = Get-AuthenticodeSignature -LiteralPath $old
            if ($copied.Status -ne 'Valid' -or $copied.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') { throw "Installed Microsoft runtime signature mismatch: $name" }
        }
    }
    if (!$lite) {
        New-Item -ItemType Directory -Force -Path (Join-Path $target 'driver') | Out-Null
        foreach ($name in $driverFiles) { Copy-IfDifferent (Join-Path $Source $name) (Join-Path $target $name) }
    }
    foreach ($name in $docs) { Copy-IfDifferent (Join-Path $Source $name) (Join-Path $target $name) }
    [IO.File]::WriteAllText((Join-Path $target 'install.mode'),$Mode,[Text.UTF8Encoding]::new($false))
    Write-Output 'OMNISOUND_PHASE|85|시작 메뉴와 백그라운드 실행을 구성하는 중'
    if (!$lite) {
        $binaryPath = '"' + (Join-Path $target 'OmniSoundService.exe') + '"'
        if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) { Invoke-Checked sc.exe @('config',$serviceName,'binPath=',$binaryPath,'start=','auto') }
        else { Invoke-Checked sc.exe @('create',$serviceName,'binPath=',$binaryPath,'start=','auto','DisplayName=','OmniSound Device Manager') }
        Invoke-Checked sc.exe @('failure',$serviceName,'reset=','86400','actions=','restart/3000/restart/10000/restart/30000')
        Start-Service -Name $serviceName
        (Get-Service -Name $serviceName).WaitForStatus('Running',[TimeSpan]::FromSeconds(20))
        $action = New-ScheduledTaskAction -Execute (Join-Path $target 'OmniSoundEngine.exe') -Argument '--login'
        $trigger = New-ScheduledTaskTrigger -AtLogOn
        $principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
    } else {
        $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        $action = New-ScheduledTaskAction -Execute (Join-Path $target 'OmniSoundEngine.exe') -Argument '--lite --login'
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $currentUser
        $principal = New-ScheduledTaskPrincipal -UserId $currentUser -LogonType Interactive -RunLevel Limited
    }
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -MultipleInstances Parallel -Hidden
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
    $startupPreference = (Get-ItemProperty -LiteralPath $startupPreferenceKey -ErrorAction SilentlyContinue).StartWithWindows
    if ($startupPreference -ne 0) {
        New-Item -Path $runKey -Force | Out-Null
        New-ItemProperty -LiteralPath $runKey -Name $taskName -Value ('"' + (Join-Path $target 'OmniSound.exe') + '" --background') -PropertyType String -Force | Out-Null
    } else {
        Remove-ItemProperty -LiteralPath $runKey -Name $taskName -ErrorAction SilentlyContinue
    }
    if ($lite) { Start-ScheduledTask -TaskName $taskName }
    New-Item -Path $uninstallKey -Force | Out-Null
    New-ItemProperty $uninstallKey DisplayName -Value $(if ($lite) { 'OmniSound Lite' } else { 'OmniSound Pro' }) -PropertyType String -Force | Out-Null
    New-ItemProperty $uninstallKey DisplayVersion -Value '0.2.0' -PropertyType String -Force | Out-Null
    New-ItemProperty $uninstallKey InstallLocation -Value $target -PropertyType String -Force | Out-Null
    New-ItemProperty $uninstallKey Publisher -Value $(if ($unsignedDevLite) { 'OmniSound development build' } else { 'OmniSound' }) -PropertyType String -Force | Out-Null
    $uninstallSwitch = if ($LocalTest) { '--local-test-uninstall' } elseif ($lite) { '--lite-uninstall' } else { '--driver-uninstall' }
    New-ItemProperty $uninstallKey UninstallString -Value ('"'+(Join-Path $target 'OmniSoundSetup.exe')+'" ' + $uninstallSwitch) -PropertyType String -Force | Out-Null
    New-ItemProperty $uninstallKey NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty $uninstallKey NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    $link.TargetPath = Join-Path $target 'OmniSound.exe'; $link.WorkingDirectory=$target; $link.Save()
    # Bound update-backup disk usage without risking a successful installation.
    try {
        $backupRoot = [IO.Path]::GetFullPath((Join-Path $data 'previous-install'))
        Assert-NoReparse $backupRoot
        $obsolete = Get-ChildItem -LiteralPath $backupRoot -Directory |
            Where-Object Name -Match '^[0-9a-f]{32}$' | Sort-Object CreationTimeUtc -Descending | Select-Object -Skip 3
        foreach ($directory in $obsolete) {
            $resolvedBackup = [IO.Path]::GetFullPath($directory.FullName)
            if (!$resolvedBackup.StartsWith($backupRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or
                $resolvedBackup -eq [IO.Path]::GetFullPath($backup)) { continue }
            Assert-NoReparse $resolvedBackup
            foreach ($entry in Get-ChildItem -LiteralPath $resolvedBackup -Recurse -Force) { Assert-NoReparse $entry.FullName }
            Remove-Item -LiteralPath $resolvedBackup -Recurse -Force
        }
    } catch { Write-Warning 'Old installation backups could not be cleaned. The installation succeeded.' }
    Write-Output 'OMNISOUND_PHASE|100|설치 완료'
    if ($lite) { Write-Output 'OmniSound Lite installed for the current user. No service or audio driver was installed.' }
    elseif ($LocalTest) { Write-Output 'LOCAL TEST BUILD. Open OmniSound Pro from the Start menu. Remove Secure Boot test configuration after testing.' }
    else { Write-Output 'OmniSound Pro installed. Open it from the Start menu to create a Windows audio output.' }
    try { Remove-LegacyProShortcut } catch { Write-Warning 'An old Pro shortcut could not be removed.' }
} catch {
    $installationError = $_
    Write-Output 'OMNISOUND_PHASE|90|설치 실패 — 이전 파일을 복원하는 중'
    try {
        Stop-Product
        if ($filesChanged) {
            foreach ($name in $managedFiles) {
                if ($name -in $previousFiles) {
                    $restoredPath = Join-Path $target $name
                    $savedPath = Join-Path $backup $name
                    if ((Test-Path -LiteralPath $restoredPath) -and
                        (Get-FileHash -LiteralPath $restoredPath).Hash -eq (Get-FileHash -LiteralPath $savedPath).Hash) { continue }
                    Copy-IfDifferent $savedPath $restoredPath
                }
                else { Remove-Item -LiteralPath (Join-Path $target $name) -Force -ErrorAction SilentlyContinue }
            }
            Remove-Item -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
            if ($previousRegistry.Count) {
                New-Item -Path $uninstallKey -Force | Out-Null
                foreach ($name in $previousRegistry.Keys) {
                    $kind = if ($previousRegistry[$name] -is [int]) { 'DWord' } else { 'String' }
                    New-ItemProperty -LiteralPath $uninstallKey -Name $name -Value $previousRegistry[$name] -PropertyType $kind -Force | Out-Null
                }
            }
            if ($previousShortcut) { Copy-IfDifferent (Join-Path $backup 'start-menu.lnk') $shortcut }
            else { Remove-Item -LiteralPath $shortcut -ErrorAction SilentlyContinue }
        }
        if ($previousRun) { New-ItemProperty -LiteralPath $runKey -Name $taskName -Value $previousRun -PropertyType String -Force | Out-Null }
        else { Remove-ItemProperty -LiteralPath $runKey -Name $taskName -ErrorAction SilentlyContinue }
        if ($previousTask) {
            Register-ScheduledTask -TaskName $taskName -Xml $previousTask -Force | Out-Null
            if ($lite) { Start-ScheduledTask -TaskName $taskName }
        } else { Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue }
        if ($wasInstalled -and !$lite) { Start-Service -Name $serviceName -ErrorAction SilentlyContinue }
        elseif (!$lite -and !$wasServiceInstalled -and (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)) { Invoke-Checked sc.exe @('delete',$serviceName) }
    } catch {
        throw "Installation failed: $($installationError.Exception.Message) Rollback also failed: $($_.Exception.Message). Backup: $backup"
    }
    throw $installationError
}
} finally {
    if ($mutexOwned) { $installMutex.ReleaseMutex() }
    $installMutex.Dispose()
}
