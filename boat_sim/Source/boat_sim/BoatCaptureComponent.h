// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneCaptureComponent2D.h"
#include "BoatCaptureComponent.generated.h"

class IImageWriteQueue;
class UTextureRenderTarget2D;

// 선박 시점의 컬러 화면과 깊이 정보를 일정한 간격으로 저장하는 컴포넌트
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BOAT_SIM_API UBoatCaptureComponent : public USceneCaptureComponent2D
{
	GENERATED_BODY()

public:
	// 캡처 카메라와 저장 설정 초기화
	UBoatCaptureComponent();

protected:
	// 게임 시작 시 렌더 타깃과 실행별 저장 폴더 생성
	virtual void BeginPlay() override;

	// 게임 종료 전 대기 중인 이미지 저장과 manifest 작성 완료
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

	// 깊이 이미지에서 검은색으로 처리할 최대 거리
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float DepthMaxDistance{5000.0f};

	// Exposure Compensation(노출 보정): HDR 컬러 밝기를 조절하는 EV 값
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture", meta = (UIMin = "-5.0", UIMax = "5.0", Units = "EV"))
	float ColorExposureCompensation{0.0f};

	// Saved 폴더 아래에 생성할 캡처 루트 폴더 이름
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat|Capture")
	FString OutputFolderName{TEXT("BoatCaptures")};

private:
	// 통합 컬러와 깊이 캡처에 사용할 런타임 렌더 타깃 생성
	bool InitializeRenderTargets();

	// 캡처 결과에 필요하지 않은 후처리와 렌더링 효과 제거
	void ConfigureCaptureRendering();

	// 실행 시각을 기준으로 컬러와 깊이 저장 폴더 생성
	bool InitializeOutputDirectories();

	// 동일한 카메라 위치에서 컬러와 깊이 한 쌍 캡처
	void CaptureFrame(double ElapsedSeconds);

	// 한 번 캡처한 HDR 컬러와 깊이를 각각 PNG 저장용 픽셀로 변환
	bool ReadCombinedPixels(TArray64<FColor>& OutColorPixels, TArray64<FColor>& OutDepthPixels);

	// 이미지 픽셀을 비동기 PNG 저장 큐에 등록
	bool EnqueuePngWrite(TArray64<FColor>&& Pixels, const FString& Filename) const;

	// 캡처 설정과 프레임별 시각을 JSON 파일로 기록
	void WriteManifest() const;

	// RGB에는 HDR 컬러, A에는 실거리 깊이를 받을 통합 렌더 타깃
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> CombinedRenderTarget;

	// 엔진에서 관리하는 비동기 이미지 저장 큐
	IImageWriteQueue* ImageWriteQueue{nullptr};

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

	// manifest에 기록할 저장 프레임 번호
	TArray<int32> CapturedFrameIndices;

	// manifest에 기록할 프레임별 실제 경과 시간
	TArray<double> CapturedFrameTimesMilliseconds;

	// 렌더 타깃과 저장 폴더 초기화 성공 여부
	bool bIsCapturing{false};
};
