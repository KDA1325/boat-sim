// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BoatMovementComponent.generated.h"

class UPrimitiveComponent;

// 추진력, 물의 저항력, 선회력을 물리 힘으로 적용하는 컴포넌트
UCLASS(ClassGroup=(Boat), meta=(BlueprintSpawnableComponent))
class BOAT_SIM_API UBoatMovementComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// 컴포넌트의 기본 설정값 초기화
	UBoatMovementComponent();

	/* 전진과 후진에 사용할 추진 입력 설정 */
	UFUNCTION(BlueprintCallable, Category = "Boat|Movement")
	void SetThrottle(float Value); // -1 ~ 1

	/* 좌우 선회에 사용할 방향타 입력 설정 */
	UFUNCTION(BlueprintCallable, Category = "Boat|Movement")
	void SetRudder(float Value); // -1 ~ 1

protected:
	// 게임 시작 시 선박의 물리 컴포넌트 확인
	virtual void BeginPlay() override;

	// 매 프레임 선박에 이동 관련 힘 적용
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/*
	* 엔진 추진력 적용
	* ThrottleInput > 0: 전진
	* ThrottleInput < 0: 후진
	* ThrottleInput == 0: 추진력 없음
	*/
	void ApplyPropulsion() const;

	/*
	* 현재 이동 방향 반대로 저항력 적용
	* 추진력이 없을 때 서서히 멈추게 하는 역할
	*/
	void ApplyWaterResistance() const;

	/*
	* 선박 수직축(Z축)에 회전력 가하는 역할
	*/
	void ApplySteering() const;

	/* 최대 추진 입력일 때 선박 앞쪽으로 적용할 힘 */
	UPROPERTY(EditAnywhere, Category = "Boat|Movement|Propulsion", meta = (ClampMin = "0.0"))
	float MaxPropulsionForce{3500.0f};

	/* 선박의 전후 이동에 적용할 물의 저항 계수 */
	UPROPERTY(EditAnywhere, Category = "Boat|Movement|Resistance", meta = (ClampMin = "0.0"))
	float ForwardDragCoefficient{8.0f};

	/* 선박이 옆으로 미끄러지는 움직임을 줄이는 저항 계수 */
	UPROPERTY(EditAnywhere, Category = "Boat|Movement|Resistance", meta = (ClampMin = "0.0"))
	float LateralDragCoefficient{60.0f};

	/* 방향타 입력으로 적용할 최대 선회력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Movement|Steering", meta = (ClampMin = "0.0"))
	float MaxSteeringTorque{180000.0f};

	/* 방향타가 최대 효과를 내기 시작하는 전진 속도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Movement|Steering", meta = (ClampMin = "1.0"))
	float FullSteeringSpeed{300.0f};

	/* 방향타를 놓았을 때 남아 있는 회전을 줄이는 감쇠력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Movement|Steering", meta = (ClampMin = "0.0"))
	float YawDampingTorque{150000.0f};

	// 자율주행 단계에서 오토파일럿이 계산한 값을 전달받음
	float ThrottleInput{0.0f};
	float RudderInput{0.0f};

	// 실제 힘과 회전력을 적용할 선박의 물리 컴포넌트
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> BoatBody;
};
