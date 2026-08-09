#include "BoatAutopilotComponent.h"

#include "BoatMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

UBoatAutopilotComponent::UBoatAutopilotComponent()
{
	// 매 프레임 자율주행 입력을 갱신하도록 Tick 활성화
	PrimaryComponentTick.bCanEverTick = true;
}

void UBoatAutopilotComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	MovementComponent = Owner != nullptr ? Owner->FindComponentByClass<UBoatMovementComponent>() : nullptr;
	BoatBody = Owner != nullptr ? Cast<UPrimitiveComponent>(Owner->GetRootComponent()) : nullptr;

	// 자율주행에 필요한 선박 컴포넌트가 없으면 Tick 중지
	if (MovementComponent == nullptr || BoatBody == nullptr)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("BoatAutopilotComponent on %s requires boat movement and primitive root components."),
			*GetNameSafe(Owner));
		SetComponentTickEnabled(false);
		return;
	}

	// 모든 액터의 BeginPlay가 끝나 장벽 위치가 확정된 다음 경로 생성
	GetWorld()->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateUObject(this, &UBoatAutopilotComponent::InitializeRoute));
}

void UBoatAutopilotComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bRouteReady || bArrived)
	{
		return;
	}

	FollowRoute();
}

void UBoatAutopilotComponent::InitializeRoute()
{
	ObstacleWall = FindActorWithTag(ObstacleTag);
	GoalActor = FindActorWithTag(GoalTag);

	// 장벽이나 도착 지점을 찾지 못하면 자율주행 시작 중단
	if (ObstacleWall == nullptr || GoalActor == nullptr)
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("BoatAutopilotComponent requires actors tagged %s and %s."),
			*ObstacleTag.ToString(),
			*GoalTag.ToString());
		return;
	}

	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return;
	}

	const FVector StartLocation = Owner->GetActorLocation();
	const FVector WallCenter = ObstacleWall->GetActorLocation();
	FVector GoalLocation = GoalActor->GetActorLocation();

	// 장벽의 긴 로컬 Y축을 기준으로 양 끝 위치 계산
	FVector WallDirection = ObstacleWall->GetActorRightVector();
	WallDirection.Z = 0.0f;
	WallDirection.Normalize();

	const FVector EndA = WallCenter + WallDirection * WallHalfLength;
	const FVector EndB = WallCenter - WallDirection * WallHalfLength;
	const float CostA = FVector::Dist2D(StartLocation, EndA) + FVector::Dist2D(EndA, GoalLocation);
	const float CostB = FVector::Dist2D(StartLocation, EndB) + FVector::Dist2D(EndB, GoalLocation);
	const FVector ChosenEnd = CostA <= CostB ? EndA : EndB;

	// 선택한 벽 끝에서 바깥쪽으로 안전거리 확보
	FVector OutwardDirection = ChosenEnd - WallCenter;
	OutwardDirection.Z = 0.0f;
	OutwardDirection.Normalize();
	const FVector PassPoint = ChosenEnd + OutwardDirection * ObstacleClearance;

	// 시작점에서 도착점으로 향하는 방향을 기준으로 진입점과 통과점 생성
	FVector TravelDirection = GoalLocation - StartLocation;
	TravelDirection.Z = 0.0f;
	TravelDirection.Normalize();

	FVector ApproachPoint = PassPoint - TravelDirection * ApproachDistance;
	FVector ExitPoint = PassPoint + TravelDirection * ExitDistance;
	ApproachPoint.Z = StartLocation.Z;
	ExitPoint.Z = StartLocation.Z;
	GoalLocation.Z = StartLocation.Z;

	Waypoints.Reset();
	Waypoints.Add(ApproachPoint);
	Waypoints.Add(ExitPoint);
	Waypoints.Add(GoalLocation);

	RouteStartLocation = StartLocation;
	CurrentWaypointIndex = 0;
	bRouteReady = true;
	bArrived = false;

	DrawRoute();
}

AActor* UBoatAutopilotComponent::FindActorWithTag(const FName ActorTag) const
{
	TArray<AActor*> FoundActors;
	UGameplayStatics::GetAllActorsWithTag(this, ActorTag, FoundActors);

	return FoundActors.IsEmpty() ? nullptr : FoundActors[0];
}

void UBoatAutopilotComponent::FollowRoute()
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Waypoints.IsValidIndex(CurrentWaypointIndex))
	{
		FinishRoute();
		return;
	}

	const FVector CurrentLocation = Owner->GetActorLocation();
	AdvancePassedWaypoints(CurrentLocation);

	if (!Waypoints.IsValidIndex(CurrentWaypointIndex))
	{
		FinishRoute();
		return;
	}

	const FVector TargetLocation = Waypoints[CurrentWaypointIndex];
	const float DistanceToWaypoint = FVector::Dist2D(CurrentLocation, TargetLocation);
	const bool bFinalWaypoint = CurrentWaypointIndex == Waypoints.Num() - 1;

	const FVector Forward = Owner->GetActorForwardVector();
	const float ForwardSpeed = FVector::DotProduct(BoatBody->GetPhysicsLinearVelocity(), Forward);

	// 최종 목적지 안에서 충분히 감속하면 자율주행 종료
	if (bFinalWaypoint
		&& DistanceToWaypoint <= GoalAcceptanceRadius
		&& FMath::Abs(ForwardSpeed) <= ArrivalSpeedThreshold)
	{
		FinishRoute();
		return;
	}

	// 정확한 웨이포인트 대신 경로 위 앞쪽 지점을 바라보며 부드럽게 선회
	const FVector LookAheadTarget = CalculateLookAheadTarget(CurrentLocation);
	const float HeadingError = CalculateHeadingError(LookAheadTarget);
	const float RudderInput = FMath::Clamp(HeadingError / FullRudderAngle, -1.0f, 1.0f);
	const float ThrottleInput = CalculateThrottle(
		FMath::Abs(HeadingError),
		DistanceToWaypoint,
		bFinalWaypoint);

	MovementComponent->SetRudder(RudderInput);
	MovementComponent->SetThrottle(ThrottleInput);

	if (bDrawDebugRoute)
	{
		DrawDebugSphere(
			GetWorld(),
			LookAheadTarget,
			20.0f,
			12,
			FColor::Yellow,
			false,
			0.0f,
			0,
			3.0f);
	}
}

void UBoatAutopilotComponent::AdvancePassedWaypoints(const FVector& CurrentLocation)
{
	// 최종 목적지는 통과 처리하지 않고 도착 반경과 속도로만 판정
	while (Waypoints.IsValidIndex(CurrentWaypointIndex)
		&& CurrentWaypointIndex < Waypoints.Num() - 1)
	{
		const float DistanceToWaypoint = FVector::Dist2D(
			CurrentLocation,
			Waypoints[CurrentWaypointIndex]);
		const bool bReachedWaypoint = DistanceToWaypoint <= WaypointAcceptanceRadius;
		const bool bPassedWaypoint = HasPassedWaypoint(CurrentLocation, CurrentWaypointIndex);

		if (!bReachedWaypoint && !bPassedWaypoint)
		{
			break;
		}

		++CurrentWaypointIndex;
	}
}

bool UBoatAutopilotComponent::HasPassedWaypoint(
	const FVector& CurrentLocation,
	const int32 WaypointIndex) const
{
	if (!Waypoints.IsValidIndex(WaypointIndex))
	{
		return false;
	}

	const FVector SegmentStart = GetSegmentStart(WaypointIndex);
	const FVector SegmentEnd = Waypoints[WaypointIndex];
	FVector SegmentDirection = SegmentEnd - SegmentStart;
	SegmentDirection.Z = 0.0f;

	const float SegmentLength = SegmentDirection.Size2D();
	if (SegmentLength <= UE_KINDA_SMALL_NUMBER)
	{
		return true;
	}

	SegmentDirection /= SegmentLength;

	FVector FromStart = CurrentLocation - SegmentStart;
	FromStart.Z = 0.0f;
	FVector FromEnd = CurrentLocation - SegmentEnd;
	FromEnd.Z = 0.0f;

	const bool bCrossedEndPlane = FVector::DotProduct(FromEnd, SegmentDirection) >= 0.0f;
	const float CrossTrackDistance = FMath::Abs(
		FVector::CrossProduct(SegmentDirection, FromStart).Z);

	return bCrossedEndPlane && CrossTrackDistance <= WaypointPassCorridor;
}

FVector UBoatAutopilotComponent::CalculateLookAheadTarget(const FVector& CurrentLocation) const
{
	if (!Waypoints.IsValidIndex(CurrentWaypointIndex))
	{
		return CurrentLocation;
	}

	const FVector SegmentStart = GetSegmentStart(CurrentWaypointIndex);
	const FVector SegmentEnd = Waypoints[CurrentWaypointIndex];
	FVector Segment = SegmentEnd - SegmentStart;
	Segment.Z = 0.0f;

	const float SegmentLengthSquared = Segment.SizeSquared2D();
	if (SegmentLengthSquared <= UE_KINDA_SMALL_NUMBER)
	{
		return SegmentEnd;
	}

	FVector FromStart = CurrentLocation - SegmentStart;
	FromStart.Z = 0.0f;
	const float ProjectionRatio = FMath::Clamp(
		FVector::DotProduct(FromStart, Segment) / SegmentLengthSquared,
		0.0f,
		1.0f);
	FVector Cursor = SegmentStart + Segment * ProjectionRatio;

	const FVector SegmentDirection = Segment.GetSafeNormal2D();
	const float CrossTrackDistance = FMath::Abs(
		FVector::CrossProduct(SegmentDirection, FromStart).Z);
	float RemainingDistance = LookAheadDistance;
	int32 SegmentTargetIndex = CurrentWaypointIndex;

	while (Waypoints.IsValidIndex(SegmentTargetIndex))
	{
		const FVector NextPoint = Waypoints[SegmentTargetIndex];
		FVector ToNextPoint = NextPoint - Cursor;
		ToNextPoint.Z = 0.0f;
		const float DistanceToNextPoint = ToNextPoint.Size2D();

		if (RemainingDistance <= DistanceToNextPoint)
		{
			return Cursor + ToNextPoint.GetSafeNormal2D() * RemainingDistance;
		}

		// 경로에서 크게 벗어나면 안전 웨이포인트 너머를 미리 조준하지 않음
		if (SegmentTargetIndex == CurrentWaypointIndex
			&& CrossTrackDistance > WaypointPassCorridor)
		{
			return NextPoint;
		}

		RemainingDistance -= DistanceToNextPoint;
		Cursor = NextPoint;
		++SegmentTargetIndex;
	}

	return Waypoints.Last();
}

FVector UBoatAutopilotComponent::GetSegmentStart(const int32 WaypointIndex) const
{
	return WaypointIndex > 0 && Waypoints.IsValidIndex(WaypointIndex - 1)
		? Waypoints[WaypointIndex - 1]
		: RouteStartLocation;
}

float UBoatAutopilotComponent::CalculateHeadingError(const FVector& TargetLocation) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		return 0.0f;
	}

	FVector Forward = Owner->GetActorForwardVector();
	Forward.Z = 0.0f;
	Forward.Normalize();

	FVector DesiredDirection = TargetLocation - Owner->GetActorLocation();
	DesiredDirection.Z = 0.0f;
	DesiredDirection.Normalize();

	// 외적의 Z 부호로 좌우 방향을 구하고 내적으로 각도 크기 계산
	const float CrossZ = FVector::CrossProduct(Forward, DesiredDirection).Z;
	const float Dot = FVector::DotProduct(Forward, DesiredDirection);

	return FMath::RadiansToDegrees(FMath::Atan2(CrossZ, Dot));
}

float UBoatAutopilotComponent::CalculateThrottle(
	const float AbsoluteHeadingError,
	const float DistanceToWaypoint,
	const bool bFinalWaypoint) const
{
	// 방향 오차가 커질수록 추진 입력을 낮춰 선회 거리 확보
	const float HeadingRatio = FMath::Clamp(AbsoluteHeadingError / 90.0f, 0.0f, 1.0f);
	float Throttle = FMath::Lerp(CruiseThrottle, TurningThrottle, HeadingRatio);

	if (!bFinalWaypoint)
	{
		return Throttle;
	}

	const AActor* Owner = GetOwner();
	const float ForwardSpeed = Owner != nullptr
		? FVector::DotProduct(BoatBody->GetPhysicsLinearVelocity(), Owner->GetActorForwardVector())
		: 0.0f;

	// 목적지 가까이에서는 추진을 줄이고 남은 관성이 크면 약하게 역추진
	if (DistanceToWaypoint <= GoalAcceptanceRadius && ForwardSpeed > ArrivalSpeedThreshold)
	{
		return -BrakingThrottle;
	}

	if (DistanceToWaypoint < GoalSlowdownDistance)
	{
		const float DistanceRatio = FMath::Clamp(
			DistanceToWaypoint / GoalSlowdownDistance,
			0.0f,
			1.0f);
		Throttle *= DistanceRatio;
	}

	return Throttle;
}

void UBoatAutopilotComponent::FinishRoute()
{
	if (MovementComponent != nullptr)
	{
		MovementComponent->SetThrottle(0.0f);
		MovementComponent->SetRudder(0.0f);
	}

	bArrived = true;
}

void UBoatAutopilotComponent::DrawRoute() const
{
	if (!bDrawDebugRoute || GetWorld() == nullptr || GetOwner() == nullptr)
	{
		return;
	}

	FVector PreviousPoint = GetOwner()->GetActorLocation();

	for (const FVector& Waypoint : Waypoints)
	{
		DrawDebugSphere(
			GetWorld(),
			Waypoint,
			30.0f,
			12,
			FColor::Green,
			true);
		DrawDebugLine(
			GetWorld(),
			PreviousPoint,
			Waypoint,
			FColor::Green,
			true,
			-1.0f,
			0,
			5.0f);

		PreviousPoint = Waypoint;
	}
}
