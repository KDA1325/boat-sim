#include "BoatMovementComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"

UBoatMovementComponent::UBoatMovementComponent()
{
	// 매 프레임 물리 힘을 적용하도록 Tick 활성화
	PrimaryComponentTick.bCanEverTick = true;
}

void UBoatMovementComponent::SetThrottle(const float Value)
{
	// 추진 입력을 -1 ~ 1 범위로 제한
	ThrottleInput = FMath::Clamp(Value, -1.0f, 1.0f);
}

void UBoatMovementComponent::SetRudder(const float Value)
{
	// 방향타 입력을 -1 ~ 1 범위로 제한
	RudderInput = FMath::Clamp(Value, -1.0f, 1.0f);
}

void UBoatMovementComponent::BeginPlay()
{
	Super::BeginPlay();

	// 소유 액터의 루트 컴포넌트를 선박의 물리 몸체로 사용
	const AActor* Owner = GetOwner();

	// UPrimitiveComponent: 지오메트리 표현이 있는 씬 컴포넌트
	BoatBody = Owner != nullptr ? Cast<UPrimitiveComponent>(Owner->GetRootComponent()) : nullptr;

	// 물리 힘을 적용할 컴포넌트 없으면 Tick 중지
	if (BoatBody == nullptr)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("BoatMovementComponent on %s requires a primitive root component."),
			*GetNameSafe(Owner));
		SetComponentTickEnabled(false);
		return;
	}

	// 물리 시뮬레이션이 꺼져 있으면 이동할 수 없음 알림
	if (!BoatBody->IsSimulatingPhysics())
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("BoatMovementComponent on %s requires physics simulation to move."),
			*GetNameSafe(Owner));
	}
}

void UBoatMovementComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (BoatBody == nullptr || !BoatBody->IsSimulatingPhysics())
	{
		return;
	}

	// 매 프레임 추진력, 물의 저항력 적용
	ApplyPropulsion();
	ApplyWaterResistance();
	ApplySteering();
}

void UBoatMovementComponent::ApplyPropulsion() const
{
	// 선박의 앞쪽 방향에 추진 입력만큼 힘 적용
	const FVector PropulsionForce =
		BoatBody->GetForwardVector() * ThrottleInput * MaxPropulsionForce;

	BoatBody->AddForce(PropulsionForce);
}

void UBoatMovementComponent::ApplyWaterResistance() const
{
	// 현재 속도를 선박 기준의 전후 속도와 좌우 속도로 분리
	const FVector Velocity = BoatBody->GetPhysicsLinearVelocity();
	const FVector Forward = BoatBody->GetForwardVector();
	const FVector Right = BoatBody->GetRightVector();

	const float ForwardSpeed = FVector::DotProduct(Velocity, Forward);
	const float LateralSpeed = FVector::DotProduct(Velocity, Right);

	// 방향타를 많이 꺾고 빠르게 이동할수록 추가 항력 증가
	const float RudderDragMagnitude = FMath::Min(
		FMath::Square(ForwardSpeed) * FMath::Abs(RudderInput) * RudderDragCoefficient,
		MaxRudderDragForce);
	const FVector RudderDragForce =
		-Forward * FMath::Sign(ForwardSpeed) * RudderDragMagnitude;

	// 각 이동 방향과 방향타 움직임의 반대로 물의 저항력 적용
	const FVector ResistanceForce =
		(-Forward * ForwardSpeed * ForwardDragCoefficient)
		+ (-Right * LateralSpeed * LateralDragCoefficient)
		+ RudderDragForce;

	BoatBody->AddForce(ResistanceForce);
}

void UBoatMovementComponent::ApplySteering() const
{
	// 전진 속도가 높을수록 방향타가 강하게 작동
	const FVector Forward = BoatBody->GetForwardVector();
	const float ForwardSpeed = FVector::DotProduct(BoatBody->GetPhysicsLinearVelocity(), Forward);
	const float SteeringEffectiveness = FMath::Clamp(FMath::Abs(ForwardSpeed) / FullSteeringSpeed, 0.0f, 1.0f);

	// 후진 중에는 방향타의 선회 방향 반전
	const float MovementDirection = FMath::Sign(ForwardSpeed);
	const float SteeringTorque = RudderInput * MovementDirection * SteeringEffectiveness * MaxSteeringTorque;

	// 현재 회전의 반대 방향으로 감쇠력을 적용해 계속 도는 현상 방지
	const float CurrentYawSpeed = BoatBody->GetPhysicsAngularVelocityInRadians().Z;
	const float DampingTorque = -CurrentYawSpeed * YawDampingTorque;
	const float TotalYawTorque = SteeringTorque + DampingTorque;

	BoatBody->AddTorqueInRadians(FVector::UpVector * TotalYawTorque);
}
