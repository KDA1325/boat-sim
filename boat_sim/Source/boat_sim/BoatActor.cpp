// Fill out your copyright notice in the Description page of Project Settings.


#include "BoatActor.h"

// Sets default values
ABoatActor::ABoatActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

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

