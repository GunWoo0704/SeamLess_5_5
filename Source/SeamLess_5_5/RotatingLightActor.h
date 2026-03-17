#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/DirectionalLightComponent.h"
#include "RotatingLightActor.generated.h"

UCLASS()
class SEAMLESS_5_5_API ARotatingLightActor : public AActor
{
    GENERATED_BODY()

public:
    ARotatingLightActor();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

    // 회전 총 시간 (기본 20초)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
    float RotationDuration = 20.0f;

private:
    float ElapsedTime = 0.0f;
};