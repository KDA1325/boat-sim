# Part 2b 웹 뷰어 구현 계획

## Summary

Part 2a에서 생성한 한 레벨의 Color·Depth 이미지 시퀀스를 브라우저에서 연속 재생하는 웹 뷰어를 구현한다.

현재 캡처 결과는 `boat_sim/Saved/BoatCaptures/<세션 시각>` 아래에 Manifest와 Color·Depth PNG로 저장된다. 웹 뷰어는 사용자가 세션 폴더 하나를 선택하면 Manifest 1.1을 읽고 같은 인덱스의 두 이미지를 나란히 재생한다.

- 해상도: `1280×720`
- 목표 저장 주기: real-time `100ms`
- Manifest 버전: `1.1`
- Color: 톤 매핑된 RGB PNG
- Depth: RGB `0–255` 그레이스케일 PNG
- Depth 표현: 가까운 곳 `255`, 먼 곳 `0`
- 기본 최대 깊이: `5000cm`

별도 서버, 패키지 설치, 프레임워크, CDN 없이 `index.html`을 Chrome 또는 Edge에서 직접 실행할 수 있게 한다.

## Implementation Changes

### 웹 프로젝트 구조

저장소 루트에 `web_viewer`를 추가하고 화면, 스타일, 핵심 로더, 브라우저 제어, 테스트, 사용 설명을 분리한다.

```text
web_viewer/
├── index.html
├── styles.css
├── viewer-core.js
├── app.js
├── README.md
└── tests/
    └── viewer-core.test.cjs
```

`file://` 실행을 위해 ES Module과 `fetch()`를 사용하지 않는다. `viewer-core.js`는 브라우저에서는 `window.BoatViewerCore`, Node.js에서는 `module.exports`로 제공한다.

### 폴더 및 Manifest 로딩

- `<input type="file" webkitdirectory multiple>`로 타임스탬프 세션 폴더 하나를 선택한다.
- `webkitRelativePath`의 경로 구분자를 정규화하고 Manifest 기준 상대 경로로 파일을 연결한다.
- `manifest.json`이 없거나 여러 개이면 명확한 오류를 표시한다.
- Manifest 버전은 `1.1`만 지원하고 다른 버전은 거부한다.
- 해상도, 캡처 간격, 최대 깊이, Depth 인코딩, 프레임 수와 프레임별 필드를 검증한다.
- 절대 경로, URL, 드라이브 경로, `..` 상위 폴더 이동을 거부한다.
- 프레임 인덱스를 숫자로 정렬하고 중복 인덱스는 오류 처리한다.
- Color와 Depth가 모두 존재하는 프레임만 재생한다.
- 파일 누락, 선언 프레임 수 불일치, 미참조 PNG, 캡처 누락과 실패 횟수는 경고로 표시한다.

폴더 로더는 `FolderSequenceLoader.load(FileList)` 인터페이스를 제공하고 다음 재생 세션을 반환한다.

```text
PlaybackSession
├── sessionName
├── metadata
│   ├── version, width, height
│   ├── captureIntervalMs, depthMaxCm, depthEncoding
│   ├── declaredFrameCount
│   ├── gpuReadback, readbackBufferCount
│   ├── droppedCaptureCount, failedCaptureCount
│   └── captureSource, colorToneMapping
├── frames[]
│   ├── index, timestampMs, playbackTimeMs
│   └── colorFile, depthFile
└── warnings[]
```

### 시간 기반 재생

- `timestamp_ms`가 모두 증가하면 첫 프레임을 0ms로 정규화한 실제 캡처 시간을 사용한다.
- 시간이 중복되거나 역전되면 세션 전체를 `capture_interval_ms` 간격으로 재구성한다.
- `requestAnimationFrame()` 타임스탬프를 재생 시계로 사용한다.
- 현재 시간에 해당하는 프레임을 이진 탐색한다.
- 렌더링이 늦으면 과거 프레임을 몰아서 표시하지 않고 현재 시간에 해당하는 프레임으로 이동한다.
- 백그라운드 탭으로 이동하면 재생을 일시 정지해 복귀 시 프레임 폭주를 방지한다.

### 이미지 디코딩과 캐시

- 선택한 `File`을 `URL.createObjectURL()`로 이미지에 연결한다.
- 한 프레임의 Color와 Depth를 `HTMLImageElement.decode()`로 함께 디코딩한다.
- 두 이미지가 모두 준비된 뒤 같은 화면 갱신에서 동시에 교체한다.
- 현재 기준 이전 1개, 현재 1개, 다음 4개로 최대 6프레임 쌍을 캐시한다.
- 캐시 제거, 세션 변경, 페이지 종료 시 Object URL을 해제한다.
- 렌더 요청 토큰으로 늦게 완료된 이전 비동기 결과가 새 화면을 덮지 않게 한다.
- 이미지 손상 시 재생을 멈추고 마지막 정상 프레임과 오류 정보를 유지한다.

### Depth Map 시각화

과제의 “Depth Map을 0–255 범위 RGB로 시각화해 사람이 형태를 분간할 수 있게 표시”하는 조건을 명시적으로 구현한다.

- 언리얼에서 RGB8 그레이스케일로 저장된 Depth PNG를 추가 변환 없이 표시한다.
- 가까운 영역은 `255`에 가까운 흰색, 먼 영역은 `0`에 가까운 검은색으로 표시한다.
- `depth_max_cm` 이상의 영역은 현재 인코딩에 따라 `0`으로 표시한다.
- CSS 필터나 후처리로 원본 명암을 변경하지 않는다.
- Depth 패널에 `Near=255`, `Far=0`, `Max Depth=<manifest 값>` 범례를 제공한다.
- Color와 Depth는 항상 같은 프레임 인덱스로 표시한다.

### 화면과 조작

- 데스크톱에서는 Color와 Depth를 같은 크기의 16:9 패널로 좌우 배치한다.
- 900px 미만 화면에서는 위아래로 배치한다.
- 세션명, 버전, 해상도, 프레임 수, 저장 간격, 전체 시간, GPU Readback, 누락·실패 횟수를 표시한다.
- 처음, 이전, 재생·일시 정지, 다음, 마지막, 타임라인 탐색을 제공한다.
- 현재 순번, Manifest 인덱스, 타임스탬프와 재생 시간을 표시한다.
- 세션 로드 후 첫 프레임이 준비되면 자동 재생한다.
- 수동 탐색 시 일시 정지하고, 마지막 프레임에서 다시 재생하면 처음부터 시작한다.
- `Space`, 방향키, `Home`, `End` 단축키를 지원한다.

## Test Plan

Node.js 18에서 다음 명령으로 핵심 로직을 검증한다.

```powershell
node --test web_viewer/tests/viewer-core.test.cjs
```

- Manifest 1.1 정상 파싱 및 다른 버전 거부
- 필수 값과 안전한 상대 경로 검증
- 숫자 인덱스 정렬과 중복 검출
- Color·Depth 누락 프레임 필터링
- 프레임 수, 미참조 파일, 캡처 누락·실패 경고
- 정상 시간 정규화와 비정상 시간 재구성
- 재생 시간에 해당하는 프레임 이진 탐색
- 폴더에서 단일 Manifest와 파일 쌍 로드

실제 `20260809_205310_636` 세션에서는 Manifest 1.1, 500개 프레임, 약 50.207초 재생 시간, Depth 인코딩을 확인한다. Chrome과 Edge에서 `index.html` 직접 실행, 폴더 선택, Color·Depth 동기 재생, 컨트롤, 반응형 화면을 확인한다.

## Completion Criteria

- 설치와 서버 없이 `index.html`을 직접 실행할 수 있다.
- Manifest 1.1 세션 폴더 하나를 읽고 프레임을 시간 순서대로 재생한다.
- Color와 Depth가 동일 인덱스로 동시에 표시된다.
- Depth가 RGB 0–255 명암과 명확한 범례로 표시되어 장면 형태를 구분할 수 있다.
- 렌더링 지연 시 전체 재생 시간이 지속해서 밀리지 않는다.
- 파일과 Manifest 오류를 사용자에게 설명한다.
- 자동 테스트와 Chrome 초기 화면 검증을 통과한다.
- README만 보고 비개발자도 실행할 수 있다.

## Assumptions

- 제출 Manifest는 현재 `BoatCaptureComponent`가 생성하는 버전 1.1이다.
- Manifest 1.0 호환은 구현하지 않는다.
- Depth PNG는 언리얼 단계에서 이미 RGB 0–255로 변환된다.
- `Saved/BoatCaptures`는 Git에 포함하지 않고 평가 시 새 세션을 생성한다.
- 지원 브라우저는 최신 Chrome과 Edge이다.
- Part 2c 서버 연동, Part 1 코드, 언리얼 캡처 코드는 변경하지 않는다.
