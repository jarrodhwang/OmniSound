# OmniSound 0.2

[![Download the latest release](https://img.shields.io/badge/Download-Latest%20release-2ea44f?style=for-the-badge&logo=windows)](https://github.com/jarrodhwang/OmniSound/releases/latest/download/OmniSound-Setup.exe) · [Previous versions](https://github.com/jarrodhwang/OmniSound/releases)

[English](README.md) · [한국어](README.ko.md)

C++20 · WinUI 3 · Multi-output audio for Windows 11 x64.

| Edition | How it works |
| --- | --- |
| **Lite** | Mirrors Windows' default output to selected devices. Installs for the current user without a driver or administrator privileges. Only one saved group can be active. |
| **Pro** | Select a virtual `OmniSound · group name` output in Windows. Provides independent audio per group, speaker positioning, and stereo or 5.1/7.1 PCM routing. Requires a signed driver and a privileged service. |

`dist/OmniSound-Setup.exe` is a local development installer. The standard development installer currently supports Lite only; the Pro selection and unattended Pro installation are disabled. Pro will be enabled when a release package with a Microsoft-signed driver and publisher-signed application is ready. Lite installs with normal Windows security settings. Testing the Pro virtual output driver locally requires a separate test package and Windows test mode. Test mode requires disabling Secure Boot and restarting, so use Lite if you do not want to change your PC's security settings. Microsoft signing is required to distribute Pro like a regular Windows application. See [Pro signing and installation](docs/PRO_RELEASE.md) for local testing and rollback steps. A successful build does not confirm that virtual output, audio synchronization, or formal certification has passed.

## Installation and use

1. Run `OmniSound-Setup.exe` → choose Lite → accept the license → choose an install location → install. Pro is disabled in the standard development installer until a signed release is ready.
2. Add a group, check the box to the left of each output device, and save. Clicking a row or name only selects its settings. Find unavailable devices under **Show more devices**.
3. Lite keeps Windows' default output. With Pro, select a virtual group in Windows sound settings or an app's output settings. Audio plays through the group selected by the source app.
4. **Sync all output device volumes** matches the Lite default output or Pro virtual group volume to each Windows device's percentage. It does not multiply 50% twice. It also affects the volume of other apps using those physical devices.
5. In Pro's **Speaker layout and routing**, choose stereo, swap left and right, mono, a single channel, or speaker positions. Dragging a marker switches to position routing. For 5.1/7.1, enable those channels in the Windows virtual group speaker configuration and in the source app.
6. Place a built-in or USB microphone at the listening position and run **Auto calibration**. Recordings are not saved. Manual additional delay ranges from 0 to 2000 ms. Headset or Bluetooth call microphones may not be suitable for automatic measurement.

Closing the window leaves playback running in the system tray. **Quit** in the tray also stops the engine. Choose launch-at-sign-in and the language in Settings. The app supports Korean, English, French, German, and Japanese; unsupported system languages start in English. The app follows the Windows theme and accent color. Names and device identifiers do not change when you switch languages.

Existing installations update in their registered location. Install and uninstall logs are created in the user's temporary folder. You can uninstall the app from Windows Installed apps; group settings are kept for reinstall. Lite settings are stored in `%LocalAppData%/OmniSound/lite-config.json`; Pro settings are in `%ProgramData%/OmniSound/users`. Lite and Pro can be installed side by side in separate folders.

## Audio scope

Pro input supports 48 kHz stereo/5.1/7.1 PCM16/24/32. Audio is processed internally as 48 kHz float32, with stereo WASAPI shared-mode streams for physical outputs. Standard stereo routing preserves the source left and right channels. Multichannel input uses per-speaker channel selection, position routing, or a headroom-aware downmix. The app does not create missing channels, and forwards LFE only when explicitly selected. RAW output is requested; unsupported devices fall back to normal mode.

**Windows audio processing** allows the device's normal Windows processing. Position features route PCM channels; they do not implement a Dolby/Windows Sonic dynamic-object decoder or their own HRTF. Configure Windows spatial sound separately for each device in Windows settings.

The app uses 128-tap interpolation and IAudioClock/QPC to compensate for independent device clocks. New Pro groups add 0 ms of buffering by default, without a fixed 200/400 ms wait. Existing explicit buffer settings are retained. Physical-device, shared-mode, and Bluetooth buffer latency remain. Lite cannot delay the directly played default output, so it cannot guarantee perfect synchronization with mirrored devices. Pro aligns playback to the slower output.

**1 ms is a target measured repeatedly at a specified position.** Historical measurements are distinguished from current clock estimates. Recalibration is needed after reconnecting devices, changing format/layout/delay, or resuming from sleep. Pro installation, Bluetooth combinations, long-duration audio testing, and HLK testing are not yet complete. ASIO/exclusive-mode input, protected content, Wi-Fi outputs, and simultaneous remote sessions are outside the supported scope.

## Development build

Use Visual Studio 2026 C++/CMake and PowerShell 7. The scripts restore pinned tool versions to `tools/cache` and do not change system security settings.

```powershell
.\tools\Restore-WinUI.ps1
.\tools\Restore-DriverTools.ps1  # Pro SDK/WDK 28000.2526
.\tools\Restore-InstallerTools.ps1  # Inno Setup 7.1.0 portable
.\tools\Build.ps1 -Test
.\tools\Build-Driver.ps1
.\tools\Package.ps1
# Use the package folder printed above, not its ZIP path:
.\tools\Build-Installer.ps1 -PackageDirectory 'C:\absolute\package-directory'
```

`Package.ps1 -LiteOnly` excludes the driver and does not require a WDK build. `OmniSoundSetup.exe` is an internal installation/recovery tool; the single user-facing installer is `OmniSound-Setup.exe`. Unattended installation arguments are:

```text
/MODE=lite /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="C:\path\setup.log"
```

| Build output | Purpose |
| --- | --- |
| `build/windows-x64/Release/OmniSound.exe` | Shared WinUI settings and system tray |
| `OmniSoundEngine.exe` | User audio process |
| `OmniSoundService.exe` | Pro privileged device/configuration service |
| `OmniSoundProbe.exe` | Device/engine/digital diagnostics |
| `OmniSoundSetup.exe` | Provisioning helper |
| `build/driver/Release/OmniSoundAudio.sys` | Unsigned development driver |

[Pro signing and release steps](docs/PRO_RELEASE.md), [validation](docs/VALIDATION.md), [architecture and quality](docs/ARCHITECTURE.md), [audio diagnostics](docs/AUDIO_QUALITY.md).

## Licensing

OmniSound is open source. Original project code, assets, and documentation are licensed under MIT. The `driver/` tree, including Microsoft SysVAD-derived code, is licensed under the Microsoft Public License (MS-PL). Other third-party components retain their own licenses. See [LICENSE](LICENSE), [license scope](LICENSES.md), and [third-party notices](THIRD_PARTY_NOTICES.md).
