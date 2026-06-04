#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/LevelStreamingDynamic.h"
#include "PortalLevelManager.generated.h"

/**
 * 로드된 레벨 1개의 캐시 엔트리.
 * USTRUCT로 reflect돼야 TMap<...,FLoadedLevelEntry>에 UPROPERTY를 붙여
 * GC로부터 Level 포인터를 지킬 수 있다.
 *
 * (단순 raw pointer를 쓰면 World가 다른 경로로 StreamingLevels를 정리할 때
 *  dangling pointer가 될 수 있어서 strong ref + UPROPERTY 구조가 안전.)
 */
USTRUCT()
struct FPortalLoadedLevelEntry
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<ULevelStreamingDynamic> Level = nullptr;

    UPROPERTY()
    int32 RefCount = 0;
};

/**
 * 같은 TargetLevel을 가진 여러 포탈이 단 1번만 레벨을 로드하도록 캐시하는 WorldSubsystem.
 *
 * 교수님 피드백 — "한정된 메모리로 포탈을 여러 개 만들어도..." 의 핵심 구현체.
 *
 * 사용 흐름:
 *   1) 포탈 생성 시: AcquireLevel()  → refcount++ (필요하면 로드)
 *   2) 포탈 파괴 시: ReleaseLevel()  → refcount-- (0이 되면 unload)
 *
 * 기존 단순 캐시(GetOrLoadLevel)와 차이:
 *   - 참조 카운트로 마지막 포탈이 사라지면 자동 메모리 해제
 *   - 명시적 ReleaseLevel API로 PIE 종료/액터 파괴 시 정리 가능
 *   - 디버그 로그로 메모리 추적 (LOAD / REUSE / RELEASE / UNLOAD)
 */
UCLASS()
class SEAMLESS_5_5_API UPortalLevelManager : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    // ── WorldSubsystem 라이프사이클 ─────────────────────────
    virtual void Deinitialize() override;

    // ── 신규 권장 API: refcount 기반 ─────────────────────────

    /**
     * 레벨을 참조하고 인스턴스를 반환.
     * 같은 레벨이 이미 로드돼 있으면 refcount++만 하고 캐시 반환.
     * 없으면 새로 로드한 뒤 refcount=1로 등록.
     *
     * @note Location/Rotation은 새로 로드할 때만 사용됨.
     *       이미 로드된 레벨을 공유할 경우 기존 위치/회전이 그대로 쓰임.
     */
    ULevelStreamingDynamic* AcquireLevel(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

    /**
     * 레벨 참조 해제. refcount==0이 되면 즉시 unload하고 캐시에서 제거.
     * 같은 레벨을 참조하던 마지막 포탈이 사라지는 시점에 메모리 회수.
     */
    void ReleaseLevel(const TSoftObjectPtr<UWorld>& TargetLevel);

    // ── 호환: 기존 코드 깨지지 않도록 유지 (refcount 1짜리 Acquire로 위임) ──
    UE_DEPRECATED(5.5, "Use AcquireLevel/ReleaseLevel for proper refcount memory management.")
    ULevelStreamingDynamic* GetOrLoadLevel(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

    // ── 디버그/모니터링 ─────────────────────────────────────
    int32 GetLoadedLevelCount() const { return LoadedLevels.Num(); }
    int32 GetRefCount(const TSoftObjectPtr<UWorld>& TargetLevel) const;
    /** 모든 캐시된 레벨 강제 unload (PIE 종료 등에 사용) */
    void ForceUnloadAll();

private:
    /**
     * 캐시: 키(레벨 경로) → 엔트리(strong ref + refcount).
     * UPROPERTY로 reflect돼 있어 GC가 Level을 회수하지 않음.
     */
    UPROPERTY()
    TMap<FString, FPortalLoadedLevelEntry> LoadedLevels;

    /** 내부: 캐시 키 생성 일관성 보장 */
    static FString MakeKey(const TSoftObjectPtr<UWorld>& TargetLevel);

    /** 내부: 실제 LoadLevelInstanceBySoftObjectPtr 호출 */
    ULevelStreamingDynamic* DoLoad(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

    /** 내부: 실제 SetIsRequestingUnloadAndRemoval 호출 */
    void DoUnload(ULevelStreamingDynamic* Level, const FString& Key);
};
