#include "PortalLevelManager.h"
#include "Engine/World.h"

// ────────────────────────────────────────────────────────────────
// 키 헬퍼
// ────────────────────────────────────────────────────────────────

FString UPortalLevelManager::MakeKey(const TSoftObjectPtr<UWorld>& TargetLevel)
{
    return TargetLevel.ToSoftObjectPath().ToString();
}

// ────────────────────────────────────────────────────────────────
// 신규 API: AcquireLevel / ReleaseLevel (refcount 기반)
// ────────────────────────────────────────────────────────────────

ULevelStreamingDynamic* UPortalLevelManager::AcquireLevel(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    if (TargetLevel.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("[PortalLevelMgr] AcquireLevel: TargetLevel is null"));
        return nullptr;
    }

    const FString Key = MakeKey(TargetLevel);

    // 1) 이미 캐시에 있으면 refcount++ 후 반환
    if (FPortalLoadedLevelEntry* Entry = LoadedLevels.Find(Key))
    {
        if (IsValid(Entry->Level))
        {
            Entry->RefCount++;
            UE_LOG(LogTemp, Warning,
                TEXT("[PortalLevelMgr] REUSE  refcount %d→%d  %s"),
                Entry->RefCount - 1, Entry->RefCount, *Key);
            return Entry->Level;
        }
        // 객체가 어떤 이유로든 dead면 캐시 정리하고 아래로 떨어져서 재로드
        LoadedLevels.Remove(Key);
    }

    // 2) 새로 로드
    ULevelStreamingDynamic* NewLevel = DoLoad(TargetLevel, Location, Rotation);
    if (NewLevel)
    {
        FPortalLoadedLevelEntry NewEntry;
        NewEntry.Level = NewLevel;
        NewEntry.RefCount = 1;
        LoadedLevels.Add(Key, NewEntry);
        UE_LOG(LogTemp, Warning,
            TEXT("[PortalLevelMgr] LOAD   refcount 0→1  %s"), *Key);
    }
    return NewLevel;
}

void UPortalLevelManager::ReleaseLevel(const TSoftObjectPtr<UWorld>& TargetLevel)
{
    if (TargetLevel.IsNull()) return;

    const FString Key = MakeKey(TargetLevel);

    FPortalLoadedLevelEntry* Entry = LoadedLevels.Find(Key);
    if (!Entry)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[PortalLevelMgr] RELEASE called on unknown key  %s"), *Key);
        return;
    }

    Entry->RefCount--;
    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] RELEASE refcount %d→%d  %s"),
        Entry->RefCount + 1, Entry->RefCount, *Key);

    // refcount 0 → 즉시 unload 및 캐시 제거
    if (Entry->RefCount <= 0)
    {
        ULevelStreamingDynamic* Lvl = Entry->Level;
        DoUnload(Lvl, Key);
        LoadedLevels.Remove(Key);
        UE_LOG(LogTemp, Warning,
            TEXT("[PortalLevelMgr] UNLOAD (refcount=0)  %s"), *Key);
    }
}

// ────────────────────────────────────────────────────────────────
// 호환: 기존 GetOrLoadLevel — AcquireLevel로 위임
//   (콜러가 ReleaseLevel을 부르지 않으면 leak이지만,
//    기존 코드 깨지지 않도록 일단 유지)
// ────────────────────────────────────────────────────────────────

PRAGMA_DISABLE_DEPRECATION_WARNINGS
ULevelStreamingDynamic* UPortalLevelManager::GetOrLoadLevel(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] DEPRECATED: GetOrLoadLevel 호출. AcquireLevel/ReleaseLevel로 교체 권장."));
    return AcquireLevel(TargetLevel, Location, Rotation);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

// ────────────────────────────────────────────────────────────────
// 라이프사이클 / 강제 정리
// ────────────────────────────────────────────────────────────────

void UPortalLevelManager::Deinitialize()
{
    ForceUnloadAll();
    Super::Deinitialize();
}

void UPortalLevelManager::ForceUnloadAll()
{
    if (LoadedLevels.Num() == 0) return;

    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] ForceUnloadAll: %d levels"), LoadedLevels.Num());

    for (auto& Pair : LoadedLevels)
    {
        if (IsValid(Pair.Value.Level))
        {
            DoUnload(Pair.Value.Level, Pair.Key);
        }
    }
    LoadedLevels.Empty();
}

int32 UPortalLevelManager::GetRefCount(const TSoftObjectPtr<UWorld>& TargetLevel) const
{
    if (TargetLevel.IsNull()) return 0;
    const FPortalLoadedLevelEntry* Entry = LoadedLevels.Find(MakeKey(TargetLevel));
    return Entry ? Entry->RefCount : 0;
}

// ────────────────────────────────────────────────────────────────
// 내부 로드/언로드 헬퍼
// ────────────────────────────────────────────────────────────────

ULevelStreamingDynamic* UPortalLevelManager::DoLoad(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    bool bSuccess = false;
    ULevelStreamingDynamic* NewLevel = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(
        GetWorld(),
        TargetLevel,
        Location,
        Rotation,
        bSuccess);

    if (!NewLevel || !bSuccess)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[PortalLevelMgr] LOAD FAILED  %s"),
            *TargetLevel.ToSoftObjectPath().ToString());
        return nullptr;
    }
    return NewLevel;
}

void UPortalLevelManager::DoUnload(ULevelStreamingDynamic* Level, const FString& Key)
{
    if (!Level) return;

    // SetIsRequestingUnloadAndRemoval → 다음 World tick에서 안전하게 언로드 + 제거
    Level->SetIsRequestingUnloadAndRemoval(true);
    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] Requested unload  %s"), *Key);
}
