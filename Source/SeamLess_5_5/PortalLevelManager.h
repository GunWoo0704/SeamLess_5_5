#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/LevelStreamingDynamic.h"
#include "PortalLevelManager.generated.h"

/**
 * 다중 포탈이 같은 TargetLevel을 요청할 때 한 번만 로드되도록 캐시하는 서브시스템.
 * UWorldSubsystem이라 World당 자동으로 인스턴스 1개 생성됨.
 */
UCLASS()
class SEAMLESS_5_5_API UPortalLevelManager : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    /** 동일 TargetLevel이 이미 로드돼 있으면 캐시 반환, 없으면 새로 로드 */
    ULevelStreamingDynamic* GetOrLoadLevel(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

private:
    UPROPERTY()
    TMap<FString, ULevelStreamingDynamic*> LoadedLevels;
};