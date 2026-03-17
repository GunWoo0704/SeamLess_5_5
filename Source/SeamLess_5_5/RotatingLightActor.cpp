#include "RotatingLightActor.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DirectionalLight.h"

ARotatingLightActor::ARotatingLightActor()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ARotatingLightActor::BeginPlay()
{
    Super::BeginPlay();
    ElapsedTime = 0.0f;
}

void ARotatingLightActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // 20초에 360도 → 초당 회전 각도 계산
    float DegreesPerSecond = 360.0f / RotationDuration;
    float DeltaAngle = DegreesPerSecond * DeltaTime;

    TArray<AActor*> DirectionalLights;
    UGameplayStatics::GetAllActorsOfClass(
        GetWorld(),
        ADirectionalLight::StaticClass(),
        DirectionalLights);

    for (AActor* Light : DirectionalLights)
    {
        // 절대값 세팅 대신 매 프레임 조금씩 증분 회전
        Light->AddActorLocalRotation(FRotator(-DeltaAngle, 0.0f, 0.0f));
    }
}