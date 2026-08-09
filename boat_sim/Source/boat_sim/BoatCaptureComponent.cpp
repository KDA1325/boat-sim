// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#include "BoatCaptureComponent.h"

#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImagePixelData.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "Math/Float16Color.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <atomic>

namespace BoatCapture
{
	constexpr double MinimumCaptureIntervalSeconds{0.01};
	constexpr int32 PngCompressionQuality{0};
	constexpr int32 MinimumReadbackBufferCount{2};
	constexpr int32 MaximumReadbackBufferCount{8};

	// 한 Readback 슬롯이 게임 스레드, 렌더 스레드, GPU 중 어디까지 처리됐는지 표시
	enum class EReadbackState : uint8
	{
		Free,
		CopyQueued,
		WaitingForGpu,
		FinishQueued,
		CpuDataReady,
		Failed
	};

	// 캡처 한 장의 GPU 복사 자원과 프레임 정보를 함께 보관하는 순환 슬롯
	struct FReadbackSlot final
	{
		explicit FReadbackSlot(const int32 SlotIndex)
			: ReadbackName{*FString::Printf(TEXT("BoatCaptureReadback_%d"), SlotIndex)}
		{
		}

		// 여러 스레드가 같은 슬롯의 진행 상태를 확인하므로 원자적으로 변경
		std::atomic<EReadbackState> State{EReadbackState::Free};

		// GPU Fence와 CPU에서 읽을 수 있는 Staging Texture를 관리하는 UE RHI 객체
		TUniquePtr<FRHIGPUTextureReadback> GpuReadback;

		// 렌더 스레드가 Staging Texture에서 복사하고 게임 스레드가 PNG로 변환할 픽셀
		TArray<FFloat16Color> CpuPixels;

		// Readback 자원을 구분하기 위해 슬롯마다 부여하는 디버그 이름
		FName ReadbackName;

		// 요청 순서와 파일 이름을 일치시키는 캡처 프레임 번호
		int32 FrameIndex{-1};

		// 캡처 시작 이후 실제 경과 시간
		double ElapsedSeconds{0.0};
	};

	// UE 기본 Filmic Tonemapper와 비슷한 명암 압축을 만드는 ACES 근사식
	float ApplyAcesToneCurve(const float HdrValue)
	{
		const float SafeHdrValue{FMath::IsFinite(HdrValue) ? FMath::Max(HdrValue, 0.0f) : 0.0f};
		const float Numerator{SafeHdrValue * (2.51f * SafeHdrValue + 0.03f)};
		const float Denominator{SafeHdrValue * (2.43f * SafeHdrValue + 0.59f) + 0.14f};
		return FMath::Clamp(Numerator / Denominator, 0.0f, 1.0f);
	}

	// HDR Scene Color에 고정 노출과 Filmic Tone Curve를 적용해 sRGB 8비트 컬러로 변환
	FColor ToneMapSceneColor(const FLinearColor& HdrColor, const float ExposureCompensation)
	{
		const float ExposureScale{FMath::Pow(2.0f, ExposureCompensation)};
		const FLinearColor ToneMappedColor{
			ApplyAcesToneCurve(HdrColor.R * ExposureScale),
			ApplyAcesToneCurve(HdrColor.G * ExposureScale),
			ApplyAcesToneCurve(HdrColor.B * ExposureScale),
			1.0f};
		return ToneMappedColor.ToFColorSRGB();
	}

	// 기존 블루프린트 Show Flag 설정을 보존하면서 지정한 항목만 변경
	void SetShowFlagEnabled(
		TArray<FEngineShowFlagsSetting>& ShowFlagSettings,
		const TCHAR* ShowFlagName,
		const bool bEnabled)
	{
		FEngineShowFlagsSetting* const ExistingSetting{ShowFlagSettings.FindByPredicate(
			[ShowFlagName](const FEngineShowFlagsSetting& Setting)
			{
				return Setting.ShowFlagName == ShowFlagName;
			})};

		if (ExistingSetting != nullptr)
		{
			ExistingSetting->Enabled = bEnabled;
			return;
		}

		FEngineShowFlagsSetting NewSetting;
		NewSetting.ShowFlagName = ShowFlagName;
		NewSetting.Enabled = bEnabled;
		ShowFlagSettings.Add(MoveTemp(NewSetting));
	}
}

// Readback 슬롯의 생성과 빈 슬롯 선택을 담당하는 내부 관리 객체
class FBoatCaptureReadbackManager final
{
public:
	explicit FBoatCaptureReadbackManager(const int32 BufferCount)
	{
		Slots.Reserve(BufferCount);
		for (int32 SlotIndex = 0; SlotIndex < BufferCount; ++SlotIndex)
		{
			Slots.Add(MakeShared<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>(SlotIndex));
		}
	}

	// GPU 작업이 끝난 슬롯만 재사용해 아직 처리 중인 캡처 결과를 덮어쓰지 않음
	TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe> AcquireFreeSlot()
	{
		for (const TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>& Slot : Slots)
		{
			BoatCapture::EReadbackState ExpectedState{BoatCapture::EReadbackState::Free};
			if (Slot->State.compare_exchange_strong(
				ExpectedState,
				BoatCapture::EReadbackState::CopyQueued,
				std::memory_order_acq_rel))
			{
				return Slot;
			}
		}

		return nullptr;
	}

	const TArray<TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>>& GetSlots() const
	{
		return Slots;
	}

private:
	// TSharedPtr로 렌더 명령까지 슬롯 수명을 연장해 컴포넌트 종료 중 접근 오류 방지
	TArray<TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>> Slots;
};

// 캡처 카메라와 저장 설정 초기화
UBoatCaptureComponent::UBoatCaptureComponent()
{
	// 선박 물리 이동이 끝난 뒤 캡처 시간 확인
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// 정해진 100ms 시점에만 직접 장면 캡처
	bCaptureEveryFrame = false;
	bCaptureOnMovement = false;
	FOVAngle = 90.0f;
}

UBoatCaptureComponent::~UBoatCaptureComponent() = default;

// 게임 시작 시 렌더 타깃과 실행별 저장 폴더 생성
void UBoatCaptureComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!bAutoCapture)
	{
		SetComponentTickEnabled(false);
		return;
	}

	ConfigureCaptureRendering();
	ImageWriteQueue = &FModuleManager::LoadModuleChecked<IImageWriteQueueModule>(TEXT("ImageWriteQueue")).GetWriteQueue();

	if (!InitializeRenderTargets() || !InitializeOutputDirectories() || !InitializeReadbackSlots())
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: 캡처 초기화에 실패했습니다."));
		SetComponentTickEnabled(false);
		return;
	}

	CaptureStartTimeSeconds = FPlatformTime::Seconds();
	NextCaptureTimeSeconds = CaptureStartTimeSeconds;
	NextFrameIndex = 0;
	DroppedCaptureCount = 0;
	FailedCaptureCount = 0;
	CapturedFrameIndices.Reset();
	CapturedFrameTimesMilliseconds.Reset();
	bIsCapturing = true;

	UE_LOG(LogTemp, Log, TEXT("BoatCapture: 이미지 저장을 시작합니다. %s"), *SessionOutputDirectory);
}

// 게임 종료 전 대기 중인 이미지 저장과 manifest 작성 완료
void UBoatCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bIsCapturing = false;
	SetComponentTickEnabled(false);

	// 실행 중에는 기다리지 않지만 종료 시에는 남은 GPU 결과까지 회수해 파일 누락 방지
	FlushPendingReadbacks();

	if (ImageWriteQueue != nullptr && !SessionOutputDirectory.IsEmpty())
	{
		// 비동기 PNG 파일이 모두 저장된 뒤 manifest 작성
		ImageWriteQueue->CreateFence().Wait();
		WriteManifest();
	}

	// FRHIGPUTextureReadback은 생성하고 사용한 렌더 스레드에서 안전하게 해제
	ReleaseReadbackResources();

	TextureTarget = nullptr;
	CombinedRenderTarget = nullptr;
	ImageWriteQueue = nullptr;

	Super::EndPlay(EndPlayReason);
}

// 실제 시간 100ms 간격에 맞춰 컬러와 깊이 화면 캡처
void UBoatCaptureComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsCapturing)
	{
		return;
	}

	// 이전 프레임의 GPU Fence만 확인하므로 준비되지 않은 결과를 기다리지 않음
	QueueReadyReadbacks(false);
	ProcessCompletedReadbacks();

	const double CurrentTimeSeconds{FPlatformTime::Seconds()};
	if (CurrentTimeSeconds < NextCaptureTimeSeconds)
	{
		return;
	}

	RequestCaptureReadback(CurrentTimeSeconds - CaptureStartTimeSeconds);

	const double SafeIntervalSeconds{
		FMath::Max(static_cast<double>(CaptureIntervalSeconds), BoatCapture::MinimumCaptureIntervalSeconds)};
	NextCaptureTimeSeconds += SafeIntervalSeconds;

	// 프레임 지연이 생겨도 한 Tick에서 지난 이미지를 몰아서 저장하지 않음
	if (NextCaptureTimeSeconds <= CurrentTimeSeconds)
	{
		NextCaptureTimeSeconds = CurrentTimeSeconds + SafeIntervalSeconds;
	}
}

// 통합 컬러와 깊이 캡처에 사용할 런타임 렌더 타깃 생성
bool UBoatCaptureComponent::InitializeRenderTargets()
{
	if (CaptureWidth <= 0 || CaptureHeight <= 0 || DepthMaxDistance <= 0.0f)
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: 해상도와 최대 깊이는 0보다 커야 합니다."));
		return false;
	}

	CombinedRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("BoatCombinedRenderTarget"));
	if (CombinedRenderTarget == nullptr)
	{
		return false;
	}

	// Render Target Format의 RGBA16f: HDR RGB와 센티미터 단위 깊이 A를 함께 보존
	CombinedRenderTarget->RenderTargetFormat = RTF_RGBA16f;
	CombinedRenderTarget->ClearColor = FLinearColor::Black;
	CombinedRenderTarget->bAutoGenerateMips = false;
	CombinedRenderTarget->InitAutoFormat(CaptureWidth, CaptureHeight);
	CombinedRenderTarget->UpdateResourceImmediate(true);

	// Capture Source의 SceneColor (HDR) in RGB, SceneDepth in A를 사용해 한 번만 렌더링
	TextureTarget = CombinedRenderTarget;
	CaptureSource = SCS_SceneColorSceneDepth;
	return true;
}

// 여러 캡처가 GPU에서 처리되는 동안 순환 사용할 Readback 슬롯 생성
bool UBoatCaptureComponent::InitializeReadbackSlots()
{
	const int32 SafeBufferCount{FMath::Clamp(
		ReadbackBufferCount,
		BoatCapture::MinimumReadbackBufferCount,
		BoatCapture::MaximumReadbackBufferCount)};

	ReadbackManager = MakeShared<FBoatCaptureReadbackManager, ESPMode::NotThreadSafe>(SafeBufferCount);
	return ReadbackManager.IsValid();
}

// 캡처 결과에 필요하지 않은 후처리와 렌더링 효과 제거
void UBoatCaptureComponent::ConfigureCaptureRendering()
{
	TArray<FEngineShowFlagsSetting> CaptureShowFlagSettings{GetShowFlagSettings()};

	// Motion Blur(모션 블러): 이동 방향을 따라 화면을 흐리게 만드는 효과 제거
	BoatCapture::SetShowFlagEnabled(CaptureShowFlagSettings, TEXT("MotionBlur"), false);

	// Depth of Field(피사계 심도): 초점 거리 밖의 화면을 흐리게 만드는 효과 제거
	BoatCapture::SetShowFlagEnabled(CaptureShowFlagSettings, TEXT("DepthOfField"), false);

	// Bloom(블룸): 매우 밝은 픽셀 주변에 빛이 번지는 효과 제거
	BoatCapture::SetShowFlagEnabled(CaptureShowFlagSettings, TEXT("Bloom"), false);

	// Lens Flares(렌즈 플레어): 강한 광원을 볼 때 렌즈 내부 반사를 표현하는 효과 제거
	BoatCapture::SetShowFlagEnabled(CaptureShowFlagSettings, TEXT("LensFlares"), false);

	// Screen Space Reflections(스크린 스페이스 리플렉션): 화면에 보이는 정보로 계산하는 반사 효과 제거
	BoatCapture::SetShowFlagEnabled(CaptureShowFlagSettings, TEXT("ScreenSpaceReflections"), false);

	// Volumetric Fog(볼류메트릭 포그): 공간 안에서 빛이 안개에 산란되는 효과 제거
	BoatCapture::SetShowFlagEnabled(CaptureShowFlagSettings, TEXT("VolumetricFog"), false);

	SetShowFlagSettings(CaptureShowFlagSettings);

	// Use Ray Tracing If Enabled: 프로젝트 설정과 관계없이 이 캡처 카메라의 레이 트레이싱 제거
	bUseRayTracingIfEnabled = false;
}

// 실행 시각을 기준으로 컬러와 깊이 저장 폴더 생성
bool UBoatCaptureComponent::InitializeOutputDirectories()
{
	const FDateTime CurrentTime{FDateTime::Now()};
	const FString SessionName{FString::Printf(
		TEXT("%s_%03d"),
		*CurrentTime.ToString(TEXT("%Y%m%d_%H%M%S")),
		CurrentTime.GetMillisecond())};
	const FString SafeOutputFolder{OutputFolderName.IsEmpty() ? TEXT("BoatCaptures") : OutputFolderName};

	SessionOutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), SafeOutputFolder, SessionName);
	ColorOutputDirectory = FPaths::Combine(SessionOutputDirectory, TEXT("color"));
	DepthOutputDirectory = FPaths::Combine(SessionOutputDirectory, TEXT("depth"));

	IFileManager& FileManager{IFileManager::Get()};
	const bool bCreatedColorDirectory{FileManager.MakeDirectory(*ColorOutputDirectory, true)};
	const bool bCreatedDepthDirectory{FileManager.MakeDirectory(*DepthOutputDirectory, true)};
	return bCreatedColorDirectory && bCreatedDepthDirectory;
}

// 장면 캡처 뒤 GPU 결과 복사만 요청하고 기다리지 않고 반환
void UBoatCaptureComponent::RequestCaptureReadback(const double ElapsedSeconds)
{
	if (!ReadbackManager.IsValid() || CombinedRenderTarget == nullptr)
	{
		++FailedCaptureCount;
		return;
	}

	// 빈 슬롯이 없으면 게임을 멈춰 기다리지 않고 이번 캡처만 건너뜀
	const TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe> Slot{ReadbackManager->AcquireFreeSlot()};
	if (!Slot.IsValid())
	{
		++DroppedCaptureCount;
		return;
	}

	FTextureRenderTargetResource* const RenderTargetResource{
		CombinedRenderTarget->GameThread_GetRenderTargetResource()};
	if (RenderTargetResource == nullptr)
	{
		++FailedCaptureCount;
		Slot->State.store(BoatCapture::EReadbackState::Free, std::memory_order_release);
		return;
	}

	Slot->FrameIndex = NextFrameIndex++;
	Slot->ElapsedSeconds = ElapsedSeconds;
	Slot->CpuPixels.Reset();

	TextureTarget = CombinedRenderTarget;
	CaptureSource = SCS_SceneColorSceneDepth;

	// CaptureScene은 장면 렌더 명령만 등록하며 아래 Readback 복사는 그 명령 뒤에 실행됨
	CaptureScene();

	// 렌더 스레드에서 Render Target을 CPUReadback용 Staging Texture로 복사
	// 여기서는 복사가 끝날 때까지 기다리지 않고 명령만 큐에 넣어 게임 Tick을 즉시 반환
	ENQUEUE_RENDER_COMMAND(FBoatCaptureBeginReadback)(
		[Slot, RenderTargetResource](FRHICommandListImmediate& RHICmdList)
		{
			const FTextureRHIRef& SourceTexture{RenderTargetResource->GetRenderTargetTexture()};
			if (!SourceTexture.IsValid())
			{
				Slot->State.store(BoatCapture::EReadbackState::Failed, std::memory_order_release);
				return;
			}

			// RHI 객체는 실제로 사용하는 렌더 스레드에서 생성하고 같은 슬롯에서 재사용
			if (!Slot->GpuReadback.IsValid())
			{
				Slot->GpuReadback = MakeUnique<FRHIGPUTextureReadback>(Slot->ReadbackName);
			}

			// EnqueueCopy는 GPU 복사와 Fence 기록만 예약하므로 CPU나 게임 스레드를 대기시키지 않음
			Slot->GpuReadback->EnqueueCopy(RHICmdList, SourceTexture.GetReference());
			Slot->State.store(BoatCapture::EReadbackState::WaitingForGpu, std::memory_order_release);
		});
}

// GPU Fence가 완료된 슬롯의 CPU 복사를 렌더 스레드에 요청
void UBoatCaptureComponent::QueueReadyReadbacks(const bool bForceFinish)
{
	if (!ReadbackManager.IsValid())
	{
		return;
	}

	for (const TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>& Slot : ReadbackManager->GetSlots())
	{
		if (Slot->State.load(std::memory_order_acquire) != BoatCapture::EReadbackState::WaitingForGpu)
		{
			continue;
		}

		if (!Slot->GpuReadback.IsValid())
		{
			Slot->State.store(BoatCapture::EReadbackState::Failed, std::memory_order_release);
			continue;
		}

		// 실행 중에는 Fence가 준비된 슬롯만 처리해 Lock에서 GPU를 기다리는 상황 방지
		if (!bForceFinish && !Slot->GpuReadback->IsReady())
		{
			continue;
		}

		BoatCapture::EReadbackState ExpectedState{BoatCapture::EReadbackState::WaitingForGpu};
		if (!Slot->State.compare_exchange_strong(
			ExpectedState,
			BoatCapture::EReadbackState::FinishQueued,
			std::memory_order_acq_rel))
		{
			continue;
		}

		const int32 ReadbackWidth{CaptureWidth};
		const int32 ReadbackHeight{CaptureHeight};

		// Lock과 Unlock은 RHI 자원을 소유한 렌더 스레드에서 실행
		// bForceFinish는 게임 종료 시에만 true이며 마지막 GPU 복사가 끝날 때까지 정리 목적으로 대기
		ENQUEUE_RENDER_COMMAND(FBoatCaptureFinishReadback)(
			[Slot, ReadbackWidth, ReadbackHeight](FRHICommandListImmediate& RHICmdList)
			{
				if (!Slot->GpuReadback.IsValid())
				{
					Slot->State.store(BoatCapture::EReadbackState::Failed, std::memory_order_release);
					return;
				}

				int32 RowPitchInPixels{0};
				int32 BufferHeight{0};
				void* const SourceData{Slot->GpuReadback->Lock(RowPitchInPixels, &BufferHeight)};
				if (SourceData == nullptr || RowPitchInPixels < ReadbackWidth || BufferHeight < ReadbackHeight)
				{
					if (SourceData != nullptr)
					{
						Slot->GpuReadback->Unlock();
					}

					Slot->State.store(BoatCapture::EReadbackState::Failed, std::memory_order_release);
					return;
				}

				// GPU 행 간격에는 정렬용 여백이 있을 수 있어 실제 이미지 너비만 행별로 복사
				Slot->CpuPixels.SetNumUninitialized(ReadbackWidth * ReadbackHeight);
				const FFloat16Color* SourceRow{static_cast<const FFloat16Color*>(SourceData)};
				FFloat16Color* DestinationRow{Slot->CpuPixels.GetData()};
				for (int32 RowIndex = 0; RowIndex < ReadbackHeight; ++RowIndex)
				{
					FMemory::Memcpy(
						DestinationRow,
						SourceRow,
						static_cast<SIZE_T>(ReadbackWidth) * sizeof(FFloat16Color));
					SourceRow += RowPitchInPixels;
					DestinationRow += ReadbackWidth;
				}

				// 필요한 픽셀을 일반 CPU 배열로 옮겼으므로 Staging Texture 매핑 즉시 해제
				Slot->GpuReadback->Unlock();
				Slot->State.store(BoatCapture::EReadbackState::CpuDataReady, std::memory_order_release);
			});
	}
}

// 렌더 스레드가 복사를 끝낸 슬롯을 컬러와 깊이 PNG 저장 작업으로 전달
void UBoatCaptureComponent::ProcessCompletedReadbacks()
{
	if (!ReadbackManager.IsValid())
	{
		return;
	}

	for (const TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>& Slot : ReadbackManager->GetSlots())
	{
		const BoatCapture::EReadbackState CurrentState{Slot->State.load(std::memory_order_acquire)};
		if (CurrentState == BoatCapture::EReadbackState::Failed)
		{
			++FailedCaptureCount;
			Slot->CpuPixels.Reset();
			Slot->FrameIndex = -1;
			Slot->ElapsedSeconds = 0.0;
			Slot->State.store(BoatCapture::EReadbackState::Free, std::memory_order_release);
			continue;
		}

		if (CurrentState != BoatCapture::EReadbackState::CpuDataReady)
		{
			continue;
		}

		if (!SaveReadbackFrame(Slot->FrameIndex, Slot->ElapsedSeconds, Slot->CpuPixels))
		{
			++FailedCaptureCount;
		}

		// PNG 저장 큐가 픽셀 소유권을 넘겨받은 뒤 슬롯을 다음 캡처에 재사용
		Slot->CpuPixels.Reset();
		Slot->FrameIndex = -1;
		Slot->ElapsedSeconds = 0.0;
		Slot->State.store(BoatCapture::EReadbackState::Free, std::memory_order_release);
	}
}

// GPU에서 복사된 RGBA16f 픽셀을 컬러와 깊이 PNG 저장용 픽셀로 변환
bool UBoatCaptureComponent::SaveReadbackFrame(
	const int32 FrameIndex,
	const double ElapsedSeconds,
	const TArray<FFloat16Color>& SourcePixels)
{
	const int32 ExpectedPixelCount{CaptureWidth * CaptureHeight};
	if (FrameIndex < 0 || SourcePixels.Num() != ExpectedPixelCount)
	{
		return false;
	}

	TArray64<FColor> ColorPixels;
	TArray64<FColor> DepthPixels;
	ColorPixels.SetNumUninitialized(ExpectedPixelCount);
	DepthPixels.SetNumUninitialized(ExpectedPixelCount);

	for (int32 PixelIndex = 0; PixelIndex < ExpectedPixelCount; ++PixelIndex)
	{
		const FLinearColor SourcePixel{SourcePixels[PixelIndex].GetFloats()};
		ColorPixels[PixelIndex] = BoatCapture::ToneMapSceneColor(SourcePixel, ColorExposureCompensation);

		const float DepthCentimeters{SourcePixel.A};
		uint8 GrayValue{0};
		if (FMath::IsFinite(DepthCentimeters) && DepthCentimeters > 0.0f)
		{
			const float NormalizedDepth{FMath::Clamp(DepthCentimeters / DepthMaxDistance, 0.0f, 1.0f)};
			GrayValue = static_cast<uint8>(FMath::RoundToInt((1.0f - NormalizedDepth) * 255.0f));
		}

		DepthPixels[PixelIndex] = FColor(GrayValue, GrayValue, GrayValue, 255);
	}

	const FString FrameFilename{FString::Printf(TEXT("frame_%06d.png"), FrameIndex)};
	const FString ColorFilename{FPaths::Combine(ColorOutputDirectory, FrameFilename)};
	const FString DepthFilename{FPaths::Combine(DepthOutputDirectory, FrameFilename)};
	const bool bColorQueued{EnqueuePngWrite(MoveTemp(ColorPixels), ColorFilename)};
	const bool bDepthQueued{EnqueuePngWrite(MoveTemp(DepthPixels), DepthFilename)};
	if (!bColorQueued || !bDepthQueued)
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: %d번 프레임 저장 작업을 등록하지 못했습니다."), FrameIndex);
		return false;
	}

	CapturedFrameIndices.Add(FrameIndex);
	CapturedFrameTimesMilliseconds.Add(ElapsedSeconds * 1000.0);
	return true;
}

// 게임 종료 시 진행 중인 Readback을 완료해 마지막 이미지까지 저장
void UBoatCaptureComponent::FlushPendingReadbacks()
{
	if (!ReadbackManager.IsValid())
	{
		return;
	}

	// 먼저 캡처와 GPU 복사 시작 명령을 렌더 스레드까지 전달
	FlushRenderingCommands();

	// 종료 시에만 남은 Fence를 강제로 완료해 요청했던 마지막 프레임까지 회수
	QueueReadyReadbacks(true);
	FlushRenderingCommands();
	ProcessCompletedReadbacks();
}

// 렌더 스레드에서 GPU Readback 자원을 해제한 뒤 관리 객체 정리
void UBoatCaptureComponent::ReleaseReadbackResources()
{
	if (!ReadbackManager.IsValid())
	{
		return;
	}

	TArray<TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>> Slots{
		ReadbackManager->GetSlots()};

	// FRHIGPUTextureReadback 내부의 RHI Texture와 Fence도 렌더 스레드에서 파괴
	ENQUEUE_RENDER_COMMAND(FBoatCaptureReleaseReadbacks)(
		[Slots = MoveTemp(Slots)](FRHICommandListImmediate& RHICmdList)
		{
			for (const TSharedPtr<BoatCapture::FReadbackSlot, ESPMode::ThreadSafe>& Slot : Slots)
			{
				Slot->GpuReadback.Reset();
				Slot->State.store(BoatCapture::EReadbackState::Free, std::memory_order_release);
			}
		});

	FlushRenderingCommands();
	ReadbackManager.Reset();
}

// 이미지 픽셀을 비동기 PNG 저장 큐에 등록
bool UBoatCaptureComponent::EnqueuePngWrite(TArray64<FColor>&& Pixels, const FString& Filename) const
{
	if (ImageWriteQueue == nullptr || Pixels.IsEmpty())
	{
		return false;
	}

	TUniquePtr<FImageWriteTask> ImageTask{MakeUnique<FImageWriteTask>()};
	ImageTask->Filename = Filename;
	ImageTask->Format = EImageFormat::PNG;
	ImageTask->CompressionQuality = BoatCapture::PngCompressionQuality;
	ImageTask->bOverwriteFile = false;
	ImageTask->PixelData = MakeUnique<TImagePixelData<FColor>>(
		FIntPoint(CaptureWidth, CaptureHeight),
		MoveTemp(Pixels));

	TFuture<bool> WriteResult{ImageWriteQueue->Enqueue(MoveTemp(ImageTask), true)};
	return WriteResult.IsValid();
}

// 캡처 설정과 프레임별 시각을 JSON 파일로 기록
void UBoatCaptureComponent::WriteManifest() const
{
	TSharedRef<FJsonObject> RootObject{MakeShared<FJsonObject>()};
	RootObject->SetStringField(TEXT("version"), TEXT("1.1"));
	RootObject->SetNumberField(TEXT("width"), CaptureWidth);
	RootObject->SetNumberField(TEXT("height"), CaptureHeight);
	RootObject->SetNumberField(TEXT("capture_interval_ms"), CaptureIntervalSeconds * 1000.0);
	RootObject->SetStringField(TEXT("gpu_readback"), TEXT("FRHIGPUTextureReadback; asynchronous"));
	RootObject->SetNumberField(
		TEXT("readback_buffer_count"),
		FMath::Clamp(ReadbackBufferCount, BoatCapture::MinimumReadbackBufferCount, BoatCapture::MaximumReadbackBufferCount));
	RootObject->SetNumberField(TEXT("dropped_capture_count"), DroppedCaptureCount);
	RootObject->SetNumberField(TEXT("failed_capture_count"), FailedCaptureCount);
	RootObject->SetNumberField(TEXT("depth_max_cm"), DepthMaxDistance);
	RootObject->SetStringField(TEXT("capture_source"), TEXT("SceneColor HDR in RGB; SceneDepth in A"));
	RootObject->SetStringField(TEXT("color_tone_mapping"), TEXT("ACES fitted; sRGB"));
	RootObject->SetNumberField(TEXT("color_exposure_compensation_ev"), ColorExposureCompensation);
	RootObject->SetStringField(TEXT("depth_encoding"), TEXT("RGB8; near=255; far=0"));
	RootObject->SetNumberField(TEXT("frame_count"), CapturedFrameIndices.Num());

	// 여러 Readback 슬롯은 완료 순서가 달라질 수 있어 manifest는 요청 프레임 순서로 정렬
	TArray<int32> FrameOrder;
	FrameOrder.SetNumUninitialized(CapturedFrameIndices.Num());
	for (int32 ArrayIndex = 0; ArrayIndex < FrameOrder.Num(); ++ArrayIndex)
	{
		FrameOrder[ArrayIndex] = ArrayIndex;
	}
	FrameOrder.Sort(
		[this](const int32 LeftArrayIndex, const int32 RightArrayIndex)
		{
			return CapturedFrameIndices[LeftArrayIndex] < CapturedFrameIndices[RightArrayIndex];
		});

	TArray<TSharedPtr<FJsonValue>> FrameValues;
	FrameValues.Reserve(CapturedFrameIndices.Num());
	for (const int32 ArrayIndex : FrameOrder)
	{
		const int32 FrameIndex{CapturedFrameIndices[ArrayIndex]};
		const FString FrameFilename{FString::Printf(TEXT("frame_%06d.png"), FrameIndex)};

		TSharedRef<FJsonObject> FrameObject{MakeShared<FJsonObject>()};
		FrameObject->SetNumberField(TEXT("index"), FrameIndex);
		FrameObject->SetNumberField(TEXT("timestamp_ms"), CapturedFrameTimesMilliseconds[ArrayIndex]);
		FrameObject->SetStringField(TEXT("color"), FPaths::Combine(TEXT("color"), FrameFilename));
		FrameObject->SetStringField(TEXT("depth"), FPaths::Combine(TEXT("depth"), FrameFilename));
		FrameValues.Add(MakeShared<FJsonValueObject>(FrameObject));
	}
	RootObject->SetArrayField(TEXT("frames"), FrameValues);

	FString JsonText;
	const TSharedRef<TJsonWriter<>> JsonWriter{TJsonWriterFactory<>::Create(&JsonText)};
	if (!FJsonSerializer::Serialize(RootObject, JsonWriter))
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: manifest JSON 생성에 실패했습니다."));
		return;
	}

	const FString ManifestFilename{FPaths::Combine(SessionOutputDirectory, TEXT("manifest.json"))};
	if (!FFileHelper::SaveStringToFile(
		JsonText,
		*ManifestFilename,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: manifest 파일 저장에 실패했습니다. %s"), *ManifestFilename);
		return;
	}

	UE_LOG(
		LogTemp,
		Log,
		TEXT("BoatCapture: %d개 프레임 저장 완료, %d개 건너뜀, %d개 실패. %s"),
		CapturedFrameIndices.Num(),
		DroppedCaptureCount,
		FailedCaptureCount,
		*SessionOutputDirectory);
}
