#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PortalRTPool.generated.h"

class APortalActor;
class UTextureRenderTarget2D;

/**
 * Phase 3 — RT Memory Pool with LRU/Priority Eviction.
 *
 * N개 포탈이 K개의 공유 고해상도 RT 슬롯을 두고 경쟁.
 * 우선순위 상위 K개만 "Hot" 슬롯 점유 → SceneCapture가 그 RT에 캡처.
 * 나머지는 자기 ColdRT(저해상도)를 이전 캡처 결과로 계속 사용.
 *
 * 결과: VRAM 사용량이 N에 무관하게 O(K) 상수로 유지됨.
 */
UCLASS()
class SEAMLESS_5_5_API UPortalRTPool : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** 이번 프레임의 우선순위 등록 + Hot 슬롯 RT 반환.
     *  null 반환 = 이번 프레임 Hot 슬롯 못 받음 (Cold 상태). */
    UTextureRenderTarget2D* RequestHotRT(APortalActor* Portal, float Priority);

    /** 직전 프레임 이 포탈이 Hot이었는지 (Hot→Cold 전환 감지용) */
    bool WasHotLastFrame(APortalActor* Portal) const;

    /** 포탈 등록 해제 (EndPlay에서 호출) */
    void UnregisterPortal(APortalActor* Portal);

    /** 디버그 통계 */
    int32 GetPoolSize() const { return PoolRTs.Num(); }
    int32 GetActivePortalCount() const;
    float GetTotalPoolMemoryMB() const;

    /** 풀 설정 (CVar로 토글 가능하게 할 수도 있음) */
    static constexpr int32 PoolSize = 4;
    static constexpr int32 HotRTWidth = 1920;
    static constexpr int32 HotRTHeight = 1080;

private:
    /** 공유 Hot RT 풀 (K개) */
    UPROPERTY()
    TArray<UTextureRenderTarget2D*> PoolRTs;

    /** 현재 프레임에 각 슬롯을 점유한 포탈 */
    TArray<APortalActor*> CurrentSlotOwners;

    /** 직전 프레임 슬롯 점유 (Hot→Cold 전환 감지) */
    TArray<APortalActor*> PreviousSlotOwners;

    /** 이번 프레임 누적된 우선순위 (다음 프레임 재할당용) */
    TMap<APortalActor*, float> PendingPriorities;

    uint64 LastReallocFrame = TNumericLimits<uint64>::Max();

    void InitializePool();
    void ReallocateSlots();
};
