// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BoatActor.generated.h"

class UStaticMeshComponent;

/*
 * Water 플러그인 컴포넌트
 * 배 아래에 여러 개의 가상 구체인 Pontoon을 두고 각 지점의 수면 높이를 조회해
 * 잠긴 정도만큼 위쪽 힘을 적용하는 간이 부력 모델입니다.
 * 수면에 뜨는 힘, 파도에 따른 상하 움직임, Pontoon 차이로 발생하는 롤/피치,
 * 수직 흔들림 감쇠와 Water Body 진입/이탈 감지를 처리합니다.
 * 부유는 Water 플러그인을 사용하고 추진/감속/관성/선회는 직접 구현합니다.
 */
class UBuoyancyComponent;
class UBoatMovementComponent;

// 선박 메시와 부력, 이동 컴포넌트를 하나로 묶는 액터
UCLASS()
class BOAT_SIM_API ABoatActor : public AActor
{
	GENERATED_BODY()

public:
	// 액터의 기본 컴포넌트와 설정값 초기화
	ABoatActor();

protected:
	// 게임이 시작되거나 액터가 생성될 때 호출
	virtual void BeginPlay() override;

	// 물리적으로 움직이는 보트 메시
	UPROPERTY(VisibleAnywhere, Category = "Boat")
	TObjectPtr<UStaticMeshComponent> BoatMesh;

	// Pontoon 부력을 관리하는 컴포넌트
	UPROPERTY(VisibleAnywhere, Category = "Boat")
	TObjectPtr<UBuoyancyComponent> BuoyancyComponent;

	// 추진력, 저항력, 관성, 선회력 계산 컴포넌트
	UPROPERTY(VisibleAnywhere, Category = "Boat")
	TObjectPtr<UBoatMovementComponent> MovementComponent;

public:
	// 매 프레임 호출
	virtual void Tick(float DeltaTime) override;
};
