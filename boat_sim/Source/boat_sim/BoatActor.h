// Fill out your copyright notice in the Description page of Project Settings.

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

UCLASS()
class BOAT_SIM_API ABoatActor : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	ABoatActor();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	// 물리적으로 움직이는 보트 메시
	UPROPERTY(VisibleAnywhere, Category = "Boat")
	TObjectPtr<UStaticMeshComponent> BoatMesh;

	// Pontoon 부력을 관리하는 컴포넌트
	UPROPERTY(VisibleAnywhere, Category = "Boat")
	TObjectPtr<UBuoyancyComponent> BuoyancyComponent;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;
};
