// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneCaptureComponent2D.h"
#include "BoatCaptureComponent.generated.h"

class IImageWriteQueue;
class FBoatCaptureReadbackManager;
class UTextureRenderTarget2D;

// 선박 시점의 컬러 화면과 깊이 정보를 일정한 간격으로 저장하는 컴포넌트
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BOAT_SIM_API UBoatCaptureComponent : public USceneCaptureComponent2D
{
	GENERATED_BODY()

public:
	// 캡처 카메라와 저장 설정 초기화
	UBoatCaptureComponent();

	// 비동기 GPU 읽기 관리 객체의 수명을 컴포넌트와 함께 정리
	virtual ~UBoatCaptureComponent() override;

protected:
	// 게임 시작 시 렌더 타깃과 실행별 저장 폴더 생성
	virtual void BeginPlay() override;

	// 게임 종료 전 대기 중인 이미지 저장과 manifest·binary archive 작성 완료
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	// 실제 시간 100ms 간격에 맞춰 컬러와 깊이 화면 캡처
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

protected:
	// 게임 시작과 함께 자동으로 캡처할지 여부
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture")
	bool bAutoCapture{true};

	// 저장할 이미지의 가로 해상도
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (ClampMin = "1", UIMin = "1"))
	int32 CaptureWidth{1280};

	// 저장할 이미지의 세로 해상도
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (ClampMin = "1", UIMin = "1"))
	int32 CaptureHeight{720};

	// 실제 시간 기준 이미지 저장 간격
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (ClampMin = "0.01", UIMin = "0.01", Units = "s"))
	float CaptureIntervalSeconds{0.1f};

	// GPU 결과를 기다리는 동안 다음 캡처를 받을 비동기 Readback Buffer 개수
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (ClampMin = "2", ClampMax = "8", UIMin = "2", UIMax = "8"))
	int32 ReadbackBufferCount{3};

	// 깊이 이미지에서 검은색으로 처리할 최대 거리
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float DepthMaxDistance{5000.0f};

	// Exposure Compensation(노출 보정): HDR 컬러 밝기를 조절하는 EV 값
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (UIMin = "-5.0", UIMax = "5.0", Units = "EV"))
	float ColorExposureCompensation{0.0f};

	// Saved 폴더 아래에 생성할 캡처 루트 폴더 이름
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture")
	FString OutputFolderName{TEXT("BoatCaptures")};

	// Binary Archive(바이너리 아카이브): 저장된 모든 Color·Depth PNG를 하나의 파일로 묶을지 여부
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture")
	bool bWriteBinaryArchive{true};

private:
	// 통합 컬러와 깊이 캡처에 사용할 런타임 렌더 타깃 생성
	bool InitializeRenderTargets();

	// 여러 캡처가 GPU에서 처리되는 동안 순환 사용할 Readback 슬롯 생성
	bool InitializeReadbackSlots();

	// 캡처 결과에 필요하지 않은 후처리와 렌더링 효과 제거
	void ConfigureCaptureRendering();

	// 실행 시각을 기준으로 컬러와 깊이 저장 폴더 생성
	bool InitializeOutputDirectories();

	// 장면 캡처 뒤 GPU 결과 복사만 요청하고 기다리지 않고 반환
	void RequestCaptureReadback(double ElapsedSeconds);

	// GPU Fence가 완료된 슬롯의 CPU 복사를 렌더 스레드에 요청
	void QueueReadyReadbacks(bool bForceFinish);

	// 렌더 스레드가 복사를 끝낸 슬롯을 컬러와 깊이 PNG 저장 작업으로 전달
	void ProcessCompletedReadbacks();

	// GPU에서 복사된 RGBA16f 픽셀을 컬러와 깊이 PNG 저장용 픽셀로 변환
	bool SaveReadbackFrame(int32 FrameIndex, double ElapsedSeconds, const TArray<FFloat16Color>& SourcePixels);

	// 게임 종료 시 진행 중인 Readback을 완료해 마지막 이미지까지 저장
	void FlushPendingReadbacks();

	// 렌더 스레드에서 GPU Readback 자원을 해제한 뒤 관리 객체 정리
	void ReleaseReadbackResources();

	// 이미지 픽셀을 비동기 PNG 저장 큐에 등록
	bool EnqueuePngWrite(TArray64<FColor>&& Pixels, const FString& Filename) const;

	// 캡처 설정과 프레임별 시각을 JSON 파일로 기록
	void WriteManifest() const;

	// PNG 저장이 끝난 뒤 모든 프레임을 임의 접근 가능한 단일 바이너리 파일로 묶기
	bool WriteBinaryArchive() const;

	// RGB에는 HDR 컬러, A에는 실거리 깊이를 받을 통합 렌더 타깃
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> CombinedRenderTarget;

	// 엔진에서 관리하는 비동기 이미지 저장 큐
	IImageWriteQueue* ImageWriteQueue{nullptr};

	// GPU 복사 요청과 완료 픽셀을 보관하는 게임 스레드 전용 순환 Readback 슬롯 관리자
	TSharedPtr<FBoatCaptureReadbackManager, ESPMode::NotThreadSafe> ReadbackManager;

	// 현재 실행의 이미지가 저장되는 폴더
	FString SessionOutputDirectory;

	// 컬러 이미지 저장 폴더
	FString ColorOutputDirectory;

	// 깊이 이미지 저장 폴더
	FString DepthOutputDirectory;

	// 캡처 시작 실제 시각
	double CaptureStartTimeSeconds{0.0};

	// 다음 이미지를 캡처할 실제 시각
	double NextCaptureTimeSeconds{0.0};

	// 다음 파일 이름에 사용할 프레임 번호
	int32 NextFrameIndex{0};

	// 모든 Readback 슬롯이 사용 중이어서 기다리지 않고 건너뛴 캡처 수
	int32 DroppedCaptureCount{0};

	// GPU 자원 또는 픽셀 복사 문제로 저장하지 못한 캡처 수
	int32 FailedCaptureCount{0};

	// manifest에 기록할 저장 프레임 번호
	TArray<int32> CapturedFrameIndices;

	// manifest에 기록할 프레임별 실제 경과 시간
	TArray<double> CapturedFrameTimesMilliseconds;

	// 렌더 타깃과 저장 폴더 초기화 성공 여부
	bool bIsCapturing{false};
};
