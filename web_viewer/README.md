# Boat Capture Viewer

언리얼 엔진에서 한 레벨을 플레이하는 동안 저장한 Color·Depth 이미지 시퀀스를 시간 순서대로 재생하고, 단일 바이너리의 접근 성능을 측정하는 Part 2b·2c 웹 뷰어입니다.

별도의 웹 서버, 패키지 설치, 빌드 과정 없이 사용할 수 있습니다.

## 실행 방법

1. 언리얼 에디터에서 캡처 기능이 포함된 레벨을 한 번 플레이합니다.
2. Chrome 또는 Edge에서 `web_viewer/index.html`을 엽니다.
3. 다음 중 한 가지 방법으로 세션을 선택합니다.
   - Part 2b: `세션 폴더 선택`으로 타임스탬프 폴더 하나를 선택합니다.
   - Part 2c: `capture.boatbin 선택`으로 세션 안의 바이너리 파일을 선택합니다.

```text
boat_sim/Saved/BoatCaptures/<세션 시각>
```

예시:

```text
boat_sim/Saved/BoatCaptures/20260809_205310_636
```

`BoatCaptures` 폴더 전체를 선택하면 여러 `manifest.json`이 포함되므로 불러오지 않습니다. 반드시 한 번의 레벨 플레이에 해당하는 타임스탬프 폴더 하나를 선택해야 합니다.

## 지원 형식

폴더 입력은 `BoatCaptureComponent`가 생성하는 Manifest `1.1`만 지원한다. 바이너리 입력은 별도의 Binary Format `1`만 지원한다.

```text
<세션 폴더>/
├── manifest.json
├── capture.boatbin
├── color/
│   ├── frame_000000.png
│   └── ...
└── depth/
    ├── frame_000000.png
    └── ...
```

Manifest의 각 프레임에는 다음 정보가 필요합니다.

```json
{
  "index": 0,
  "timestamp_ms": 3.0188,
  "color": "color/frame_000000.png",
  "depth": "depth/frame_000000.png"
}
```

컬러와 Depth 파일이 모두 존재하는 프레임만 재생합니다. 파일 누락, 프레임 수 불일치, 캡처 누락이나 저장 실패는 화면 위쪽 경고 영역에 표시합니다.

`capture.boatbin`은 동일한 PNG Payload와 재생 Metadata·Frame Index를 한 파일에 포함한다. 바이너리를 선택한 경우 외부 Manifest와 PNG 폴더를 읽지 않는다.

## Depth Map 표시

Depth PNG는 언리얼 캡처 단계에서 이미 RGB 0–255 그레이스케일로 저장됩니다. 웹 뷰어는 CSS 필터나 추가 변환을 적용하지 않고 저장된 값을 그대로 표시합니다.

- 가까운 영역: `255`, 흰색
- 먼 영역: `0`, 검은색
- 최대 거리: Manifest의 `depth_max_cm`
- 현재 기본 최대 거리: `5000cm`

Depth 화면 오른쪽 아래의 범례에서 이 인코딩을 확인할 수 있습니다. Color와 Depth는 두 이미지의 디코딩이 모두 완료된 뒤 같은 프레임끼리 동시에 교체됩니다.

## 재생 조작

화면 아래에서 다음 기능을 사용할 수 있습니다.

- 첫 프레임
- 이전 프레임
- 재생·일시 정지
- 다음 프레임
- 마지막 프레임
- 타임라인 탐색

키보드 단축키:

| 키 | 동작 |
|---|---|
| `Space` | 재생·일시 정지 |
| `←` | 이전 프레임 |
| `→` | 다음 프레임 |
| `Home` | 첫 프레임 |
| `End` | 마지막 프레임 |

수동으로 프레임을 이동하면 재생은 일시 정지합니다. 마지막 프레임에서 재생 버튼을 다시 누르면 처음부터 재생합니다.

## 재생 시간 기준

Manifest의 `timestamp_ms`가 모두 증가하면 첫 프레임을 0ms로 맞춘 실제 캡처 시간을 사용합니다. 따라서 real-time 100ms 목표 주기에서 발생한 작은 시간 차이도 재생에 반영됩니다.

타임스탬프가 중복되거나 역전되면 세션 전체를 `capture_interval_ms` 간격으로 재구성합니다.

브라우저 렌더링이 늦어진 경우 밀린 프레임을 빠르게 몰아서 표시하지 않습니다. 현재 시간에 해당하는 프레임으로 이동해 전체 재생 시간이 계속 밀리지 않게 합니다.

## 오류와 경고

다음 상황에서는 세션 로드를 중단합니다.

- Manifest가 없거나 두 개 이상인 경우
- Manifest 버전이 `1.1`이 아닌 경우
- 필수 메타데이터가 잘못된 경우
- 프레임 인덱스가 중복된 경우
- 절대 경로나 상위 폴더 이동 경로가 포함된 경우
- 유효한 Color·Depth 프레임 쌍이 하나도 없는 경우

이미지 디코딩이 실패하면 재생을 일시 정지하고 마지막 정상 프레임을 유지합니다. 오류 메시지에는 문제가 생긴 프레임 인덱스가 표시됩니다.

## 자동 테스트

저장소 루트에서 다음 명령을 실행합니다.

```powershell
node --test web_viewer/tests/viewer-core.test.cjs
```

Node.js 18 이상에서 다음 핵심 동작을 검사합니다.

- Manifest 1.1 검증
- 안전한 상대 경로 검사
- 숫자 프레임 정렬과 중복 검출
- Color·Depth 파일 쌍 구성
- 타임스탬프 재생 시간 계산
- 현재 시간에 해당하는 프레임 이진 탐색
- 누락 및 캡처 상태 경고
- Binary Format 1 Prefix·Metadata·Index 파싱
- 잘못된 Magic·버전·Offset·잘린 파일 거부
- 랜덤 접근 순서의 재현성과 중복 방지

## Part 2c 접근 성능 측정

`capture.boatbin`을 선택하면 화면 아래에 `바이너리 이미지 접근 성능` 영역이 나타난다.

1. `성능 측정`을 누른다.
2. 순차·랜덤 접근의 `Total ms/image`를 확인한다.
3. `결과 JSON 저장`을 눌러 측정 환경과 결과를 보관한다.

측정값은 바이너리 범위 읽기와 PNG 디코딩을 합친 이미지 한 장당 평균 시간이다. DOM 화면 갱신 시간은 포함하지 않는다. Color와 Depth를 각각 한 장으로 계산하며 두 방식 모두 전체 이미지를 두 번씩 처리한다.

최종 보고서에는 저장한 JSON의 다음 값을 기재한다.

- `sequential.averageTotalMs`
- `random.averageTotalMs`

## 제출 시 참고

`boat_sim/Saved`는 `.gitignore` 대상이므로 실제 캡처 이미지는 저장소에 포함되지 않습니다. 웹 뷰어 소스만 제출하고, 평가 시 언리얼에서 새 캡처 세션을 생성해 선택합니다.

Part 2c도 별도 서버 없이 로컬 `File`·`Blob` API로 동작한다. `capture.boatbin`은 외부 Manifest 없이 단독으로 재생할 수 있다.
