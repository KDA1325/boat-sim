// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.
#include "BoatActor.h"

#include "BoatMovementComponent.h"
#include "BuoyancyComponent.h"
#include "BoatAutopilotComponent.h"
#include "BoatCaptureComponent.h"
#include "Components/StaticMeshComponent.h"

// 선박에 필요한 기본 컴포넌트와 물리 설정 생성
ABoatActor::ABoatActor()
{
	// 매 프레임 Tick 함수 호출
	PrimaryActorTick.bCanEverTick = true;

	// 물리 시뮬레이션에 사용할 선박 메시 생성
	BoatMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoatMesh"));
	SetRootComponent(BoatMesh);

	// 선박 메시의 물리 시뮬레이션 활성화
	BoatMesh->SetSimulatePhysics(true);

	// Water 플러그인 부력 기능용 컴포넌트 생성
	// Pontoon 개수와 배치는 BP_BoatActor에서 관리
	BuoyancyComponent = CreateDefaultSubobject<UBuoyancyComponent>(TEXT("BuoyancyComponent"));

	// 물리 계산은 전부 MovementComponent가 처리
	MovementComponent = CreateDefaultSubobject<UBoatMovementComponent>(TEXT("MovementComponent"));

	// 경로 계산은 전부 AutopilotComponent가 처리
	AutopilotComponent = CreateDefaultSubobject<UBoatAutopilotComponent>(TEXT("AutopilotComponent"));

	// 화면 캡쳐는 전부 CaptureComponent가 처리
	CaptureComponent = CreateDefaultSubobject<UBoatCaptureComponent>(TEXT("CaptureComponent"));
	CaptureComponent->SetupAttachment(BoatMesh);
	CaptureComponent->SetRelativeLocation(FVector(200.0f, 0.0f, 180.0f));
	CaptureComponent->SetRelativeRotation(FRotator(-5.0f, 0.0f, 0.0f));
}

// 게임 시작 시 액터 초기화
void ABoatActor::BeginPlay()
{
	Super::BeginPlay();

	// CDO 생성이 끝난 실제 선박 인스턴스에 질량 적용
	BoatMesh->SetMassOverrideInKg(NAME_None, 100.0f);
}

// 매 프레임 액터 상태 갱신
void ABoatActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
