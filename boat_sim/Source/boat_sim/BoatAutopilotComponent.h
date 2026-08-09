// 프로젝트 설정의 Description에서 저작권 문구를 설정할 수 있습니다.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BoatAutopilotComponent.generated.h"

class AActor;
class UBoatMovementComponent;
class UPrimitiveComponent;

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
	void FollowRoute();

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

	// 방향 오차와 도착 거리에 따라 추진 입력 계산
	float CalculateThrottle(float AbsoluteHeadingError, float DistanceToWaypoint, bool bFinalWaypoint) const;

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
	float CruiseThrottle{0.8f};

	/* 큰 각도로 선회할 때 사용할 추진 입력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurningThrottle{0.25f};

	/* 방향타 입력이 최대가 되는 방향 오차 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float FullRudderAngle{30.0f};

	/* 최종 목적지에 접근하며 감속을 시작할 거리 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "1.0"))
	float GoalSlowdownDistance{350.0f};

	/* 목적지 안에서 관성을 줄이기 위한 역추진 입력 */
	UPROPERTY(EditAnywhere, Category = "Boat|Autopilot|Control", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BrakingThrottle{0.25f};

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

	int32 CurrentWaypointIndex{0};
	bool bRouteReady{false};
	bool bArrived{false};
};
