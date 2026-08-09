# Part 2C 단일 바이너리 이미지 아카이브

## 1. 구현 목적

한 번의 레벨 플레이에서 생성된 모든 Color·Depth PNG를 `capture.boatbin` 하나로 묶고, 웹 뷰어에서 외부 `manifest.json`이나 개별 PNG 없이 재생한다.

기존 Part 2A·2B 검증 경로는 유지한다. 따라서 한 세션의 최종 구조는 다음과 같다.

```text
Saved/BoatCaptures/<세션 시각>/
├── manifest.json
├── capture.boatbin
├── color/
│   ├── frame_000000.png
│   └── ...
└── depth/
    ├── frame_000000.png
    └── ...
```

`capture.boatbin` 안의 이미지 Payload도 PNG다. PNG 자체가 이미 압축된 형식이므로 아카이브 전체를 다시 압축하지 않는다. 전체 압축을 추가하면 임의의 한 장을 읽기 위해 앞부분부터 압축을 풀어야 하므로 Part 2C의 랜덤 접근 성능 측정에 불리하다.

## 2. 언리얼 저장 순서

`BoatCaptureComponent`는 실시간 캡처 중에 바이너리를 만들지 않는다. 기존 100ms 캡처와 비동기 GPU Readback의 성능에 영향을 주지 않도록 게임 종료 시 다음 순서로 처리한다.

1. 아직 GPU에 남아 있는 Readback을 CPU로 회수한다.
2. `ImageWriteQueue` Fence를 기다려 Color·Depth PNG 저장을 끝낸다.
3. Manifest 1.1을 저장한다.
4. Manifest 대상 PNG가 모두 존재하고 크기가 0보다 큰지 검사한다.
5. `capture.boatbin.tmp`에 Prefix, Metadata, Index와 PNG Payload를 기록한다.
6. 모든 쓰기가 성공한 경우에만 `capture.boatbin`으로 이름을 바꾼다.

PNG는 1MB 단위로 읽어서 아카이브에 복사한다. 따라서 세션 전체 이미지를 한꺼번에 메모리에 올리지 않는다.

파일이 하나라도 없거나 기록 중 오류가 발생하면 완성되지 않은 임시 바이너리를 제거한다. 이 경우에도 필수 과제 결과인 기존 PNG와 Manifest는 그대로 남는다.

## 3. Binary Format 1

파일의 모든 숫자는 Little Endian이다. Offset은 파일 시작 위치를 0으로 본 절대 바이트 위치이며, 큰 세션도 처리할 수 있도록 `uint64`를 사용한다.

### 3.1 Prefix

고정 32바이트다.

| Offset | Size | Type | 값 |
|---:|---:|---|---|
| 0 | 8 | bytes | `BOATCAP\0` |
| 8 | 2 | `uint16` | Format Version `1` |
| 10 | 2 | `uint16` | Prefix Size `32` |
| 12 | 4 | `uint32` | UTF-8 Metadata 길이 |
| 16 | 4 | `uint32` | Frame Count |
| 20 | 4 | `uint32` | Index Entry Size `48` |
| 24 | 8 | `uint64` | 첫 PNG Payload Offset |

### 3.2 Metadata

Prefix 바로 뒤에 UTF-8 JSON으로 저장한다. Metadata 뒤는 8바이트 경계까지 `0`으로 채운다.

주요 필드는 다음과 같다.

- `session_name`
- `width`, `height`
- `capture_interval_ms`
- `depth_max_cm`
- `color_encoding`, `depth_encoding`
- `capture_source`, `color_tone_mapping`
- `gpu_readback`, `readback_buffer_count`
- `dropped_capture_count`, `failed_capture_count`

이 정보를 바이너리 안에 포함하므로 웹 뷰어는 외부 Manifest 없이 파일 하나만으로 재생 설정을 복원할 수 있다.

### 3.3 Frame Index

Metadata 정렬 영역 뒤에 Frame Index 순서로 저장한다. 한 Entry는 48바이트다.

| Entry Offset | Size | Type | 의미 |
|---:|---:|---|---|
| 0 | 4 | `uint32` | Frame Index |
| 4 | 4 | `uint32` | Reserved, 현재 `0` |
| 8 | 8 | `float64` | 실제 캡처 Timestamp, ms |
| 16 | 8 | `uint64` | Color PNG Offset |
| 24 | 8 | `uint64` | Color PNG Length |
| 32 | 8 | `uint64` | Depth PNG Offset |
| 40 | 8 | `uint64` | Depth PNG Length |

각 Entry의 Color 다음에 같은 프레임의 Depth가 이어지고, 다음 프레임 Payload가 연속해서 배치된다.

## 4. 웹 디코딩

웹 뷰어에는 기존 `세션 폴더 선택`과 별도로 `capture.boatbin 선택` 입력이 있다.

바이너리를 선택하면 다음 부분만 먼저 읽는다.

```text
Prefix → Metadata → Frame Index
```

Magic, 버전, 각 영역 크기, 프레임 순서, Offset·Length와 전체 파일 범위를 모두 확인한 뒤 재생 세션을 만든다. 검증되지 않은 Offset으로 파일을 읽지 않는다.

실제 프레임을 표시할 때는 `File.slice()`로 해당 Color·Depth 구간만 Blob으로 참조한다. Blob Slice는 원본 파일 전체를 복사하지 않는다. 두 PNG의 `Image.decode()`가 모두 끝난 뒤 화면을 함께 교체해 Color·Depth 시점을 맞춘다.

Depth PNG는 언리얼에서 이미 `RGB8`, `Near=255`, `Far=0`으로 변환됐다. 웹에서는 CSS 필터나 재정규화를 적용하지 않고 저장된 0–255 명암을 그대로 표시한다.

## 5. 순차·랜덤 접근 성능 측정

성능 측정은 바이너리 세션에서만 사용할 수 있다. Color와 Depth는 각각 이미지 한 장으로 계산한다.

한 이미지의 측정 범위는 다음과 같다.

```text
File.slice().arrayBuffer() 읽기
  + PNG Blob 생성과 createImageBitmap() 디코딩
```

DOM 이미지 교체와 화면 Paint 시간은 포함하지 않는다.

- 워밍업: 최대 10장, 평균에서 제외
- 순차 접근: Frame Index 순으로 Color → Depth
- 랜덤 접근: 고정 Seed `0xB0A7`의 Fisher–Yates 순서
- 측정 순서: 순차 → 랜덤 → 랜덤 → 순차
- 각 방식의 표본 수: 전체 이미지 수 × 2회
- 표시 기준: 이미지 한 장당 평균 Read, Decode, Total 시간(ms)

순서 교차는 한 방식만 항상 두 번째로 실행돼 OS 파일 캐시 이점을 받는 편향을 줄이기 위한 것이다. OS 캐시를 완전히 비우지는 않으므로 보고서에도 같은 측정 조건을 적는다.

`결과 JSON 저장` 버튼은 다음 내용을 저장한다.

- 측정 시각과 브라우저 User Agent
- 아카이브 이름·크기·프레임 수·이미지 수
- 워밍업 수, 방식별 반복 횟수와 랜덤 Seed
- 순차·랜덤의 표본 수와 평균 Read·Decode·Total ms/image

최종 보고서에는 JSON의 `sequential.averageTotalMs`, `random.averageTotalMs`를 사용한다. 측정하지 않은 값을 임의로 작성하지 않는다.

## 6. 검증 현황과 실행 확인

- UE 5.5 `boat_simEditor Win64 Development` C++ 컴파일 통과
- Manifest 1.1 기존 테스트 포함 Node.js 단위 테스트 15개 통과
- Binary Format 1 정상 로드, Magic·버전·잘린 파일·잘못된 Offset 거부 테스트 포함
- 고정 Seed 랜덤 순서의 중복·누락 방지 테스트 포함

실제 제출용 데이터는 에디터를 완전히 재시작하거나 새 C++ 모듈을 로드한 뒤 PIE를 한 번 끝내 생성한다. 이후 Chrome 또는 Edge에서 `capture.boatbin`을 열어 재생과 성능 측정을 실행하고 JSON 결과를 보고서에 옮긴다.
