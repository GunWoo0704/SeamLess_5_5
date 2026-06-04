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

    // RotationDuration이 0(또는 음수/비정상)이면 360/0 = inf → NaN 회전 → 크래시.
    // 안전하게 방어: 유효하지 않으면 이번 틱은 건너뛴다.
    if (!FMath::IsFinite(RotationDuration) || RotationDuration <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    // RotationDuration초에 360도 도는 초당 회전 각도 계산
    float DegreesPerSecond = 360.0f / RotationDuration;
    float DeltaAngle = DegreesPerSecond * DeltaTime;

    // DeltaTime 스파이크 등으로 비정상 값이 나오면 회전하지 않음
    if (!FMath::IsFinite(DeltaAngle))
    {
        return;
    }

    TArray<AActor*> DirectionalLights;
    UGameplayStatics::GetAllActorsOfClass(
        GetWorld(),
        ADirectionalLight::StaticClass(),
        DirectionalLights);

    for (AActor* Light : DirectionalLights)
    {
        // ���밪 ���� ��� �� ������ ���ݾ� ���� ȸ��
        Light->AddActorLocalRotation(FRotator(-DeltaAngle, 0.0f, 0.0f));
    }
}