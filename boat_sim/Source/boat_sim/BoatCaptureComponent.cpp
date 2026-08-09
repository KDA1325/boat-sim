// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#include "BoatCaptureComponent.h"

#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "ImagePixelData.h"
#include "ImageUtils.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace BoatCapture
{
	constexpr double MinimumCaptureIntervalSeconds{0.01};
	constexpr int32 PngCompressionQuality{0};

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

	if (!InitializeRenderTargets() || !InitializeOutputDirectories())
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: 캡처 초기화에 실패했습니다."));
		SetComponentTickEnabled(false);
		return;
	}

	CaptureStartTimeSeconds = FPlatformTime::Seconds();
	NextCaptureTimeSeconds = CaptureStartTimeSeconds;
	NextFrameIndex = 0;
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

	if (ImageWriteQueue != nullptr && !SessionOutputDirectory.IsEmpty())
	{
		// 비동기 PNG 파일이 모두 저장된 뒤 manifest 작성
		ImageWriteQueue->CreateFence().Wait();
		WriteManifest();
	}

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

	const double CurrentTimeSeconds{FPlatformTime::Seconds()};
	if (CurrentTimeSeconds < NextCaptureTimeSeconds)
	{
		return;
	}

	CaptureFrame(CurrentTimeSeconds - CaptureStartTimeSeconds);

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

// 동일한 카메라 위치에서 컬러와 깊이 한 쌍 캡처
void UBoatCaptureComponent::CaptureFrame(const double ElapsedSeconds)
{
	TArray64<FColor> ColorPixels;
	TArray64<FColor> DepthPixels;

	if (!ReadCombinedPixels(ColorPixels, DepthPixels))
	{
		UE_LOG(LogTemp, Warning, TEXT("BoatCapture: %d번 프레임을 읽지 못했습니다."), NextFrameIndex);
		return;
	}

	const int32 FrameIndex{NextFrameIndex++};
	const FString FrameFilename{FString::Printf(TEXT("frame_%06d.png"), FrameIndex)};
	const FString ColorFilename{FPaths::Combine(ColorOutputDirectory, FrameFilename)};
	const FString DepthFilename{FPaths::Combine(DepthOutputDirectory, FrameFilename)};

	const bool bColorQueued{EnqueuePngWrite(MoveTemp(ColorPixels), ColorFilename)};
	const bool bDepthQueued{EnqueuePngWrite(MoveTemp(DepthPixels), DepthFilename)};
	if (!bColorQueued || !bDepthQueued)
	{
		UE_LOG(LogTemp, Error, TEXT("BoatCapture: %d번 프레임 저장 작업을 등록하지 못했습니다."), FrameIndex);
		return;
	}

	CapturedFrameIndices.Add(FrameIndex);
	CapturedFrameTimesMilliseconds.Add(ElapsedSeconds * 1000.0);
}

// 한 번 캡처한 HDR 컬러와 깊이를 각각 PNG 저장용 픽셀로 변환
bool UBoatCaptureComponent::ReadCombinedPixels(
	TArray64<FColor>& OutColorPixels,
	TArray64<FColor>& OutDepthPixels)
{
	TextureTarget = CombinedRenderTarget;
	CaptureSource = SCS_SceneColorSceneDepth;
	CaptureScene();

	FImage CombinedImage;
	if (!FImageUtils::GetRenderTargetImage(CombinedRenderTarget, CombinedImage))
	{
		return false;
	}

	CombinedImage.ChangeFormat(ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	const TArrayView64<FLinearColor> SourcePixels{CombinedImage.AsRGBA32F()};
	OutColorPixels.SetNumUninitialized(SourcePixels.Num());
	OutDepthPixels.SetNumUninitialized(SourcePixels.Num());

	for (int64 PixelIndex = 0; PixelIndex < SourcePixels.Num(); ++PixelIndex)
	{
		const FLinearColor& SourcePixel{SourcePixels[PixelIndex]};
		OutColorPixels[PixelIndex] = BoatCapture::ToneMapSceneColor(SourcePixel, ColorExposureCompensation);

		const float DepthCentimeters{SourcePixel.A};
		uint8 GrayValue{0};

		if (FMath::IsFinite(DepthCentimeters) && DepthCentimeters > 0.0f)
		{
			const float NormalizedDepth{FMath::Clamp(DepthCentimeters / DepthMaxDistance, 0.0f, 1.0f)};
			GrayValue = static_cast<uint8>(FMath::RoundToInt((1.0f - NormalizedDepth) * 255.0f));
		}

		OutDepthPixels[PixelIndex] = FColor(GrayValue, GrayValue, GrayValue, 255);
	}

	return true;
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
	RootObject->SetStringField(TEXT("version"), TEXT("1.0"));
	RootObject->SetNumberField(TEXT("width"), CaptureWidth);
	RootObject->SetNumberField(TEXT("height"), CaptureHeight);
	RootObject->SetNumberField(TEXT("capture_interval_ms"), CaptureIntervalSeconds * 1000.0);
	RootObject->SetNumberField(TEXT("depth_max_cm"), DepthMaxDistance);
	RootObject->SetStringField(TEXT("capture_source"), TEXT("SceneColor HDR in RGB; SceneDepth in A"));
	RootObject->SetStringField(TEXT("color_tone_mapping"), TEXT("ACES fitted; sRGB"));
	RootObject->SetNumberField(TEXT("color_exposure_compensation_ev"), ColorExposureCompensation);
	RootObject->SetStringField(TEXT("depth_encoding"), TEXT("RGB8; near=255; far=0"));
	RootObject->SetNumberField(TEXT("frame_count"), CapturedFrameIndices.Num());

	TArray<TSharedPtr<FJsonValue>> FrameValues;
	FrameValues.Reserve(CapturedFrameIndices.Num());
	for (int32 ArrayIndex = 0; ArrayIndex < CapturedFrameIndices.Num(); ++ArrayIndex)
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

	UE_LOG(LogTemp, Log, TEXT("BoatCapture: %d개 프레임 저장을 완료했습니다. %s"), CapturedFrameIndices.Num(), *SessionOutputDirectory);
}
