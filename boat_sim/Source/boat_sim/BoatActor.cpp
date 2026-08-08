// Fill out your copyright notice in the Description page of Project Settings.
#include "BoatActor.h"

#include "Components/StaticMeshComponent.h"
#include "BuoyancyComponent.h"

// Sets default values
ABoatActor::ABoatActor()
{
	// Set this actor to call Tick() every frame. You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	BoatMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoatMesh"));
	SetRootComponent(BoatMesh);

	BoatMesh->SetSimulatePhysics(true);
	BoatMesh->SetMassOverrideInKg(NAME_None, 100.0f);

	// Water 플러그인 부력 기능용 컴포넌트 생성
	// Pontoon 개수와 배치는 BP_BoatActor에서 관리
	BuoyancyComponent = CreateDefaultSubobject<UBuoyancyComponent>(TEXT("BuoyancyComponent"));
}

// Called when the game starts or when spawned
void ABoatActor::BeginPlay()
{
	Super::BeginPlay();
}

// Called every frame
void ABoatActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}
