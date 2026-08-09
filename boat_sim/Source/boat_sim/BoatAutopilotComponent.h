// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BoatAutopilotComponent.generated.h"

class AActor;
class UBoatMovementComponent;
class UPrimitiveComponent;

// 최종 목적지에 접근하며 수행할 주행 단계
enum class EBoatArrivalState : uint8
{
	RouteFollowing,
	FinalCoasting,
	FinalAligning,
	FinalApproach,
	RecoveryReturn,
	Arrived
};

// 러더 감속 중 번갈아 바라볼 방향
enum class EBoatRudderBrakeSide : uint8
{
	Port,
	Starboard
};

// 장벽의 짧은 쪽으로 우회 경로를 만들고 선박 이동 컴포넌트를 제어
UCLASS(ClassGroup=(Boat), meta=(BlueprintSpawnableComponent))
class BOAT_SIM_API UBoatAutopilotComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// 컴포넌트의 기본 설정값 초기화
	UBoatAutopilotComponent();

protected:
	// 게임 시작 시 필요한 컴포넌트 확인 및 경로 생성 예약
	virtual void BeginPlay() override;

	// 매 프레임 현재 웨이포인트를 향하도록 추진력과 방향타 입력 갱신
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// 장벽 위치가 확정된 다음 우회 웨이포인트 생성
	void InitializeRoute();

	// 지정한 태그를 가진 첫 번째 액터 탐색
	AActor* FindActorWithTag(FName ActorTag) const;

	// 현재 웨이포인트를 향해 선박 이동 입력 갱신
	void FollowRoute(float DeltaTime);

	// 도달하거나 안전하게 지나친 중간 웨이포인트를 다음 목표로 전환
	void AdvancePassedWaypoints(const FVector& CurrentLocation);

	// 현재 경로 구간의 진행선과 통과 허용 범위 확인
	bool HasPassedWaypoint(const FVector& CurrentLocation, int32 WaypointIndex) const;

	// 현재 경로 위치에서 일정 거리 앞의 조향 목표 계산
	FVector CalculateLookAheadTarget(const FVector& CurrentLocation) const;

	// 웨이포인트 인덱스에 해당하는 경로 구간 시작점 반환
	FVector GetSegmentStart(int32 WaypointIndex) const;

	// 현재 선박 방향과 목표 방향 사이의 좌우 각도 계산
	float CalculateHeadingError(const FVector& TargetLocation) const;

	// 최종 경로 방향을 유지하도록 목적지 앞쪽의 조향 목표 계산
	FVector CalculateFinalHeadingTarget() const;

	// 현재 도착 단계와 감속 상태에 맞는 조향 목표 계산
	FVector CalculateSteeringTarget(
		const FVector& CurrentLocation,
		float DistanceToGoal,
		float ForwardSpeed,
		float DesiredSpeed);

	// 최종 직선에서 완만한 좌우 선회로 항력을 만드는 목표 계산
	FVector CalculateRudderBrakeTarget(const FVector& CurrentLocation, float BrakeStrength);

	// 최종 경로 중심선 앞쪽의 복귀 목표 계산
	FVector CalculateFinalPathTarget(const FVector& CurrentLocation) const;

	// 마지막 중간 웨이포인트에서 목적지로 향하는 방향 반환
	FVector GetFinalRouteDirection() const;

	// 최종 경로 중심선에서 떨어진 좌우 거리 계산
	float CalculateFinalCrossTrackDistance(const FVector& CurrentLocation) const;

	// 선박이 최종 목적지 진행선을 통과했는지 확인
	bool HasPassedGoalPlane(const FVector& CurrentLocation) const;

	// 목적지를 지나쳤을 때 다시 돌아갈 안전 지점 반환
	FVector GetRecoveryTarget() const;

	// 목적지 거리와 현재 움직임에 따라 도착 단계 갱신
	void UpdateArrivalState(
		const FVector& CurrentLocation,
		float DistanceToGoal,
		float ForwardSpeed,
		float AbsoluteHeadingError);

	// 현재 웨이포인트와 남은 거리에 맞는 기본 목표 속도 계산
	float CalculateBaseDesiredSpeed(float DistanceToWaypoint, bool bFinalWaypoint) const;

	// 중간 웨이포인트에서 다음 경로 각도에 맞는 통과 속도 계산
	float CalculateWaypointTargetSpeed(int32 WaypointIndex) const;

	// 최종 목적지까지 남은 거리에 맞는 목표 속도 계산
	float CalculateFinalDesiredSpeed(float DistanceToGoal) const;

	// 방향 오차를 반영해 최종 목표 속도 제한
	float CalculateDesiredSpeed(float BaseDesiredSpeed, float AbsoluteHeadingError) const;

	// 목표 속도와 현재 속도의 차이로 전진 추진 입력 계산
	float CalculateTargetThrottle(float DesiredSpeed, float ForwardSpeed, float MaxThrottle) const;

	// 추진과 방향타 입력이 갑자기 변하지 않도록 매 프레임 보간
	void UpdateSmoothedInputs(float TargetThrottle, float TargetRudder, float DeltaTime);

	// 모든 이동 입력을 해제하고 자율주행 종료
	void FinishRoute();

	// 생성된 우회 경로를 화면에 표시
	void DrawRoute() const;

	/* 랜덤 이동 장벽에 설정한 Actor Tag */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot")
	FName ObstacleTag{TEXT("ObstacleWall")};

	/* 도착 지점에 설정한 Actor Tag */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot")
	FName GoalTag{TEXT("EndPoint")};

	/* 벽 중심부터 한쪽 끝까지의 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Route", meta = (ClampMin = "0.0"))
	float WallHalfLength{500.0f};

	/* 벽 끝에서 바깥쪽으로 확보할 안전거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Route", meta = (ClampMin = "0.0"))
	float ObstacleClearance{200.0f};

	/* 벽 앞쪽에서 우회를 시작할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Route", meta = (ClampMin = "0.0"))
	float ApproachDistance{300.0f};

	/* 벽을 지난 뒤 직진할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Route", meta = (ClampMin = "0.0"))
	float ExitDistance{200.0f};

	/* 중간 웨이포인트를 통과했다고 판단할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float WaypointAcceptanceRadius{125.0f};

	/* 경로 진행선 통과를 인정할 최대 횡방향 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float WaypointPassCorridor{250.0f};

	/* 현재 경로 위치보다 앞쪽에서 조향 목표를 잡을 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float LookAheadDistance{250.0f};

	/* 최종 목적지에 도착했다고 판단할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float GoalAcceptanceRadius{100.0f};

	/* 도착으로 인정할 최대 선박 속도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float ArrivalSpeedThreshold{25.0f};

	/* 직진할 때 사용할 최대 추진 입력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CruiseThrottle{0.5f};

	/* 일반 경로를 따라갈 때 유지할 최대 전진 속도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float CruiseSpeed{220.0f};

	/* 큰 각도의 중간 웨이포인트를 통과할 최소 속도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float MinimumWaypointSpeed{120.0f};

	/* 중간 웨이포인트에 접근하며 감속을 시작할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float WaypointSlowdownDistance{500.0f};

	/* 방향타 입력이 최대가 되는 방향 오차 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float FullRudderAngle{30.0f};

	/* 최종 목적지에 접근하며 감속을 시작할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float GoalSlowdownDistance{900.0f};

	/* 최종 저속 진입을 위해 한 번 더 감속을 시작할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float FinalStopSlowdownDistance{300.0f};

	/* 최종 목적지에 접근할 때 유지할 최대 전진 속도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float FinalApproachSpeed{60.0f};

	/* 저속으로 방향을 맞추고 최종 진입할 때 사용할 추진 입력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FinalApproachThrottle{0.15f};

	/* 최종 진입을 시작할 수 있는 방향 오차 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float FinalAlignmentTolerance{8.0f};

	/* 목적지 너머에서 최종 경로 방향을 맞추기 위한 조향 목표 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float FinalHeadingTargetDistance{200.0f};

	/* 목표 속도와 현재 속도의 차이를 추진 입력으로 바꿀 비율 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float SpeedControlGain{0.004f};

	/* 한 초 동안 변경할 수 있는 최대 추진 입력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float ThrottleChangeRate{0.5f};

	/* 한 초 동안 변경할 수 있는 최대 방향타 입력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float RudderChangeRate{0.8f};

	/* 목적지를 지나친 뒤 안전 경로로 돌아갈 때 유지할 속도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0"))
	float RecoveryTurnSpeed{100.0f};

	/* 러더 감속 중 최종 경로에서 좌우로 기울일 최대 각도 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|RudderBrake", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float RudderBrakeAngle{18.0f};

	/* 러더 감속을 시작할 최소 목표 속도 초과량 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|RudderBrake", meta = (ClampMin = "0.0"))
	float RudderBrakeSpeedTolerance{30.0f};

	/* 반대쪽 러더 감속 목표로 전환할 방향 오차 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|RudderBrake", meta = (ClampMin = "0.0"))
	float RudderBrakeHeadingTolerance{3.0f};

	/* 러더 감속 중 최종 경로에서 벗어날 수 있는 최대 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|RudderBrake", meta = (ClampMin = "1.0"))
	float RudderBrakeCorridor{150.0f};

	/* 생성된 웨이포인트와 이동 경로 표시 여부 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Debug")
	bool bDrawDebugRoute{true};

	// 추진력과 방향타 입력을 전달할 선박 이동 컴포넌트
	UPROPERTY(Transient)
	TObjectPtr<UBoatMovementComponent> MovementComponent;

	// 현재 속도를 확인할 선박의 물리 컴포넌트
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> BoatBody;

	// 현재 실행에서 랜덤 이동이 끝난 장벽 액터
	UPROPERTY(Transient)
	TObjectPtr<AActor> ObstacleWall;

	// 선박이 최종적으로 도착할 목표 액터
	UPROPERTY(Transient)
	TObjectPtr<AActor> GoalActor;

	// 장벽을 우회해 목적지까지 이어지는 경로
	TArray<FVector> Waypoints;

	// 첫 번째 경로 구간이 시작되는 선박의 출발 위치
	FVector RouteStartLocation{FVector::ZeroVector};

	EBoatArrivalState ArrivalState{EBoatArrivalState::RouteFollowing};
	EBoatRudderBrakeSide RudderBrakeSide{EBoatRudderBrakeSide::Port};
	float CurrentThrottleInput{0.0f};
	float CurrentRudderInput{0.0f};
	int32 CurrentWaypointIndex{0};
	bool bRudderBrakeActive{false};
	bool bRudderBrakeCentering{false};
	bool bRouteReady{false};
	bool bArrived{false};
};
