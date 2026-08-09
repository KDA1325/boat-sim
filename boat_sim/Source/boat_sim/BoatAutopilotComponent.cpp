#include "BoatAutopilotComponent.h"

#include "BoatMovementComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

namespace
{
	constexpr float CornerReferenceAngle{90.0f};
	constexpr float HeadingSpeedReductionAngle{90.0f};
	constexpr float RudderBrakeReleaseRatio{0.5f};
	constexpr float RudderBrakeCenteringReleaseRatio{0.5f};

	float SmoothRatio(const float Value)
	{
		const float ClampedValue = FMath::Clamp(Value, 0.0f, 1.0f);
		return ClampedValue * ClampedValue * (3.0f - 2.0f * ClampedValue);
	}
}

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

	FollowRoute(DeltaTime);
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
	ArrivalState = EBoatArrivalState::RouteFollowing;
	RudderBrakeSide = EBoatRudderBrakeSide::Port;
	CurrentThrottleInput = 0.0f;
	CurrentRudderInput = 0.0f;
	bRudderBrakeActive = false;
	bRudderBrakeCentering = false;
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

void UBoatAutopilotComponent::FollowRoute(const float DeltaTime)
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
	const FVector Velocity = BoatBody->GetPhysicsLinearVelocity();
	const float ForwardSpeed = FVector::DotProduct(Velocity, Forward);
	const float PlanarSpeed = Velocity.Size2D();

	if (bFinalWaypoint)
	{
		const float FinalHeadingError = FMath::Abs(
			CalculateHeadingError(CalculateFinalHeadingTarget()));

		// 목적지 안에서 속도와 방향이 모두 안정되면 자율주행 종료
		if (DistanceToWaypoint <= GoalAcceptanceRadius
			&& PlanarSpeed <= ArrivalSpeedThreshold
			&& FinalHeadingError <= FinalAlignmentTolerance)
		{
			FinishRoute();
			return;
		}

		UpdateArrivalState(
			CurrentLocation,
			DistanceToWaypoint,
			ForwardSpeed,
			FinalHeadingError);
	}

	const float BaseDesiredSpeed = CalculateBaseDesiredSpeed(
		DistanceToWaypoint,
		bFinalWaypoint);

	// 정확한 웨이포인트 대신 경로와 감속 상태에 맞는 앞쪽 지점을 바라봄
	const FVector SteeringTarget = CalculateSteeringTarget(
		CurrentLocation,
		DistanceToWaypoint,
		ForwardSpeed,
		BaseDesiredSpeed);
	const float HeadingError = CalculateHeadingError(SteeringTarget);
	const float AbsoluteHeadingError = FMath::Abs(HeadingError);
	const float DesiredSpeed = CalculateDesiredSpeed(BaseDesiredSpeed, AbsoluteHeadingError);

	const float TargetRudderInput = FMath::Clamp(
		HeadingError / FullRudderAngle,
		-1.0f,
		1.0f);
	const bool bLowSpeedArrival = ArrivalState == EBoatArrivalState::FinalAligning
		|| ArrivalState == EBoatArrivalState::FinalApproach;
	const float MaxThrottle = bLowSpeedArrival ? FinalApproachThrottle : CruiseThrottle;
	const float TargetThrottleInput = CalculateTargetThrottle(
		DesiredSpeed,
		ForwardSpeed,
		MaxThrottle);

	UpdateSmoothedInputs(TargetThrottleInput, TargetRudderInput, DeltaTime);

	if (bDrawDebugRoute)
	{
		DrawDebugSphere(
			GetWorld(),
			SteeringTarget,
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

FVector UBoatAutopilotComponent::CalculateFinalHeadingTarget() const
{
	if (Waypoints.IsEmpty())
	{
		return GetOwner() != nullptr ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
	}

	const FVector GoalLocation = Waypoints.Last();
	if (Waypoints.Num() < 2)
	{
		return GoalLocation;
	}

	FVector FinalDirection = GoalLocation - Waypoints[Waypoints.Num() - 2];
	FinalDirection.Z = 0.0f;
	if (!FinalDirection.Normalize())
	{
		return GoalLocation;
	}

	// 목적지를 지나도 마지막 경로 방향이 뒤집히지 않도록 앞쪽 지점을 바라봄
	return GoalLocation + FinalDirection * FinalHeadingTargetDistance;
}

FVector UBoatAutopilotComponent::CalculateSteeringTarget(
	const FVector& CurrentLocation,
	const float DistanceToGoal,
	const float ForwardSpeed,
	const float DesiredSpeed)
{
	if (ArrivalState == EBoatArrivalState::RecoveryReturn)
	{
		bRudderBrakeActive = false;
		bRudderBrakeCentering = false;
		return GetRecoveryTarget();
	}

	const bool bFinalWaypoint = CurrentWaypointIndex == Waypoints.Num() - 1;
	if (!bFinalWaypoint || ArrivalState == EBoatArrivalState::RouteFollowing)
	{
		bRudderBrakeActive = false;
		bRudderBrakeCentering = false;
		return CalculateLookAheadTarget(CurrentLocation);
	}

	if (ArrivalState != EBoatArrivalState::FinalCoasting
		|| DistanceToGoal <= FinalStopSlowdownDistance)
	{
		bRudderBrakeActive = false;
		bRudderBrakeCentering = false;
		return CalculateFinalHeadingTarget();
	}

	const float SpeedExcess = ForwardSpeed - DesiredSpeed;
	const float RudderBrakeReleaseSpeed =
		RudderBrakeSpeedTolerance * RudderBrakeReleaseRatio;

	if (bRudderBrakeActive)
	{
		bRudderBrakeActive = SpeedExcess > RudderBrakeReleaseSpeed;
	}
	else
	{
		bRudderBrakeActive = SpeedExcess > RudderBrakeSpeedTolerance;
	}

	if (!bRudderBrakeActive)
	{
		bRudderBrakeCentering = false;
		return CalculateFinalHeadingTarget();
	}

	const float AbsoluteCrossTrackDistance = FMath::Abs(
		CalculateFinalCrossTrackDistance(CurrentLocation));
	const float CenteringReleaseDistance =
		RudderBrakeCorridor * RudderBrakeCenteringReleaseRatio;

	if (bRudderBrakeCentering)
	{
		bRudderBrakeCentering = AbsoluteCrossTrackDistance > CenteringReleaseDistance;
	}
	else
	{
		bRudderBrakeCentering = AbsoluteCrossTrackDistance >= RudderBrakeCorridor;
	}

	if (bRudderBrakeCentering)
	{
		return CalculateFinalPathTarget(CurrentLocation);
	}

	const float SpeedFadeRange = FMath::Max(
		RudderBrakeSpeedTolerance - RudderBrakeReleaseSpeed,
		1.0f);
	const float SpeedStrength = FMath::Clamp(
		(SpeedExcess - RudderBrakeReleaseSpeed) / SpeedFadeRange,
		0.0f,
		1.0f);
	const float DistanceStrength = FMath::Clamp(
		(DistanceToGoal - FinalStopSlowdownDistance) / FMath::Max(LookAheadDistance, 1.0f),
		0.0f,
		1.0f);
	const float BrakeStrength = FMath::Min(SpeedStrength, DistanceStrength);

	return CalculateRudderBrakeTarget(CurrentLocation, BrakeStrength);
}

FVector UBoatAutopilotComponent::CalculateRudderBrakeTarget(
	const FVector& CurrentLocation,
	const float BrakeStrength)
{
	const float BrakeAngle = RudderBrakeAngle * FMath::Clamp(BrakeStrength, 0.0f, 1.0f);
	if (BrakeAngle <= RudderBrakeHeadingTolerance)
	{
		return CalculateFinalPathTarget(CurrentLocation);
	}

	const FVector FinalDirection = GetFinalRouteDirection();
	const auto MakeTarget = [this, &CurrentLocation, &FinalDirection, BrakeAngle](
		const EBoatRudderBrakeSide Side)
	{
		const float DirectionSign = Side == EBoatRudderBrakeSide::Starboard ? 1.0f : -1.0f;
		const FVector BrakeDirection = FinalDirection.RotateAngleAxis(
			BrakeAngle * DirectionSign,
			FVector::UpVector);
		return CurrentLocation + BrakeDirection * LookAheadDistance;
	};

	FVector BrakeTarget = MakeTarget(RudderBrakeSide);
	if (FMath::Abs(CalculateHeadingError(BrakeTarget)) <= RudderBrakeHeadingTolerance)
	{
		RudderBrakeSide = RudderBrakeSide == EBoatRudderBrakeSide::Port
			? EBoatRudderBrakeSide::Starboard
			: EBoatRudderBrakeSide::Port;
		BrakeTarget = MakeTarget(RudderBrakeSide);
	}

	return BrakeTarget;
}

FVector UBoatAutopilotComponent::CalculateFinalPathTarget(const FVector& CurrentLocation) const
{
	if (Waypoints.Num() < 2)
	{
		return CalculateFinalHeadingTarget();
	}

	const FVector SegmentStart = Waypoints[Waypoints.Num() - 2];
	const FVector FinalDirection = GetFinalRouteDirection();
	FVector FromStart = CurrentLocation - SegmentStart;
	FromStart.Z = 0.0f;
	const FVector ProjectedLocation =
		SegmentStart + FinalDirection * FVector::DotProduct(FromStart, FinalDirection);

	return ProjectedLocation + FinalDirection * LookAheadDistance;
}

FVector UBoatAutopilotComponent::GetFinalRouteDirection() const
{
	if (Waypoints.Num() >= 2)
	{
		FVector FinalDirection = Waypoints.Last() - Waypoints[Waypoints.Num() - 2];
		FinalDirection.Z = 0.0f;
		if (FinalDirection.Normalize())
		{
			return FinalDirection;
		}
	}

	const AActor* Owner = GetOwner();
	return Owner != nullptr
		? Owner->GetActorForwardVector().GetSafeNormal2D()
		: FVector::ForwardVector;
}

float UBoatAutopilotComponent::CalculateFinalCrossTrackDistance(
	const FVector& CurrentLocation) const
{
	if (Waypoints.Num() < 2)
	{
		return 0.0f;
	}

	FVector FromStart = CurrentLocation - Waypoints[Waypoints.Num() - 2];
	FromStart.Z = 0.0f;
	return FVector::CrossProduct(GetFinalRouteDirection(), FromStart).Z;
}

bool UBoatAutopilotComponent::HasPassedGoalPlane(const FVector& CurrentLocation) const
{
	if (Waypoints.IsEmpty())
	{
		return false;
	}

	FVector FromGoal = CurrentLocation - Waypoints.Last();
	FromGoal.Z = 0.0f;
	return FVector::DotProduct(FromGoal, GetFinalRouteDirection()) > GoalAcceptanceRadius;
}

FVector UBoatAutopilotComponent::GetRecoveryTarget() const
{
	return Waypoints.Num() >= 2
		? Waypoints[Waypoints.Num() - 2]
		: RouteStartLocation;
}

void UBoatAutopilotComponent::UpdateArrivalState(
	const FVector& CurrentLocation,
	const float DistanceToGoal,
	const float ForwardSpeed,
	const float AbsoluteHeadingError)
{
	if (ArrivalState == EBoatArrivalState::Arrived)
	{
		return;
	}

	// 목적지를 지나치면 후진하지 않고 마지막 안전 지점으로 복귀
	if (ArrivalState != EBoatArrivalState::RecoveryReturn
		&& HasPassedGoalPlane(CurrentLocation))
	{
		ArrivalState = EBoatArrivalState::RecoveryReturn;
		bRudderBrakeActive = false;
		bRudderBrakeCentering = false;
		return;
	}

	if (ArrivalState == EBoatArrivalState::RecoveryReturn)
	{
		if (FVector::Dist2D(CurrentLocation, GetRecoveryTarget()) <= WaypointAcceptanceRadius)
		{
			ArrivalState = EBoatArrivalState::FinalCoasting;
			RudderBrakeSide = EBoatRudderBrakeSide::Port;
		}
		return;
	}

	switch (ArrivalState)
	{
	case EBoatArrivalState::RouteFollowing:
		if (DistanceToGoal <= GoalSlowdownDistance)
		{
			ArrivalState = EBoatArrivalState::FinalCoasting;
		}
		break;

	case EBoatArrivalState::FinalCoasting:
		if (DistanceToGoal <= FinalStopSlowdownDistance
			&& ForwardSpeed <= FinalApproachSpeed)
		{
			ArrivalState = EBoatArrivalState::FinalAligning;
			bRudderBrakeActive = false;
			bRudderBrakeCentering = false;
		}
		break;

	case EBoatArrivalState::FinalAligning:
		if (ForwardSpeed > FinalApproachSpeed + RudderBrakeSpeedTolerance)
		{
			ArrivalState = EBoatArrivalState::FinalCoasting;
		}
		else if (AbsoluteHeadingError <= FinalAlignmentTolerance)
		{
			ArrivalState = EBoatArrivalState::FinalApproach;
		}
		break;

	case EBoatArrivalState::FinalApproach:
		// 진입 단계가 자주 바뀌지 않도록 허용 각도의 두 배에서 다시 정렬
		if (ForwardSpeed > FinalApproachSpeed + RudderBrakeSpeedTolerance)
		{
			ArrivalState = EBoatArrivalState::FinalCoasting;
		}
		else if (AbsoluteHeadingError > FinalAlignmentTolerance * 2.0f)
		{
			ArrivalState = EBoatArrivalState::FinalAligning;
		}
		break;

	case EBoatArrivalState::RecoveryReturn:
	case EBoatArrivalState::Arrived:
		break;
	}
}

float UBoatAutopilotComponent::CalculateBaseDesiredSpeed(
	const float DistanceToWaypoint,
	const bool bFinalWaypoint) const
{
	if (ArrivalState == EBoatArrivalState::RecoveryReturn)
	{
		return RecoveryTurnSpeed;
	}

	if (bFinalWaypoint)
	{
		float DesiredSpeed = CalculateFinalDesiredSpeed(DistanceToWaypoint);
		if (ArrivalState == EBoatArrivalState::FinalAligning)
		{
			// 방향타가 작동할 수 있도록 정렬 중에는 최소 저속 유지
			DesiredSpeed = FMath::Max(DesiredSpeed, FinalApproachSpeed * 0.5f);
		}
		return DesiredSpeed;
	}

	const float TargetWaypointSpeed = CalculateWaypointTargetSpeed(CurrentWaypointIndex);
	const float SlowdownRange = FMath::Max(
		WaypointSlowdownDistance - WaypointAcceptanceRadius,
		1.0f);
	const float DistanceRatio =
		(DistanceToWaypoint - WaypointAcceptanceRadius) / SlowdownRange;

	return FMath::Lerp(
		TargetWaypointSpeed,
		CruiseSpeed,
		SmoothRatio(DistanceRatio));
}

float UBoatAutopilotComponent::CalculateWaypointTargetSpeed(const int32 WaypointIndex) const
{
	if (!Waypoints.IsValidIndex(WaypointIndex)
		|| !Waypoints.IsValidIndex(WaypointIndex + 1))
	{
		return CruiseSpeed;
	}

	FVector IncomingDirection = Waypoints[WaypointIndex] - GetSegmentStart(WaypointIndex);
	FVector OutgoingDirection = Waypoints[WaypointIndex + 1] - Waypoints[WaypointIndex];
	IncomingDirection.Z = 0.0f;
	OutgoingDirection.Z = 0.0f;

	if (!IncomingDirection.Normalize() || !OutgoingDirection.Normalize())
	{
		return CruiseSpeed;
	}

	const float DirectionDot = FMath::Clamp(
		FVector::DotProduct(IncomingDirection, OutgoingDirection),
		-1.0f,
		1.0f);
	const float CornerAngle = FMath::RadiansToDegrees(FMath::Acos(DirectionDot));
	const float CornerRatio = FMath::Clamp(
		CornerAngle / CornerReferenceAngle,
		0.0f,
		1.0f);

	return FMath::Lerp(CruiseSpeed, MinimumWaypointSpeed, CornerRatio);
}

float UBoatAutopilotComponent::CalculateFinalDesiredSpeed(const float DistanceToGoal) const
{
	if (DistanceToGoal >= GoalSlowdownDistance)
	{
		return CruiseSpeed;
	}

	if (DistanceToGoal > FinalStopSlowdownDistance)
	{
		const float SlowdownRange = FMath::Max(
			GoalSlowdownDistance - FinalStopSlowdownDistance,
			1.0f);
		const float DistanceRatio =
			(DistanceToGoal - FinalStopSlowdownDistance) / SlowdownRange;
		return FMath::Lerp(
			FinalApproachSpeed,
			CruiseSpeed,
			SmoothRatio(DistanceRatio));
	}

	const float StopRange = FMath::Max(
		FinalStopSlowdownDistance - GoalAcceptanceRadius,
		1.0f);
	const float StopRatio =
		(DistanceToGoal - GoalAcceptanceRadius) / StopRange;
	return FinalApproachSpeed * SmoothRatio(StopRatio);
}

float UBoatAutopilotComponent::CalculateDesiredSpeed(
	const float BaseDesiredSpeed,
	const float AbsoluteHeadingError) const
{
	const float HeadingRatio = FMath::Clamp(
		AbsoluteHeadingError / HeadingSpeedReductionAngle,
		0.0f,
		1.0f);
	const float HeadingLimitedSpeed = FMath::Lerp(
		CruiseSpeed,
		MinimumWaypointSpeed,
		HeadingRatio);

	return FMath::Max(0.0f, FMath::Min(BaseDesiredSpeed, HeadingLimitedSpeed));
}

float UBoatAutopilotComponent::CalculateTargetThrottle(
	const float DesiredSpeed,
	const float ForwardSpeed,
	const float MaxThrottle) const
{
	const float SafeCruiseSpeed = FMath::Max(CruiseSpeed, 1.0f);
	const float FeedForwardThrottle =
		CruiseThrottle * FMath::Clamp(DesiredSpeed / SafeCruiseSpeed, 0.0f, 1.0f);
	const float SpeedCorrection = (DesiredSpeed - ForwardSpeed) * SpeedControlGain;

	// 자율주행은 역추진 없이 추진력 해제와 물의 저항으로만 감속
	return FMath::Clamp(
		FeedForwardThrottle + SpeedCorrection,
		0.0f,
		FMath::Clamp(MaxThrottle, 0.0f, 1.0f));
}

void UBoatAutopilotComponent::UpdateSmoothedInputs(
	const float TargetThrottle,
	const float TargetRudder,
	const float DeltaTime)
{
	if (MovementComponent == nullptr)
	{
		return;
	}

	CurrentThrottleInput = FMath::FInterpConstantTo(
		CurrentThrottleInput,
		FMath::Clamp(TargetThrottle, 0.0f, 1.0f),
		DeltaTime,
		ThrottleChangeRate);
	CurrentRudderInput = FMath::FInterpConstantTo(
		CurrentRudderInput,
		FMath::Clamp(TargetRudder, -1.0f, 1.0f),
		DeltaTime,
		RudderChangeRate);

	MovementComponent->SetThrottle(CurrentThrottleInput);
	MovementComponent->SetRudder(CurrentRudderInput);
}

void UBoatAutopilotComponent::FinishRoute()
{
	if (MovementComponent != nullptr)
	{
		MovementComponent->SetThrottle(0.0f);
		MovementComponent->SetRudder(0.0f);
	}

	CurrentThrottleInput = 0.0f;
	CurrentRudderInput = 0.0f;
	bRudderBrakeActive = false;
	bRudderBrakeCentering = false;
	ArrivalState = EBoatArrivalState::Arrived;
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
