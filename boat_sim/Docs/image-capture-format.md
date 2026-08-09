# 이미지 캡처 및 깊이 저장 포맷 정리

- 정리일: 2026-08-09
- 대상 프로젝트: Unreal Engine 5.5.4 `boat_sim`
- 관련 과제: 수상 자율주행 시뮬레이션 Part 2
- 구현 파일: `Source/boat_sim/BoatCaptureComponent.h`, `Source/boat_sim/BoatCaptureComponent.cpp`

## 1. 결론

현재 깊이 이미지 저장 포맷은 미정 상태가 아니다. 다음 규격으로 확정되어 구현돼 있다.

```text
깊이 캡처 Render Target: R32_FLOAT
캡처 깊이 단위: Unreal Unit인 cm
최종 저장 파일: 8-bit RGB PNG
정규화 거리: 0~5000cm
명암 방향: 가까운 물체 255, 먼 물체 0
```

`R32_FLOAT`는 장면 깊이를 읽는 중간 Render Target의 포맷이다. 디스크에 저장되는 PNG가 32-bit float라는 뜻은 아니다. 현재 코드는 float 깊이를 읽은 뒤 0~255로 정규화하여 RGB 세 채널에 같은 값을 넣는다. 따라서 최종 깊이 파일은 실질적으로 8-bit grayscale 정보를 가진 RGB PNG다.

현재 포맷은 웹에서 바로 표시하기 쉽고 과제의 필수 시각화 요구사항을 충족한다. 반면 거리 측정, 3D 복원, 학습 데이터처럼 높은 깊이 정밀도가 필요하다면 16-bit PNG, EXR 또는 별도 binary 원본을 추가하는 편이 적합하다.

## 2. 과제 요구사항과 구현 방향

이미지 저장과 관련된 필수 요구사항은 다음과 같다.

- 선박에 Scene Capture 카메라 장착
- 주행 중 컬러 이미지와 depth map 저장
- 실제 시간 기준 100ms 간격으로 저장
- 한 번의 레벨 실행 동안 프레임을 순서대로 저장
- 웹 뷰어에서 이미지 시퀀스를 영상처럼 재생
- depth map을 0~255 RGB 범위로 시각화
- 깊이 비트 심도와 정규화 방식은 자유

현재 구현은 웹 뷰어에서 별도 깊이 변환 없이 PNG를 바로 표시할 수 있도록, UE에서 파일을 저장하기 전에 깊이를 0~255 RGB로 변환한다.

## 3. 현재 캡처 파이프라인

```text
SceneCaptureComponent2D
├─ 컬러 캡처
│  ├─ RTF_RGBA8_SRGB
│  ├─ SCS_FinalColorLDR
│  └─ 8-bit PNG
└─ 깊이 캡처
   ├─ RTF_R32f
   ├─ SCS_SceneDepth
   ├─ R 채널의 cm 깊이 읽기
   ├─ 0~5000cm를 255~0으로 정규화
   └─ 8-bit RGB PNG
```

하나의 `UBoatCaptureComponent`가 같은 위치와 회전에서 컬러와 깊이를 차례대로 캡처한다. 두 파일은 같은 프레임 번호를 사용하므로 한 쌍으로 대응한다.

기본 설정은 다음과 같다.

| 항목 | 현재 값 |
|---|---:|
| 해상도 | 1280×720 |
| 캡처 간격 | 0.1초 |
| 컬러 Render Target | `RTF_RGBA8_SRGB` |
| 컬러 Capture Source | `SCS_FinalColorLDR` |
| 깊이 Render Target | `RTF_R32f` |
| 깊이 Capture Source | `SCS_SceneDepth` |
| 최대 깊이 | 5000cm |
| 최종 이미지 | PNG |

## 4. 현재 깊이 정규화 규칙

깊이 Render Target의 R 채널에서 센티미터 단위 값을 읽고 다음 공식으로 변환한다.

```cpp
NormalizedDepth = Clamp(DepthCm / DepthMaxDistance, 0.0f, 1.0f);
Gray = Round((1.0f - NormalizedDepth) * 255.0f);
RGB = (Gray, Gray, Gray);
```

기본 `DepthMaxDistance`는 `5000cm`, 즉 50m다.

| 실제 거리 | 정규화 값 | 저장 RGB |
|---:|---:|---:|
| 0m | 0.0 | `(255, 255, 255)` |
| 10m | 0.2 | 약 `(204, 204, 204)` |
| 20m | 0.4 | 약 `(153, 153, 153)` |
| 25m | 0.5 | 약 `(128, 128, 128)` |
| 40m | 0.8 | 약 `(51, 51, 51)` |
| 50m 이상 | 1.0 | `(0, 0, 0)` |

다음 값도 검은색으로 처리한다.

- 50m보다 먼 깊이
- 하늘 또는 무한대에 해당하는 깊이
- 0 이하의 깊이
- `NaN`, `Inf`와 같은 비정상 값

RGB 세 채널에는 동일한 값이 들어가며 알파는 255로 저장된다.

## 5. 비트 심도와 실제 정밀도

현재 PNG는 R, G, B가 각각 8-bit지만 세 채널에 같은 값이 들어간다. 따라서 깊이 단계는 24-bit가 아니라 256단계다.

50m 범위를 256단계로 나누므로 한 단계가 나타내는 거리는 대략 다음과 같다.

```text
5000cm / 255 ≈ 19.61cm
```

즉 서로 약 20cm 미만 차이 나는 두 깊이는 같은 회색 값으로 저장될 수 있다. 사람에게 장면 형태를 보여주는 용도로는 충분하지만 정확한 거리 복원용으로는 부족하다.

중간 Render Target이 `R32_FLOAT`이므로 정규화 전에는 더 정밀한 깊이를 얻는다. 다만 현재 저장 과정에서 `uint8`로 변환하므로 디스크 파일에는 그 정밀도가 남지 않는다.

## 6. 현재 출력 구조

한 번 실행할 때마다 시각 기반 세션 폴더를 생성한다.

```text
Saved/BoatCaptures/20260809_153012_123/
├─ color/
│  ├─ frame_000000.png
│  ├─ frame_000001.png
│  └─ ...
├─ depth/
│  ├─ frame_000000.png
│  ├─ frame_000001.png
│  └─ ...
└─ manifest.json
```

컬러와 깊이 읽기가 모두 성공한 경우에만 같은 프레임 번호로 저장한다. PNG 쓰기는 `ImageWriteQueue`를 통해 비동기로 처리하고, 실행 종료 시 저장 큐가 완료될 때까지 기다린 후 manifest를 작성한다.

## 7. manifest 규격

현재 manifest의 핵심 필드는 다음과 같다.

```json
{
  "version": "1.0",
  "width": 1280,
  "height": 720,
  "capture_interval_ms": 100,
  "depth_max_cm": 5000,
  "depth_encoding": "RGB8; near=255; far=0",
  "frame_count": 2,
  "frames": [
    {
      "index": 0,
      "timestamp_ms": 0.0,
      "color": "color/frame_000000.png",
      "depth": "depth/frame_000000.png"
    },
    {
      "index": 1,
      "timestamp_ms": 100.0,
      "color": "color/frame_000001.png",
      "depth": "depth/frame_000001.png"
    }
  ]
}
```

웹 뷰어는 manifest를 기준으로 컬러와 깊이 파일을 같은 프레임으로 묶고 `timestamp_ms` 순서로 재생할 수 있다.

## 8. 웹 뷰어에서의 처리

현재 depth PNG는 이미 0~255 RGB로 변환돼 있으므로 일반 `<img>` 또는 Canvas에서 바로 표시할 수 있다. 웹에서 다시 정규화할 필요는 없다.

```text
가까운 물체 → 밝게 표시
먼 물체     → 어둡게 표시
50m 이상    → 검은색
```

웹에서 픽셀 값을 거리로 근사 복원할 경우 다음 공식을 사용할 수 있다.

```javascript
const gray = pixel.r;
const normalizedDepth = 1 - gray / 255;
const depthCm = normalizedDepth * manifest.depth_max_cm;
```

이 값은 8-bit 양자화가 적용된 근사 거리이며 원래 float 깊이와 완전히 같지는 않다.

## 9. 검토했던 대안 포맷

### 9.1 8-bit RGB PNG - 현재 적용

```text
저장값: RGB 각 채널 0~255
깊이 정보: 세 채널에 같은 회색 값
정규화: 0~50m
```

장점:

- 브라우저에서 바로 표시 가능
- 별도 PNG 디코더나 셰이더 불필요
- 파일 크기가 비교적 작음
- 과제의 0~255 RGB 시각화 요구사항을 직접 충족

단점:

- 깊이가 256단계로 줄어듦
- 정확한 거리 분석과 3D 복원에는 부적합

### 9.2 16-bit grayscale PNG - 고정밀 이미지 대안

정규화 공식은 다음과 같이 확장할 수 있다.

```cpp
NormalizedDepth = Clamp(DepthCm / 5000.0f, 0.0f, 1.0f);
Depth16 = Round(NormalizedDepth * 65535.0f);
```

50m 범위에서 이론적인 한 단계는 다음과 같다.

```text
5000cm / 65535 ≈ 0.0763cm ≈ 0.763mm
```

장점:

- 현재 포맷보다 훨씬 높은 거리 정밀도
- PNG의 무손실 압축 사용 가능
- 하나의 이미지 파일로 관리 가능

단점:

- 일반 브라우저 Canvas가 표시 과정에서 8-bit로 축소할 수 있음
- 원래 16-bit 값을 사용하려면 JavaScript PNG 디코더가 필요할 수 있음
- UE의 저장 경로를 16-bit grayscale 데이터에 맞게 변경해야 함

### 9.3 32-bit float EXR - 원본 깊이 보존 대안

```text
저장값: float32
단위: cm
정규화: 하지 않음
```

장점:

- SceneDepth의 실거리 값을 거의 그대로 보존
- 후처리, 연구, 데이터셋 생성에 적합

단점:

- 파일 크기가 큼
- 웹 브라우저에서 직접 재생하기 어려움
- 별도 EXR 디코딩 과정 필요

### 9.4 UInt16 또는 Float32 binary - 선택 과제용 대안

선택 과제의 단일 binary 압축을 구현한다면 깊이를 PNG로 바꾸기 전에 원본 데이터 형태로 묶을 수 있다.

예시 1: 밀리미터 단위 UInt16

```cpp
DepthMillimeters = Clamp(Round(DepthCm * 10.0f), 0, 65535);
```

- 표현 가능 범위: 0~65.535m
- 정밀도: 1mm
- 한 픽셀당 2바이트

예시 2: 센티미터 단위 Float32

- 정규화 없이 실거리 보존
- 한 픽셀당 4바이트
- 압축 전 용량이 큼

binary를 사용할 경우 헤더에 다음 정보를 포함해야 한다.

- 매직 넘버와 버전
- 이미지 너비와 높이
- 프레임 수
- 픽셀 포맷
- 깊이 단위
- 프레임별 offset과 byte length
- 프레임별 timestamp

## 10. 포맷 비교

| 포맷 | 깊이 정밀도 | 웹 표시 | 파일 크기 | 적합한 용도 |
|---|---|---|---|---|
| RGB8 PNG | 낮음 | 매우 쉬움 | 작음 | 필수 과제, 시각화 |
| Gray16 PNG | 높음 | 추가 처리 필요 | 중간 | 정밀 깊이 이미지 |
| Float32 EXR | 매우 높음 | 어려움 | 큼 | 분석, 데이터셋 |
| UInt16 binary | 높음 | 전용 디코더 필요 | 중간 | 압축 선택 과제 |
| Float32 binary | 매우 높음 | 전용 디코더 필요 | 큼 | 원본 깊이 보존 |

## 11. 권장 운영 방식

### 필수 과제만 구현할 경우

현재의 RGB8 PNG 방식을 유지한다.

- 과제에서 요구하는 사람이 식별 가능한 depth map 제공
- 웹 뷰어에서 즉시 재생 가능
- 저장과 디코딩 구조가 단순함
- manifest에 정규화 범위가 명시돼 재현 가능함

### 정밀 깊이까지 보존할 경우

현재 preview PNG를 제거하지 않고 원본 깊이를 추가로 저장하는 방식을 권장한다.

```text
depth_preview/frame_000000.png  # RGB8, 웹 표시용
depth_raw/frame_000000.png      # Gray16 또는 EXR, 분석용
```

또는 선택 과제의 binary 파일 안에 원본 깊이를 넣고 현재 PNG는 웹 미리보기용으로 유지한다.

이 방식은 웹 호환성과 원본 정밀도를 동시에 확보하고, 기존 웹 뷰어와의 호환성도 유지한다.

## 12. 구현 변경 시 함께 수정할 항목

깊이 포맷이나 최대 거리를 변경할 때는 다음 항목을 함께 갱신해야 한다.

- `DepthRenderTarget` 포맷
- `ReadDepthPixels()` 변환 과정
- 이미지 저장용 PixelData 타입
- `manifest.json`의 `depth_max_cm`
- `manifest.json`의 `depth_encoding`
- 웹 뷰어의 깊이 디코딩 방식
- 보고서의 비트 심도와 정규화 설명
- 기존 캡처 데이터와의 버전 호환성

포맷이 달라지면 manifest의 `version`도 올리는 것이 안전하다.

## 13. 현재 결정 기록

현재 프로젝트의 결정은 다음과 같다.

1. 필수 제출용 깊이 이미지는 RGB8 PNG로 저장한다.
2. 깊이 범위는 기본 0~50m로 제한한다.
3. 가까운 물체를 흰색, 먼 물체를 검은색으로 표시한다.
4. 웹 뷰어는 현재 PNG를 그대로 재생한다.
5. 16-bit PNG와 EXR은 현재 구현이 아니라 고정밀 저장이 필요할 때 사용할 대안이다.
6. 선택 과제의 binary 압축을 구현한다면 UInt16 또는 Float32 원본 깊이 저장을 다시 검토한다.

따라서 현재 저장이 임시 또는 미정 포맷으로 동작하는 것은 아니다. 구현과 manifest 모두 `RGB8; near=255; far=0` 규격을 명시하고 있다.

## 14. 검증 체크리스트

- [ ] 컬러와 깊이 파일 수가 동일함
- [ ] 같은 프레임 번호의 컬러·깊이 파일이 모두 존재함
- [ ] 이미지 해상도가 manifest와 동일함
- [ ] 캡처 간격이 실제 시간 기준 약 100ms임
- [ ] 가까운 물체가 밝고 먼 물체가 어둡게 표시됨
- [ ] 50m 이상의 배경이 검은색으로 표시됨
- [ ] PIE 종료 후 manifest가 생성됨
- [ ] `frame_count`와 실제 파일 수가 일치함
- [ ] 웹 뷰어에서 시간순 재생이 가능함
- [ ] 포맷 변경 시 manifest 버전과 디코더가 함께 갱신됨

## 15. 컬러·깊이 통합 캡처 검토 기록

캡처 과정에서 컬러와 깊이를 각각 렌더링하면 같은 프레임마다 `CaptureScene()`이 두 번 호출된다. 이를 줄이기 위한 방법으로 `SCS_SceneColorSceneDepth`를 사용하는 통합 캡처를 검토했다.

이 Capture Source의 출력은 다음과 같다.

```text
RGB = Scene Color HDR
A   = Scene Depth
```

하나의 RGBA16f Render Target을 한 번 캡처한 뒤 RGB와 A를 분리하면 컬러와 깊이를 함께 얻을 수 있다. 따라서 두 번의 장면 렌더링을 한 번으로 줄일 가능성이 있다는 장점이 있다.

하지만 이것은 처리 과정만 합치는 변경이 아니다. 현재 컬러 캡처에 사용하는 `SCS_FinalColorLDR`와 통합 캡처의 `Scene Color HDR`은 서로 다른 단계의 영상이므로, 최종 컬러 PNG가 자동으로 같아지지는 않는다.

### 15.1 현재 방식과 통합 방식의 차이

```text
현재 방식
FinalColorLDR -> 컬러 PNG
SceneDepth   -> 깊이 정규화 -> 깊이 PNG

통합 방식
SceneColor HDR -> 별도 톤 매핑 및 sRGB 변환 -> 컬러 PNG
SceneDepth     -> 깊이 정규화              -> 깊이 PNG
```

`FinalColorLDR`에는 최종 화면을 만들기 위한 노출, 톤 매핑, 감마·sRGB 변환, 색 보정 및 일부 후처리가 반영된다. 반면 `Scene Color HDR`은 이 처리 이전의 넓은 밝기 범위를 가진다.

따라서 두 컬러의 차이는 단순한 모니터 색감 차이 정도로 한정되지 않는다. HDR 값을 별도 처리 없이 8-bit PNG로 축소하면 다음과 같은 차이가 발생할 수 있다.

- 밝은 하늘이나 물 반사가 흰색으로 날아감
- 어두운 장벽이나 선박 영역이 검게 뭉개짐
- 밝기와 대비가 크게 달라짐
- 물, 하늘, 장벽 사이의 색 분포와 경계가 달라짐

HDR RGB에 고정된 노출과 적절한 톤 매핑을 적용하면 현재 화면과 육안상 비슷하게 만들 수 있다. 다만 Unreal의 `FinalColorLDR`와 픽셀 단위로 완전히 동일한 결과를 보장하는 것은 어렵다.

깊이 채널은 기존과 같은 Scene Depth를 사용하고 동일한 단위 및 정규화 공식을 적용한다면 현재 결과와 거의 같은 규격으로 유지할 수 있다.

### 15.2 자율주행 학습·분석에서 컬러의 중요성

컬러의 중요도는 자율주행 모델이 어떤 데이터를 입력으로 사용하는지에 따라 달라진다.

| 입력 구성 | 컬러 변화의 영향 |
|---|---|
| 깊이만 사용 | 경로 및 장애물 거리 판단에는 거의 영향 없음 |
| RGB만 사용 | 장벽 인식, 분할 및 환경 구분 성능에 직접적인 영향이 있음 |
| RGB와 Depth를 함께 사용 | Depth가 기하 정보를 보완하지만 RGB 분포 변화도 학습 결과에 영향을 줄 수 있음 |

학습과 평가가 모두 동일한 통합 캡처 및 톤 매핑 과정을 사용한다면 입력 데이터의 일관성을 유지할 수 있다. 반대로 HDR 기반 이미지로 학습하고 실제 실행이나 평가에서는 `FinalColorLDR` 이미지를 사용하면 입력 분포가 달라지는 도메인 차이가 생겨 성능이 낮아질 수 있다.

따라서 자율주행 학습 데이터에서는 화면과 완전히 동일한 색을 만드는 것뿐 아니라 다음 조건이 중요하다.

- 모든 프레임에 같은 노출과 톤 매핑을 적용함
- 학습, 검증 및 실제 실행에서 동일한 컬러 처리 과정을 사용함
- 자동 노출로 프레임별 밝기 분포가 불필요하게 흔들리지 않도록 함
- 컬러 처리 방식이 바뀌면 데이터셋 및 manifest 버전에도 이를 기록함

### 15.3 현재 판단

통합 캡처는 `CaptureScene()` 호출 횟수를 줄일 수 있으므로 성능 개선 후보로 가치가 있다. 다만 컬러 결과의 의미가 달라지므로 단순한 내부 최적화로 취급해서는 안 된다.

현재와 같은 컬러 화면을 보존하는 것이 우선이면 컬러와 깊이를 따로 캡처하는 방식을 유지하는 것이 안전하다. 통합 캡처를 적용한다면 HDR 컬러의 톤 매핑 규칙을 먼저 확정하고, 기존 `FinalColorLDR` 이미지와 비교한 뒤 학습·평가 파이프라인 전체에 같은 규칙을 적용해야 한다.

또한 캡처 성능 저하는 `CaptureScene()` 횟수뿐 아니라 Render Target의 동기식 GPU 읽기에서도 발생할 수 있다. 통합 캡처만으로 전체 성능 문제가 해결된다고 단정하지 않고, 캡처 호출 횟수와 GPU Readback 비용을 각각 측정해야 한다.

이 절은 통합 캡처를 검토한 사고 과정과 적용 시 주의점을 기록한 것이며, 현재 캡처 방식을 통합 방식으로 변경했다는 구현 완료 기록은 아니다.

## 16. 캡처 카메라의 불필요한 렌더링 제거 검토

컬러와 깊이 이미지를 저장하는 과정에서 캡처 결과에 필요하지 않은 렌더링 효과까지 다시 계산하면 캡처 비용이 증가한다. 이를 줄이기 위해 캡처 카메라에서 다음 효과를 제외하는 방안을 검토했다.

- Motion Blur
- Depth of Field
- Bloom
- Lens Flare
- Screen Space Reflection
- Volumetric Fog
- Ray Tracing
- 캡처 거리 밖의 오브젝트

이 설정은 Unreal Engine 소스를 수정하지 않고 `USceneCaptureComponent2D`를 상속하는 현재 `UBoatCaptureComponent`의 C++ 코드에서 캡처 카메라에만 적용할 수 있다. 프로젝트 전체의 렌더링 설정을 끄는 방식과 달리 플레이어 카메라 및 다른 카메라의 화면에는 영향을 주지 않는다.

### 16.1 후처리 및 렌더링 기능 비활성화

현재 캡처 컴포넌트의 생성자에서 다음과 같이 Show Flag와 Ray Tracing 사용 여부를 설정할 수 있다.

```cpp
UBoatCaptureComponent::UBoatCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	bCaptureEveryFrame = false;
	bCaptureOnMovement = false;
	FOVAngle = 90.0f;

	// 학습 데이터에 필요하지 않은 렌더링 효과 제거
	ShowFlags.SetMotionBlur(false);
	ShowFlags.SetDepthOfField(false);
	ShowFlags.SetBloom(false);
	ShowFlags.SetLensFlares(false);
	ShowFlags.SetScreenSpaceReflections(false);
	ShowFlags.SetVolumetricFog(false);

	// 프로젝트에서 Ray Tracing을 사용하더라도 이 캡처에서는 사용하지 않음
	bUseRayTracingIfEnabled = false;
}
```

Motion Blur와 Depth of Field는 이동 중인 장벽이나 화면 일부를 흐리게 할 수 있으므로, 이를 제거하면 렌더링 비용뿐 아니라 학습 데이터의 프레임별 경계가 불필요하게 흐려지는 현상도 줄일 수 있다.

Bloom, Lens Flare, Screen Space Reflection과 Volumetric Fog를 제거하면 컬러 이미지는 현재의 최종 화면과 달라질 수 있다. 따라서 각 옵션을 적용할 때에는 컬러 및 깊이 이미지가 과제 요구사항과 학습 목적에 맞는지 전후 결과를 비교해야 한다.

### 16.2 최대 렌더링 거리 제한

`USceneCaptureComponent`에는 캡처 카메라의 최대 렌더링 거리를 제한하는 `MaxViewDistanceOverride`가 있다. 현재 깊이 이미지의 최대 표현 거리가 `5000cm`이므로 같은 값을 적용하는 방안을 검토할 수 있다.

```cpp
void UBoatCaptureComponent::BeginPlay()
{
	Super::BeginPlay();

	// 블루프린트에서 설정한 DepthMaxDistance 값을 런타임에 반영
	MaxViewDistanceOverride = DepthMaxDistance;

	// 기존 캡처 초기화 코드가 이어짐
}
```

`DepthMaxDistance`는 `EditAnywhere`이므로 블루프린트에서 기본값을 덮어쓸 수 있다. 따라서 생성자에서 두 값을 연결하기보다 블루프린트 설정이 적용된 이후인 `BeginPlay()`에서 `MaxViewDistanceOverride`에 반영하는 편이 정확하다.

두 설정은 같은 수치를 사용하더라도 역할이 다르다.

```text
DepthMaxDistance = 5000
-> 읽어 온 깊이를 0~5000cm 범위로 정규화하고 그보다 먼 값을 검은색으로 처리

MaxViewDistanceOverride = 5000
-> 캡처 카메라에서 5000cm보다 먼 프리미티브를 렌더링 대상에서 제외
```

현재 구현에는 첫 번째 깊이 정규화만 적용되어 있다. `MaxViewDistanceOverride`를 추가하면 먼 오브젝트의 렌더링 비용을 줄일 수 있지만 다음 사항을 확인해야 한다.

- 50m 밖의 장벽과 메시가 컬러 이미지에서도 사라질 수 있음
- 하늘, Sky Atmosphere 및 매우 큰 Water Mesh는 일반적인 거리 컬링과 다르게 동작할 수 있음
- 먼 배경도 컬러 학습 데이터에 필요하다면 깊이 최대 거리와 렌더링 거리를 반드시 같게 설정할 필요는 없음
- 깊이 이미지에서 50m 밖을 모두 검은색으로 취급한다면 거리 제한의 적용 근거가 비교적 분명함

즉, `DepthMaxDistance`가 5000이라는 이유만으로 `MaxViewDistanceOverride`도 즉시 5000으로 확정하기보다는 컬러 이미지의 배경 변화와 실제 성능 개선량을 함께 확인해야 한다.

### 16.3 특정 액터의 렌더링 제외

거리 제한 외에도 `USceneCaptureComponent`가 제공하는 다음 기능을 C++ 또는 컴포넌트 설정에서 사용할 수 있다.

- `HiddenActors`: 지정한 액터를 캡처에서 제외
- `HiddenComponents`: 지정한 컴포넌트를 캡처에서 제외
- `ShowOnlyActors`: 지정한 액터만 캡처
- `PrimitiveRenderMode`: 전체 렌더링 또는 Show Only 목록 방식 선택

`ShowOnlyActors` 방식은 캡처 대상을 강하게 제한해 비용을 줄일 수 있지만, 물이나 하늘처럼 필요한 요소를 목록에서 빠뜨리면 데이터가 불완전해질 수 있다. 우선은 명확히 불필요한 액터를 `HiddenActors`로 제외하거나 최대 렌더링 거리를 제한하는 방식부터 검증하는 것이 안전하다.

### 16.4 적용 및 검증 순서

현재 캡처 결과를 기준선으로 보존한 뒤 다음 순서로 옵션을 하나씩 적용한다.

1. Motion Blur, Depth of Field 등 불필요한 Show Flag를 비활성화한다.
2. `bUseRayTracingIfEnabled`를 `false`로 설정한다.
3. 변경 전후 컬러 및 깊이 이미지가 요구사항에 맞는지 비교한다.
4. `MaxViewDistanceOverride = DepthMaxDistance`를 시험 적용한다.
5. 하늘, 물, 장벽 및 먼 배경이 비정상적으로 사라지지 않는지 확인한다.
6. 적용 전후 평균 FPS, 프레임 수 및 실제 캡처 간격을 비교한다.
7. 필요하다면 `HiddenActors` 또는 Show Only 방식을 추가로 검토한다.

후처리 제거는 컬러 결과를 바꿀 수 있으므로 여러 최적화를 한꺼번에 적용하지 않고 항목별로 A/B 테스트해야 원인과 효과를 구분할 수 있다.

또한 캡처 성능 저하는 장면 렌더링뿐 아니라 Render Target에서 CPU로 픽셀을 읽어 오는 동기식 GPU Readback에서도 발생할 수 있다. 렌더링 옵션을 제거한 뒤에도 끊김이 남는다면 `ReadColorPixels()`와 `ReadDepthPixels()`의 읽기 비용을 별도로 측정해야 한다.

이 절은 C++로 적용 가능한 최적화 방법과 검증 순서를 기록한 것이다. 해당 Show Flag와 `MaxViewDistanceOverride`가 현재 코드에 구현 완료되었다는 의미는 아니다.

참고: [Unreal Engine 5.5 USceneCaptureComponent API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/Components/USceneCaptureComponent?application_version=5.5)

## 17. 캡처 성능 개선 적용 및 측정 기록

컬러와 깊이 이미지를 100ms 간격으로 저장하는 기능을 활성화한 뒤 PIE(Play In Editor) 성능이 크게 저하되는 현상을 확인했다. 원인을 분리하기 위해 다음 세 가지 개선 방향을 검토했다.

1. 컬러와 깊이를 한 번의 `CaptureScene()`으로 통합 캡처
2. GPU 결과를 비동기로 읽는 GPU Texture Readback 적용
3. 캡처 카메라에서 불필요한 후처리 및 렌더링 효과 제거

이 중 렌더링 횟수와 장면 자체의 비용을 먼저 줄이기 위해 1번과 3번을 우선 적용했다. 2번 비동기 GPU Readback은 구현 범위가 더 크므로 전후 성능을 다시 측정한 뒤 다음 단계로 남겼다.

### 17.1 변경 전 성능

기존 방식은 컬러와 깊이를 별도로 렌더링하고 Render Target의 픽셀을 CPU에서 동기적으로 읽었다. 캡처를 활성화한 PIE 화면에서 측정한 주요 수치는 다음과 같다.

| 항목 | 변경 전 측정값 |
|---|---:|
| FPS | 8.06 |
| 프레임 시간 | 124.02ms |
| Game | 19.03ms |
| Draw | 106.02ms |
| RHIT | 4.92ms |
| GPU Time | 90.46ms |
| Input | 140.77ms |
| World Tick Time Inclusive Average | 95.85ms |

Draw와 GPU 시간이 모두 매우 높았으며, 캡처가 활성화된 동안 화면 갱신과 선박 이동이 심하게 끊겼다.

### 17.2 SceneColor HDR와 SceneDepth 통합 캡처 적용

컬러와 깊이에 각각 사용하던 Render Target을 하나의 `RTF_RGBA16f` Render Target으로 통합하고 Capture Source를 `SCS_SceneColorSceneDepth`로 설정했다.

```text
RGB = Scene Color HDR
A   = Scene Depth(cm)
```

한 번의 `CaptureScene()`과 한 번의 픽셀 읽기로 같은 카메라 위치의 컬러와 깊이를 얻은 뒤, CPU에서 RGB와 A 채널을 각각 컬러 PNG와 깊이 PNG로 분리한다. 이 변경으로 프레임마다 두 번 수행하던 장면 렌더링을 한 번으로 줄였다.

깊이는 기존과 동일하게 `DepthMaxDistance`를 기준으로 정규화한다. 가까운 지점은 흰색, 최대 거리 이상은 검은색이 되도록 8-bit Grayscale 값으로 변환한다.

### 17.3 HDR 컬러 톤 매핑 적용

`SceneColor HDR`은 기존 `FinalColorLDR`와 달리 Unreal의 최종 톤 매핑 이전 값이다. HDR 값을 그대로 8-bit PNG로 변환하면 밝은 영역이 날아가고 전체 명암이 달라지므로 다음 처리를 추가했다.

```text
Scene Color HDR
  -> 고정 Exposure Compensation 적용
  -> ACES 근사 Filmic Tone Curve 적용
  -> Linear RGB를 sRGB 8-bit로 변환
  -> 컬러 PNG 저장
```

톤 매핑은 Unreal의 기본 Filmic Tonemapper와 비슷한 명암 압축을 만드는 ACES 근사식을 사용한다. `ColorExposureCompensation`의 기본값은 `0.0 EV`이며 블루프린트에서 조정할 수 있다.

이 방식의 목적은 기존 플레이 화면과 픽셀 단위로 동일한 이미지를 만드는 것이 아니라, 하늘·물·장벽의 밝기와 색을 육안상 비슷하고 일관된 범위로 저장하는 것이다. 학습 및 평가 데이터에는 같은 노출과 톤 매핑 설정을 계속 사용해야 한다.

### 17.4 캡처 전용 렌더링 효과 제거

캡처 결과에 필요하지 않거나 장면 경계를 흐릴 수 있는 다음 기능을 `UBoatCaptureComponent`의 Show Flag에서 비활성화했다.

| 에디터 정식 명칭 | 코드 Show Flag | 적용 효과 |
|---|---|---|
| Motion Blur | `MotionBlur` | 이동 방향으로 화면이 흐려지는 효과 제거 |
| Depth of Field | `DepthOfField` | 초점 거리 밖의 화면이 흐려지는 효과 제거 |
| Bloom | `Bloom` | 밝은 영역 주변으로 빛이 번지는 효과 제거 |
| Lens Flares | `LensFlares` | 강한 광원 주변의 렌즈 반사 효과 제거 |
| Screen Space Reflections | `ScreenSpaceReflections` | 화면 정보를 이용한 반사 효과 제거 |
| Volumetric Fog | `VolumetricFog` | 공간에 빛과 안개가 퍼지는 효과 제거 |
| Use Ray Tracing If Enabled | `bUseRayTracingIfEnabled` | 프로젝트 설정과 관계없이 캡처 카메라의 Ray Tracing 사용 중지 |

설정은 캡처 컴포넌트에만 적용되므로 플레이어 카메라나 프로젝트 전체 렌더링 설정에는 영향을 주지 않는다. 블루프린트에 저장된 다른 Show Flag 값은 보존하고 위 항목만 변경한다.

이번 단계에서는 컬러 이미지의 먼 배경이 사라질 가능성을 고려해 `MaxViewDistanceOverride`는 적용하지 않았다. `HiddenActors`와 `ShowOnlyActors`도 아직 사용하지 않았다.

### 17.5 PNG 저장과 GPU Readback의 차이

컬러와 깊이 PNG의 압축 및 파일 저장은 `ImageWriteQueue`를 사용해 비동기 작업으로 등록한다. 따라서 PNG 압축과 디스크 쓰기 자체는 매 캡처 프레임의 게임 스레드에서 완료될 때까지 기다리지 않는다.

하지만 현재 `FImageUtils::GetRenderTargetImage()`는 Render Target의 GPU 결과를 CPU 메모리로 가져오는 동기식 읽기다. `ImageWriteQueue`를 사용하더라도 이 GPU Readback 대기까지 비동기가 되는 것은 아니다.

```text
현재 처리 순서
CaptureScene()
  -> GetRenderTargetImage()가 GPU 완료를 기다림
  -> 컬러와 깊이 픽셀 분리
  -> PNG 저장 작업만 비동기 큐에 등록
```

### 17.6 변경 후 성능과 효과

통합 캡처와 불필요한 렌더링 효과 제거를 적용한 뒤 같은 PIE 환경에서 다시 측정한 결과는 다음과 같다.

| 항목 | 변경 전 | 변경 후 | 변화 |
|---|---:|---:|---:|
| FPS | 8.06 | 9.37 | 약 16% 증가 |
| 프레임 시간 | 124.02ms | 106.68ms | 약 14% 감소 |
| Draw | 106.02ms | 75.17ms | 약 29% 감소 |
| RHIT | 4.92ms | 3.16ms | 약 36% 감소 |
| GPU Time | 90.46ms | 68.84ms | 약 24% 감소 |
| Input | 140.77ms | 124.40ms | 약 12% 감소 |
| Game | 19.03ms | 37.46ms | 해당 캡처 시점에서는 증가 |
| World Tick Time Inclusive Average | 95.85ms | 101.68ms | 큰 개선 없음 |

Draw와 GPU 시간이 감소했으므로 장면 렌더링 횟수를 줄이고 불필요한 효과를 제거한 변경은 실제 효과가 있었다. 다만 FPS는 여전히 약 10에 머물고 World Tick Time도 약 100ms이므로 전체 성능 문제는 해결되지 않았다.

이 수치는 PIE에서 성능 통계를 표시한 두 시점의 측정값이다. 에디터 오버헤드와 장면 상태에 따라 값이 달라질 수 있으므로 최종 판단에는 일정 시간의 평균값과 캡처 비활성화 기준선 측정이 추가로 필요하다.

### 17.7 현재 남은 병목

현재 캡처 간격은 실제 시간 기준 100ms다. 그런데 `CaptureScene()` 직후 `FImageUtils::GetRenderTargetImage()`가 GPU 작업 완료를 동기적으로 기다리면서 한 프레임 처리 시간도 약 100ms까지 증가한다.

처리가 캡처 간격보다 오래 걸리면 다음 Tick 시점에는 이미 다음 캡처 시간이 도달한 상태가 된다. 그 결과 의도는 10Hz 캡처이지만, 실제 실행에서는 느려진 거의 모든 프레임에서 다시 캡처와 동기식 읽기를 수행하는 상태가 될 수 있다.

```text
100ms 캡처 시점 도달
  -> 장면 캡처
  -> 동기식 GPU Readback 대기
  -> 프레임 처리에 약 100ms 소요
  -> 다음 프레임에서 다시 캡처 시점 도달
```

따라서 현재의 가장 큰 개선 후보는 `FRHIGPUTextureReadback`을 이용한 비동기 GPU Readback이다. 목표 처리 구조는 다음과 같다.

```text
캡처 프레임
  -> CaptureScene()
  -> GPU Readback 요청
  -> 기다리지 않고 Tick 종료

이후 프레임
  -> Readback 준비 여부 확인
  -> 준비된 결과만 CPU 메모리로 복사
  -> 톤 매핑 및 깊이 변환
  -> PNG 저장 큐 등록
```

2~3개의 Readback 버퍼를 순환시키면 이전 캡처 결과를 기다리는 동안 다음 캡처를 요청할 수 있다. 다만 처리되지 않은 요청이 계속 쌓이지 않도록 최대 대기 개수를 제한하고, 게임 종료 시 남은 Readback과 이미지 저장 작업을 안전하게 마무리해야 한다.

### 17.8 다음 검증 순서

1. 캡처를 비활성화한 상태의 PIE FPS와 Frame, Game, Draw, GPU 시간을 측정해 월드 자체의 기준 성능을 확인한다.
2. 현재 동기식 통합 캡처를 활성화하고 일정 시간의 평균값을 측정한다.
3. `FRHIGPUTextureReadback` 기반 비동기 읽기를 적용한다.
4. 실제 저장된 프레임 사이의 간격이 목표인 100ms에 가까운지 manifest로 확인한다.
5. 컬러와 깊이 PNG가 같은 캡처 시점을 유지하는지 확인한다.
6. 변경 전후 평균 FPS와 끊김 정도를 비교한다.
7. 여전히 GPU 비용이 높으면 캡처 해상도, `MaxViewDistanceOverride`, 특정 액터 제외를 별도 A/B 테스트한다.

현재까지의 결론은 통합 렌더링과 효과 제거가 Draw 및 GPU 비용을 낮추는 데에는 유효하지만, 약 10 FPS로 제한되는 주된 문제를 해결하려면 동기식 GPU Readback을 제거해야 한다는 것이다.

## 18. 비동기 GPU Readback 적용 이후 검증 기록

17절의 측정 결과에 따라 `FImageUtils::GetRenderTargetImage()`를 제거하고 `FRHIGPUTextureReadback` 기반 비동기 GPU 읽기를 구현했다. 이 절은 비동기 읽기 적용 이후의 구현 내용, 빌드 및 실행 오류 판단, 성능 변화와 100ms 실시간 캡처 조건에 대한 최종 판단을 기록한다.

더 자세한 스레드 및 슬롯 설계는 [GPU 비동기 이미지 읽기 구현 기록](gpu-async-readback.md)에 별도로 정리했다.

### 18.1 비동기 읽기 적용 내용

기존 동기식 처리는 한 Tick 안에서 장면 캡처와 GPU 결과 대기를 모두 수행했다.

```text
CaptureScene()
  -> GetRenderTargetImage()
  -> GPU 완료까지 게임 스레드 대기
  -> 컬러와 깊이 변환
```

변경 후에는 장면 캡처 시 GPU 복사만 요청하고, 이후 Tick에서 준비된 결과만 처리한다.

```text
캡처 Tick
  -> CaptureScene()
  -> FRHIGPUTextureReadback::EnqueueCopy()
  -> 기다리지 않고 Tick 종료

이후 Tick
  -> IsReady()로 GPU Fence 확인
  -> 준비된 슬롯만 렌더 스레드에서 Lock()
  -> RGBA16f를 CPU 배열로 복사
  -> 컬러와 깊이 PNG 변환 및 저장 큐 등록
```

기본값으로 세 개의 Readback 슬롯을 순환 사용한다. 각 슬롯은 다음 상태를 거친다.

```text
Free
  -> CopyQueued
  -> WaitingForGpu
  -> FinishQueued
  -> CpuDataReady
  -> Free
```

슬롯이 모두 사용 중이면 게임을 멈춰 기다리지 않고 해당 캡처만 건너뛴다. 게임 종료 시에는 진행 중인 GPU 복사를 완료한 뒤 마지막 컬러·깊이 이미지, PNG 저장 작업과 manifest 작성을 순서대로 마무리한다.

manifest 버전은 `1.1`로 변경하고 다음 정보를 추가했다.

```json
{
  "gpu_readback": "FRHIGPUTextureReadback; asynchronous",
  "readback_buffer_count": 3,
  "dropped_capture_count": 0,
  "failed_capture_count": 0
}
```

여러 슬롯의 완료 순서는 요청 순서와 다를 수 있으므로 manifest의 프레임 목록은 Frame Index 기준으로 정렬한다.

### 18.2 구현 중 빌드 오류와 수정

처음에는 헤더에서 전방 선언한 `FBoatCaptureReadbackManager`를 `TUniquePtr`로 보관했다. UnrealHeaderTool이 생성한 생성자 코드에서 불완전 타입의 소멸자를 인스턴스화하면서 다음 컴파일 오류가 발생했다.

```text
error C4150:
불완전한 형식 FBoatCaptureReadbackManager에 대한 포인터를 삭제했습니다.
```

관리자 자체는 게임 스레드 전용 `TSharedPtr`로 보관하고, 렌더 명령까지 수명이 연장되어야 하는 개별 슬롯만 Thread Safe `TSharedPtr`로 전달하도록 변경했다. GPU RHI 객체는 슬롯 내부 `TUniquePtr`로 단독 소유하고 렌더 스레드에서 생성·해제한다.

수정 후 다음 빌드가 통과했다.

```text
UE 5.5
boat_simEditor Win64 Development
UnrealHeaderTool 통과
BoatCaptureComponent.cpp 컴파일 통과
Editor DLL 링크 통과
```

### 18.3 Hot Reload 컴포넌트 오류 판단

비동기 코드 적용 직후 Output Log에 많은 오류가 표시됐지만, 첨부 로그를 집계한 결과 여러 종류의 오류가 아니라 같은 Hot Reload 오류가 여섯 번 반복된 것이었다.

```text
Ensure condition failed: GIsTransacting
HOTRELOAD_BoatCaptureComponent_1
AttachChildren array 불일치
UnrealEditor-HotReload.dll
```

`DrawFrustumComponent`와 `StaticMeshComponent`가 기존 `HOTRELOAD_BoatCaptureComponent`와 새 `BoatCaptureComponent` 사이에서 서로 다른 부모를 가리키며 발생한 Scene Component 계층 불일치였다.

로그 집계 결과는 다음과 같다.

- Handled Ensure: 6회
- Fatal 또는 Crash: 0회
- GPU Readback 관련 오류: 0회
- `BoatCapture:` 런타임 오류: 0회

이 오류는 비동기 GPU Readback 로직의 실패가 아니라, 에디터를 연 상태에서 Scene Component 기반 C++ 클래스를 Hot Reload한 결과다. PIE를 종료하고 Unreal Editor를 완전히 재시작한 뒤 `BP_BoatActor`를 Compile 및 Save하는 방식으로 기존 Hot Reload 인스턴스를 제거해야 한다.

### 18.4 비동기 Readback 적용 후 성능

비동기 Readback 적용 후 같은 노트북 PIE 환경에서 FPS가 약 19~40 사이로 변했고, 측정 화면의 순간값은 26.13 FPS였다.

| 항목 | 비동기 적용 전 | 비동기 적용 후 | 변화 |
|---|---:|---:|---:|
| FPS | 9.37 | 26.13 | 약 2.8배 증가 |
| 프레임 시간 | 106.68ms | 38.27ms | 약 64% 감소 |
| Draw | 75.17ms | 31.18ms | 약 58% 감소 |
| GPU Time | 68.84ms | 11.24ms | 약 84% 감소 |
| World Tick Time Inclusive Average | 101.68ms | 13.98ms | 약 86% 감소 |

동기식 GPU 읽기를 제거하면서 World Tick과 GPU 시간이 크게 감소했다. 따라서 이전 약 10 FPS 병목의 주원인이 `GetRenderTargetImage()`의 동기식 대기였다는 판단을 측정으로 확인했다.

FPS가 19~40 사이에서 계속 변하는 이유는 다음 주기적 작업이 남아 있기 때문이다.

- 100ms마다 추가 Scene Capture 렌더링
- 1280×720 RGBA16f Render Target의 GPU 복사
- 약 92만 픽셀의 HDR 톤 매핑
- 약 92만 픽셀의 깊이 정규화
- 컬러 및 깊이 배열 생성과 PNG 저장 큐 전달
- PIE, 성능 통계와 AI 로깅의 추가 측정 비용

현재 `SaveReadbackFrame()`의 컬러·깊이 픽셀 변환은 게임 스레드에서 실행된다. GPU 대기 자체는 사라졌지만 이 변환이 수행되는 프레임과 수행되지 않는 프레임의 비용이 달라 FPS가 주기적으로 흔들릴 수 있다.

### 18.5 최신 저장 세션 결과

비동기 Readback 적용 후 최신 manifest는 다음 결과를 기록했다.

| 항목 | 측정값 |
|---|---:|
| Frame Count | 524 |
| Readback Buffer Count | 3 |
| Dropped Capture Count | 0 |
| Failed Capture Count | 0 |
| 설정 간격 | 100ms |
| 첫 Timestamp | 약 4.99ms |
| 마지막 Timestamp | 약 54,013.07ms |
| 저장된 프레임 사이 평균 간격 | 약 103.27ms |
| 최소 측정 간격 | 약 42.44ms |
| 최대 측정 간격 | 약 1,290.42ms |

전체 523개 간격 중 150ms를 초과한 구간은 14개, 200ms를 초과한 구간은 4개였다. 가장 긴 약 1.29초 구간은 에디터 또는 프로파일링 과정의 일시적인 정지로 판단된다.

Dropped와 Failed가 모두 0이므로 Readback 슬롯 포화나 GPU 복사 실패로 이미지를 버린 것은 아니다. 평균 간격은 목표 100ms에 가깝지만 PIE 프레임 경계와 에디터 Hitch 때문에 개별 간격에는 오차가 존재한다.

### 18.6 노트북과 에디터 환경의 영향

이번 성능 수치는 노트북에서 Unreal Editor PIE, 성능 통계와 AI 로깅을 함께 활성화한 상태에서 측정했다. 실제 제출 또는 데스크탑 환경보다 불리할 수 있는 조건이다.

- 노트북 GPU의 낮은 처리량과 메모리 대역폭
- 전력 제한 및 발열에 따른 Clock 하락
- 내장·외장 GPU 전환 또는 공유 메모리 영향
- Unreal Editor 및 PIE 자체 오버헤드
- AI 로깅과 성능 통계 수집 비용
- Codex, 브라우저 등 동시 실행 프로그램의 CPU·GPU 사용

데스크탑의 Standalone 또는 Development 패키징 빌드에서는 성능이 더 좋아질 가능성이 높다. 다만 동기식 GPU Readback은 장비가 빨라도 게임 스레드와 GPU를 강제로 동기화하는 구조이므로 비동기 Readback 변경은 유지해야 한다.

반면 픽셀 변환을 추가로 작업 스레드로 옮기는 최적화는 데스크탑 제출 환경을 먼저 측정한 뒤 결정한다. 안정적인 30 FPS 이상, 눈에 띄는 자율주행 끊김 없음, Dropped와 Failed가 0이면 현재 구현으로 충분할 수 있다.

### 18.7 과제의 실시간 100ms 조건 판단

과제 조건은 다음과 같다.

```text
저장 간격 = real-time 기준 100ms
한 레벨 플레이 동안 폴더에 순차 저장
```

현재 코드는 `FPlatformTime::Seconds()`를 사용해 프레임 수나 게임 시간 배속이 아닌 실제 경과 시간을 기준으로 다음 캡처 시점을 계산한다.

```cpp
const double CurrentTimeSeconds{FPlatformTime::Seconds()};
const double SafeIntervalSeconds{0.1};
```

파일 이름도 다음과 같이 순차 생성한다.

```text
color/frame_000000.png
color/frame_000001.png
...

depth/frame_000000.png
depth/frame_000001.png
...
```

따라서 다음 조건은 충족한다.

- 실제 경과 시간 기준 스케줄링
- 목표 주기 100ms, 즉 10Hz
- 컬러와 깊이의 동일 Frame Index 사용
- 한 실행별 폴더 생성
- 연속된 번호의 순차 파일 저장
- manifest에 실제 캡처 요청 Timestamp 기록

다만 Scene Capture는 게임 프레임 사이에서 독립적으로 렌더링할 수 없으므로 모든 개별 이미지가 정확히 `100.000ms` 간격으로 생성되는 Hard Real-time 보장은 하지 않는다.

```text
100ms 시점 도달
  -> 해당 시점 이후 최초 게임 Tick
  -> 장면 캡처 요청
```

실행 FPS가 낮거나 에디터가 순간적으로 멈추면 해당 캡처 시점도 다음 프레임까지 늦어진다. 일반적인 과제 해석인 “프레임 수가 아니라 실제 시간 기준으로 약 10Hz 저장”에는 맞지만, 각 간격에 오차가 전혀 없어야 하는 엄격한 해석에는 맞지 않는다.

안정적으로 10Hz에 가까운 데이터를 얻으려면 실행 FPS가 항상 10 FPS보다 높아야 하며, 에디터 Hot Reload와 프로파일링 Hitch가 없는 환경에서 검증해야 한다.

### 18.8 제출 전 재검증 순서

1. Unreal Editor를 완전히 재시작해 `HOTRELOAD_` 컴포넌트를 제거한다.
2. 노트북을 전원에 연결하고 고성능 모드 및 외장 GPU 사용을 확인한다.
3. AI 로깅과 `stat game`, `stat unit` 표시를 끈 상태로 실행한다.
4. PIE가 아닌 Standalone Game에서 먼저 측정한다.
5. 가능하면 데스크탑의 Development 패키징 빌드로 다시 측정한다.
6. 30초 이상 실행해 평균 FPS와 최저 FPS를 기록한다.
7. manifest의 `frame_count`, `dropped_capture_count`, `failed_capture_count`를 확인한다.
8. Timestamp 평균 간격과 150ms 이상 지연 구간 수를 확인한다.
9. 컬러와 깊이 파일이 같은 번호로 연속 생성되는지 확인한다.
10. 저장 이미지의 색, 방향과 깊이 값이 이전 방식과 같은 의미를 유지하는지 확인한다.

데스크탑 Standalone 환경에서도 20 FPS 아래로 반복해서 떨어지거나 자율주행에 눈에 띄는 끊김이 남을 때에는 `SaveReadbackFrame()`의 HDR 톤 매핑과 깊이 변환을 제한된 작업 스레드 큐로 이동하는 방안을 다음 최적화로 검토한다.

## Part 2C 단일 바이너리 아카이브 적용

기존 Color·Depth PNG와 Manifest 1.1 저장을 유지하면서 같은 세션에 `capture.boatbin`을 추가했다. 필수 과제 결과를 그대로 검증할 수 있고, 선택 과제 웹 뷰어는 바이너리 한 파일만으로 재생할 수 있다.

바이너리는 Prefix, UTF-8 Metadata, 고정 크기 Frame Index, PNG Payload 순서로 구성된다. 각 프레임의 Color·Depth Offset과 Length를 `uint64`로 기록해 파일 전체를 풀지 않고 원하는 이미지에 바로 접근한다. PNG가 이미 압축된 데이터이므로 아카이브 전체에 추가 압축을 적용하지 않았다.

아카이브 생성은 실시간 캡처 중이 아니라 게임 종료 시 `ImageWriteQueue` Fence가 끝난 뒤 실행한다. PNG를 1MB 단위로 복사하므로 전체 세션을 메모리에 올리지 않으며, 임시 파일이 완성된 경우에만 `capture.boatbin`으로 이름을 변경한다.

웹 뷰어는 Prefix·Metadata·Index만 먼저 읽고 현재 프레임의 PNG 구간만 `File.slice()`로 참조한다. 기존 폴더 입력과 바이너리 입력은 같은 Color·Depth Blob 재생 경로를 공유한다. Depth는 기존 `RGB8; near=255; far=0` PNG를 그대로 표시한다.

Part 2C 성능 측정은 Color와 Depth를 각각 이미지 한 장으로 계산하며 다음 시간을 포함한다.

```text
Binary File Slice의 ArrayBuffer 읽기 + PNG createImageBitmap 디코딩
```

최대 10장 워밍업을 제외하고 전체 이미지에 대해 `순차 → 랜덤 → 랜덤 → 순차`로 측정한다. 랜덤 순서는 고정 Seed `0xB0A7`을 사용한다. 화면에는 순차·랜덤 평균 Read, Decode, Total ms/image를 표시하며, 동일한 값과 브라우저·아카이브 정보를 JSON으로 저장할 수 있다.

상세한 바이트 배치와 측정 방법은 `Docs/part2c-binary-archive.md`에 정리했다.
