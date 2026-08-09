# Part 2b 웹 뷰어 구현 과정

## 1. 작업 목적

언리얼 엔진의 `BoatCaptureComponent`가 저장한 Color·Depth 이미지 시퀀스를 별도 서버 없이 브라우저에서 연속 재생하도록 구현했다.

과제의 핵심 조건은 다음 두 가지다.

1. 한 레벨 플레이 동안 저장한 프레임을 시간 순서대로 연속 재생한다.
2. Depth Map을 RGB 0–255 명암으로 표시해 사람이 장면 형태를 구분할 수 있게 한다.

## 2. 시작 시점의 프로젝트 상태

Part 2a 캡처 기능은 이미 다음 형식으로 결과를 저장하고 있었다.

```text
Saved/BoatCaptures/<세션 시각>/
├── manifest.json
├── color/frame_XXXXXX.png
└── depth/frame_XXXXXX.png
```

현재 Manifest 형식은 버전 1.1이다.

- 해상도: 1280×720
- 캡처 목표 간격: real-time 100ms
- 렌더 소스: SceneColor HDR RGB와 SceneDepth A
- 컬러 변환: ACES fitted와 sRGB
- Depth 변환: RGB8, near=255, far=0
- GPU 읽기: `FRHIGPUTextureReadback` 비동기 처리
- Readback 버퍼: 3개

웹 뷰어 구현 전에는 저장소에 `web_viewer` 폴더가 없었다. Node.js는 18.13.0이며, 제출 환경에서 추가 설치 없이 실행할 수 있도록 HTML·CSS·일반 JavaScript만 사용하기로 했다.

## 3. Manifest 로더와 타임라인 분리

첫 단계에서는 브라우저 화면보다 먼저 `viewer-core.js`에 순수 로직을 분리했다.

### 3.1 Manifest 1.1 전용 검증

기존 결과 호환은 필요하지 않으므로 버전 1.1만 허용했다. 잘못된 세션이 재생 도중 문제를 만들지 않도록 다음 값을 로드 시점에 검사한다.

- 해상도, 캡처 간격, 최대 깊이
- Depth 인코딩
- 선언 프레임 수
- 프레임 인덱스와 타임스탬프
- Color·Depth 상대 경로

드라이브 경로, 절대 경로, URL, `..` 이동은 허용하지 않는다. 브라우저가 제공한 폴더 밖의 파일을 Manifest가 참조하지 못하게 하기 위한 처리다.

### 3.2 프레임 쌍 구성

프레임은 파일명의 문자열 순서가 아니라 Manifest `index` 숫자로 정렬한다. Color와 Depth 중 한쪽이 없으면 두 화면의 시점이 어긋날 수 있으므로 해당 프레임 전체를 재생 목록에서 제외한다.

재생을 막지 않아도 되는 문제는 경고로 수집한다.

- 선언 프레임 수 불일치
- Color 또는 Depth 파일 누락
- Manifest가 참조하지 않는 PNG
- GPU Readback 누락
- 이미지 저장 실패

### 3.3 실제 시간 기반 재생

정상 세션에서는 다음 계산으로 첫 프레임을 0ms에 맞춘다.

```text
playbackTimeMs = timestampMs - firstTimestampMs
```

타임스탬프가 중복되거나 역전되면 일부 프레임만 보정하지 않고 세션 전체를 `capture_interval_ms` 기준으로 재구성한다.

현재 재생 시간에 맞는 프레임은 이진 탐색한다. 렌더링이 잠시 느려져도 밀린 프레임을 모두 그리지 않고 현재 시간에 해당하는 프레임으로 이동하기 위한 구조다.

### 3.4 Node.js 18 테스트 문제

Node.js 18.13의 초기 `node:test` 러너가 한글 테스트 이름을 TAP 출력으로 처리하는 과정에서 `ERR_TAP_LEXER_ERROR`를 발생시켰다.

제품 코드의 한글 오류 메시지와 주석은 유지하고 테스트 이름만 영문으로 변경했다. 이후 11개 핵심 테스트가 모두 통과했다.

이 단계의 커밋:

```text
d4283dd feat: 캡처 세션 로더와 재생 시간 계산 추가
```

## 4. 브라우저 재생 화면 구현

### 4.1 서버 없는 폴더 선택

`<input webkitdirectory multiple>`로 캡처 세션 폴더를 선택한다. `fetch()`나 ES Module을 사용하지 않으므로 `index.html`을 `file://` 주소로 직접 열 수 있다.

Manifest가 정확히 하나일 때만 세션을 로드한다. `BoatCaptures` 상위 폴더를 선택해 여러 세션이 들어오면 타임스탬프 폴더 하나를 다시 선택하도록 안내한다.

### 4.2 Color·Depth 원자적 교체

두 화면이 서로 다른 시점을 표시하지 않도록 한 프레임을 다음 순서로 처리한다.

```text
Color File  → Object URL → Image.decode()
Depth File  → Object URL → Image.decode()
                         ↓
                 두 디코딩 모두 완료
                         ↓
                 같은 화면 갱신에서 교체
```

사용자가 빠르게 프레임을 탐색하면 이전 디코딩 요청이 늦게 완료될 수 있다. 각 표시 요청에 토큰을 부여해 최신 요청이 아닌 결과는 화면에 적용하지 않는다.

### 4.3 제한된 미리 읽기 캐시

부드러운 재생을 위해 다음 6프레임 쌍을 미리 디코딩한다.

```text
이전 1 + 현재 1 + 다음 4
```

캐시 제한을 넘으면 가장 오래 사용하지 않은 프레임부터 제거한다. 제거할 때 `URL.revokeObjectURL()`을 호출해 원본 File과 디코딩 이미지 참조가 계속 쌓이지 않게 했다.

새 세션 선택, 페이지 종료, 로드 실패 시에도 모든 Object URL을 해제한다.

### 4.4 재생 제어

`requestAnimationFrame()`의 시간값을 기준으로 재생한다. 브라우저가 느린 프레임에서는 현재 시간에 해당하는 이미지로 건너뛰며, 탭이 백그라운드로 이동하면 자동으로 일시 정지한다.

다음 조작을 구현했다.

- 처음, 이전, 재생·일시 정지, 다음, 마지막
- 프레임 단위 타임라인
- Space, 방향키, Home, End
- 마지막 프레임에서 다시 재생할 경우 첫 프레임부터 시작

이 단계의 커밋:

```text
65f6014 feat: 컬러와 깊이 프레임 동기 재생 뷰어 추가
```

## 5. Depth Map 표시

Depth 이미지는 언리얼에서 이미 RGB8 PNG로 저장되므로 웹에서 다시 깊이값을 계산하지 않는다.

- Near: 255, 흰색
- Far: 0, 검은색
- Max Depth: Manifest `depth_max_cm`

원본 명암을 유지하기 위해 Depth 이미지에 CSS 필터를 적용하지 않았다. 화면에는 RGB 0–255 배지와 흰색에서 검은색으로 이어지는 범례를 제공한다.

이를 통해 과제에서 요구하는 “Depth Map을 0–255 범위 RGB로 시각화해 사람이 형태를 분간할 수 있게 표시”하는 조건을 캡처 결과와 일관된 방식으로 충족한다.

## 6. 검증 과정과 결과

### 6.1 자동 테스트

실행 명령:

```powershell
node --test web_viewer/tests/viewer-core.test.cjs
```

결과:

```text
tests 11
pass 11
fail 0
```

검증 범위:

- Manifest 1.1 전용 검증
- 안전하지 않은 상대 경로 거부
- 숫자 프레임 정렬
- 중복 인덱스 거부
- Color·Depth 파일 쌍 구성
- 누락·실패 경고
- 실제 타임스탬프 정규화
- 비정상 시간 대체
- 프레임 이진 탐색
- 단일 세션 폴더 로딩

### 6.2 실제 500프레임 세션

검증 세션:

```text
boat_sim/Saved/BoatCaptures/20260809_205310_636
```

결과:

```text
Manifest version: 1.1
Valid frame pairs: 500
Warnings: 0
Playback duration: 50207.1569ms
Depth encoding: RGB8; near=255; far=0
```

실제 Color·Depth 파일 1000개가 Manifest 프레임 500개와 모두 연결되었다.

### 6.3 Chrome 초기 화면 검증

Chrome 헤드리스 모드에서 `file://`로 `index.html`을 직접 열어 1440×1000 화면을 확인했다.

첫 검증에서 HTML `hidden` 속성이 `.viewer { display: grid; }` 스타일보다 우선하지 못해 폴더 선택 전 빈 뷰어가 노출되는 문제를 발견했다.

다음 공통 규칙을 추가해 수정했다.

```css
[hidden] {
    display: none !important;
}
```

수정 후에는 제목, 폴더 선택 안내, 초기 상태만 표시되는 것을 다시 확인했다.

이 수정의 커밋:

```text
b13e049 fix: 세션 선택 전 빈 뷰어 숨김
```

## 7. 남은 수동 확인

운영체제의 폴더 선택 창은 자동 헤드리스 검증으로 조작하지 않았다. 언리얼 에디터에서 새 세션을 만든 후 Chrome 또는 Edge에서 다음을 최종 확인한다.

- 타임스탬프 폴더 직접 선택
- Color·Depth 동기 재생
- Depth 장면 형태 식별
- 재생 버튼, 타임라인, 키보드 조작
- 창 너비 900px 전후의 반응형 전환
- 손상된 PNG의 오류 표시

웹 뷰어의 순수 로더는 실제 500프레임 세션으로 검증했으므로, 수동 확인은 브라우저 폴더 선택 권한과 화면 상호작용을 중심으로 진행한다.
