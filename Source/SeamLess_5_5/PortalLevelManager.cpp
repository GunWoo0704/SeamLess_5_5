#include "PortalLevelManager.h"
#include "Engine/World.h"

ULevelStreamingDynamic* UPortalLevelManager::GetOrLoadLevel(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    if (TargetLevel.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("[PortalLevelMgr] TargetLevel is null"));
        return nullptr;
    }

    const FString Key = TargetLevel.ToSoftObjectPath().ToString();

    if (ULevelStreamingDynamic** Existing = LoadedLevels.Find(Key))
    {
        if (*Existing && IsValid(*Existing))
        {
            UE_LOG(LogTemp, Warning, TEXT("[PortalLevelMgr] REUSE %s"), *Key);
            return *Existing;
        }
        LoadedLevels.Remove(Key);
    }

    bool bSuccess = false;
    ULevelStreamingDynamic* NewLevel = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(
        GetWorld(),
        TargetLevel,
        Location,
        Rotation,
        bSuccess);

    if (NewLevel && bSuccess)
    {
        LoadedLevels.Add(Key, NewLevel);
        UE_LOG(LogTemp, Warning, TEXT("[PortalLevelMgr] LOAD %s"), *Key);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[PortalLevelMgr] LOAD FAILED %s"), *Key);
    }

    return NewLevel;
}