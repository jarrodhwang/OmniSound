# 복제 출력 음질 확인

OmniSound Lite는 Windows 기본 출력의 공유 모드 믹스를 복제합니다. 내부 형식은 **48 kHz 스테레오 float32 PCM**입니다. 압축 인코딩이나 MP3 변환은 하지 않습니다. 원본 파일의 샘플레이트가 더 높거나 출력 장치가 다른 형식을 사용하면 Windows의 형식 변환이 포함되므로, 원본 파일의 비트 단위 일치를 보장하는 모드는 아닙니다.

## 이번 개선

- 128탭 windowed-sinc 리샘플러와 인접 위상 보간, double 누산을 사용합니다. 사전에 계산한 필터는 모든 그룹이 공유하고, 오디오 처리 중에는 메모리를 할당하지 않습니다.
- 캡처 패킷 도착 때마다 재생 위치를 옮기는 대신, 초기 위치를 정한 뒤 연속된 시간축으로 재생합니다. 클럭 속도와 위상 보정은 천천히 바꿔 주기적인 피치 변동과 고주파 오차를 줄입니다.
- 캡처 주기에 맞는 최소 여유를 두고, 실제 버퍼 부족 때만 이를 늘립니다. 상한은 20ms입니다. 수동 지연 0ms는 추가 사용자 지연이 없다는 뜻이며, 캡처·보간·장치 버퍼까지 0ms라는 뜻은 아닙니다. 기존 200/400ms 고정 대기는 추가하지 않습니다.
- 누락·재연결·무음 전환과 볼륨 보정을 부드럽게 처리합니다. 무음 중에는 마지막 오디오를 다시 재생하지 않습니다. Windows 렌더 큐가 비면 제출 위치를 현재 하드웨어 시계에 맞춰 복구합니다.
- Windows 장치 볼륨이 적용되기 전에 float 샘플을 ±1로 잘라내던 처리를 제거했습니다. 정상적인 float 여유 범위는 유지하고, 비정상 NaN/Infinity는 무음으로 바꿉니다. 최종 출력 장치나 원본의 기존 클리핑을 복구할 수는 없습니다.
- 복제 스트림은 가능한 경우 RAW를 요청해 이미 처리된 기본 출력에 추가 음향 효과가 중복 적용되는 것을 줄입니다. 지원하지 않는 장치는 일반 공유 모드로 돌아갑니다. RAW도 항상 적용되는 장치·드라이버·하드웨어 처리를 모두 제거하지는 않습니다. [Microsoft RAW 계약](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/ne-audioclient-audclnt_streamoptions).

## 직접 확인하기

다른 앱의 소리를 멈추고, OmniSound에서 원하는 그룹을 선택합니다. 다음 명령은 낮은 레벨의 테스트음을 재생하며, 마이크를 사용하거나 녹음 파일을 저장하지 않습니다. 오디오는 분석하는 동안 메모리에만 있습니다.

```powershell
& "$env:LOCALAPPDATA\Programs\OmniSoundLite\OmniSoundProbe.exe" --quality-check
& "$env:LOCALAPPDATA\Programs\OmniSoundLite\OmniSoundProbe.exe" --quality-check-raw 60
```

첫 명령은 기본 출력의 일반 음향 처리를 포함합니다. 둘째 명령은 **진단용 테스트 스트림에만** RAW를 요청합니다. Windows 전체의 음향 효과 설정이나 다른 앱을 변경하지 않습니다. 결과의 `rawReferenceActive`로 적용 여부를 확인합니다. 검사 시간은 6–60초입니다.

997/1499Hz, 9973/12011Hz, 18013/19001Hz의 독립된 좌우 신호를 검사합니다. 100ms 창마다 DC·진폭·위상·장치 속도 차이를 분리해 잔여 오차를 계산합니다. `medianSignalToResidualDb`가 클수록 해당 테스트음에 대한 오차가 작습니다. 음질 등급이나 표준 THD+N 인증 수치는 아닙니다. `dropoutWindows`, `underrunBuffers`, `missingSourceFrames`, `renderStarvations`, `resynchronizations`도 함께 확인해야 합니다.

일반 테스트와 RAW 테스트의 기본 출력 결과가 크게 다르면 Windows 기본 장치의 음향 효과를 끈 상태와 비교하십시오. OmniSound가 복제하기 전에 이미 들어간 효과나 잡음은 복제 경로에서 원본으로 복구할 수 없습니다.

디지털 루프백은 전달 신호를 검사합니다. 스피커 앰프·케이블·DAC·Bluetooth 무선 구간의 치지직 소리는 직접 청취나 독립된 음향 측정으로 확인해야 합니다. 측정하지 않은 장치 조합에는 같은 결과를 적용하지 않습니다.
