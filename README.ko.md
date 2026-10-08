# OmniSound 0.2

[![최신 버전 다운로드](https://img.shields.io/badge/Download-Latest%20release-2ea44f?style=for-the-badge&logo=windows)](https://github.com/jarrodhwang/OmniSound/releases/latest/download/OmniSound-Setup.exe) · [이전 버전 보기](https://github.com/jarrodhwang/OmniSound/releases)

[English](README.md) · [한국어](README.ko.md)

C++20 · WinUI 3 · Windows 11 x64 다중 오디오 출력.

| 에디션 | 사용 방식 |
| --- | --- |
| **Lite** | Windows 기본 출력을 선택한 다른 장치로 복제. 드라이버·관리자 권한 없이 현재 사용자에게 설치. 저장 그룹 중 하나만 활성화. |
| **Pro** | `OmniSound · 그룹 이름` 가상 출력을 Windows에서 선택. 그룹별 독립 오디오, 스피커 위치, 스테레오 및 5.1/7.1 PCM 라우팅. 서명된 드라이버와 관리 서비스 필요. |

`dist/OmniSound-Setup.exe`는 로컬 개발 설치 파일입니다. 현재 표준 개발 설치 파일에서는
Lite만 설치할 수 있으며 Pro 선택 항목과 무인 Pro 설치가 비활성화되어 있습니다. Pro는
Microsoft 서명 드라이버와 게시자 서명이 적용된 릴리스 패키지가 준비되면 활성화됩니다.
Lite는 정상 Windows 보안 설정을 유지하며 설치할 수 있습니다. Pro의 가상 출력 드라이버를 이 PC에서 무료로 시험하려면
별도의 로컬 테스트 패키지와 Windows 테스트 모드가 필요합니다. 테스트 모드는 Secure Boot를
끄고 재부팅해야 하므로 일반 PC에서 보안 설정을 바꾸고 싶지 않다면 Lite를 사용하세요.
Pro를 일반 Windows 설치처럼 배포하려면 Microsoft 서명이 필요합니다. 무료 로컬 테스트와
되돌리기 절차는 [Pro 서명 및 설치 안내](docs/PRO_RELEASE.md)를 참고하세요. 빌드 성공은
실제 가상 출력·음향 동기화·정식 인증 완료를 의미하지 않습니다.

## 설치와 사용

1. `OmniSound-Setup.exe` 실행 → Lite 선택 → 사용권 동의 → 설치 위치 → 설치. Pro는 서명된
   정식 릴리스가 준비될 때까지 표준 개발 설치 프로그램에서 비활성화되어 있습니다.
2. 그룹을 추가하고 출력 장치의 왼쪽 체크박스를 켠 뒤 저장합니다. 행이나 이름을
   클릭하면 개별 설정만 선택됩니다. 사용 불가 장치는 **장치 더보기**로 확인합니다.
3. Lite는 Windows 기본 출력을 유지합니다. Pro는 Windows 소리 출력 또는 앱별
   출력에서 가상 그룹을 선택합니다. 소스가 다른 그룹으로 바뀌면 그 그룹이 재생됩니다.
4. **모든 출력 장치 볼륨 동기화**는 Lite의 기본 출력 또는 Pro의 가상 그룹과 각
   Windows 장치의 퍼센트를 맞춥니다. 50%를 두 번 곱하지 않습니다. 해당 실제 장치를
   사용하는 다른 앱의 볼륨에도 적용됩니다.
5. Pro의 **스피커 배치와 라우팅**에서 스테레오, 좌우 교환, 모노, 단일 채널 또는
   스피커 위치를 선택합니다. 마커를 끌면 위치 라우팅으로 전환됩니다. 5.1/7.1은
   Windows의 가상 그룹 스피커 구성과 소스 앱에서 해당 채널을 활성화해야 합니다.
6. PC 내장/USB 마이크를 청취 위치에 고정하고 **자동 보정**을 실행합니다. 녹음은
   저장하지 않습니다. 수동 추가 지연은 0~2000ms입니다. 헤드폰이나 Bluetooth 통화
   마이크는 자동 측정에 적합하지 않을 수 있습니다.

창을 닫으면 트레이에서 계속 재생합니다. 트레이의 **종료**는 엔진도 종료합니다.
설정에서 로그인 실행과 언어를 선택합니다. 한국어·영어·프랑스어·독일어·일본어를
지원하며 지원하지 않는 시스템 언어는 영어로 시작합니다. Windows 테마와 강조색을
사용합니다. 이름과 장치 식별자는 언어 변경으로 바뀌지 않습니다.

기존 설치는 등록된 경로에 업데이트합니다. 설치/제거 로그는 사용자 임시 폴더에
생성됩니다. Windows 설치된 앱에서 제거할 수 있으며 그룹 설정은 재설치를 위해
보관됩니다. Lite 설정은 `%LocalAppData%/OmniSound/lite-config.json`, Pro는
`%ProgramData%/OmniSound/users`입니다. 두 에디션은 별도 폴더로 함께 둘 수 있습니다.

## 오디오 범위

Pro 입력은 48kHz 스테레오/5.1/7.1 PCM16/24/32입니다. 내부에서는 48kHz float32로
계산하고 실제 출력에는 스테레오 WASAPI 공유 스트림을 사용합니다. 일반 스테레오
라우팅은 원본 좌우를 유지합니다. 다채널 입력은 스피커별 채널 선택, 위치 라우팅 또는
헤드룸을 확보한 다운믹스로 처리합니다. 없는 채널을 생성하지 않으며 LFE는 명시적으로
선택할 때만 전달합니다. RAW 출력을 요청하고 미지원 장치는 정상 모드로 돌아갑니다.

**Windows 오디오 처리**는 장치의 정상 Windows 처리를 허용합니다. 위치 기능은 PCM
배치이며 Dolby/Windows Sonic 동적 오브젝트 디코더나 자체 HRTF를 구현한 것은 아닙니다.
Windows 공간 음향은 장치별 Windows 설정에서 관리합니다.

128탭 보간과 IAudioClock/QPC로 독립 클럭 차이를 보정합니다. 새 Pro 그룹의 추가 버퍼는
0ms이며 고정 200/400ms 대기를 넣지 않습니다. 기존 명시적 버퍼는 유지합니다. 실제
장치·공유 모드·Bluetooth 버퍼 지연은 남습니다. Lite는 직접 재생되는 기본 출력을
늦출 수 없어 복제 장치와 완전한 동기화를 보장하지 못합니다. Pro는 느린 출력에 맞춥니다.

**1ms는 지정한 위치에서 반복 측정하는 목표**입니다. 과거 실측과 현재 클럭 추정을
구분합니다. 재연결, 형식/배치/지연 변경과 절전 복귀 후 재보정이 필요합니다. 실제 Pro
설치, Bluetooth 조합, 장시간 음향 시험과 HLK는 아직 완료되지 않았습니다. ASIO/독점
입력, 보호 콘텐츠, Wi-Fi 출력과 동시 원격 세션은 지원 범위 밖입니다.

## 개발 빌드

Visual Studio 2026 C++/CMake와 PowerShell 7을 사용합니다. 고정 버전 도구를
`tools/cache`에 복원하며 시스템 보안 설정을 변경하지 않습니다.

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

`Package.ps1 -LiteOnly`는 드라이버를 포함하지 않아 WDK 빌드가 필요 없습니다.
`OmniSoundSetup.exe`는 내부 설치/복구 도구이며 사용자용 설치 파일은 단일
`OmniSound-Setup.exe`입니다. 무인 설치 인수는
`/MODE=lite /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="C:\path\setup.log"`입니다.

| Build output | Purpose |
| --- | --- |
| `build/windows-x64/Release/OmniSound.exe` | Shared WinUI settings and tray |
| `OmniSoundEngine.exe` | User audio process |
| `OmniSoundService.exe` | Pro privileged device/configuration service |
| `OmniSoundProbe.exe` | Device/engine/digital diagnostics |
| `OmniSoundSetup.exe` | Provisioning helper |
| `build/driver/Release/OmniSoundAudio.sys` | Unsigned development driver |

[Pro signing and release steps](docs/PRO_RELEASE.md), [validation](docs/VALIDATION.md),
[architecture/quality](docs/ARCHITECTURE.md), [audio diagnostics](docs/AUDIO_QUALITY.md).

## 라이선스

OmniSound는 오픈소스입니다. 프로젝트에서 작성한 원본 코드·에셋·문서는 MIT 라이선스를 따릅니다. Microsoft SysVAD 기반 코드를 포함한 `driver/` 전체는 Microsoft Public License(MS-PL)를 따릅니다. 다른 서드파티 구성 요소는 각자의 라이선스를 유지합니다. 자세한 내용은 [LICENSE](LICENSE), [라이선스 범위](LICENSES.md), [서드파티 고지](THIRD_PARTY_NOTICES.md)를 참고하세요.
