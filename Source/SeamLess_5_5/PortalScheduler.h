#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PortalScheduler.generated.h"

class APortalActor;

/**
 * Phase 2 — Frame Budget Allocator.
 * 다중 포탈을 라운드로빈으로 갱신해서 프레임당 캡처 비용을 1개 포탈 비용으로 제한.
 * 등록된 포탈 N개가 있을 때, 매 프레임마다 1개만 SceneCapture 활성화.
 */
UCLASS()
class SEAMLESS_5_5_API UPortalScheduler : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    /** 포탈을 스케줄러에 등록 (BeginPlay에서 호출) */
    void RegisterPortal(APortalActor* Portal);

    /** 포탈 제거 (EndPlay에서 호출) */
    void UnregisterPortal(APortalActor* Portal);

    /** 이번 프레임에 이 포탈이 캡처할 차례인가?
     *  Round-robin: 매 프레임 인덱스 advance, 그 인덱스 포탈만 true 반환.
     *  처음 N프레임은 warmup으로 모든 포탈이 true 반환 (RT 초기화). */
    bool ShouldCaptureThisFrame(APortalActor* Portal);

    /** 현재 등록된 포탈 수 */
    int32 GetNumPortals() const { return RegisteredPortals.Num(); }

private:
    UPROPERTY()
    TArray<APortalActor*> RegisteredPortals;

    /** 프레임 카운터 — GFrameCounter 변화 감지용 */
    uint64 LastScheduledFrame = TNumericLimits<uint64>::Max();

    /** 다음 갱신할 포탈 인덱스 (라운드로빈) */
    int32 NextPortalIndex = -1;

    /** Warmup: 첫 몇 프레임은 모든 포탈이 캡처해서 RT 초기화 */
    int32 WarmupFramesRemaining = 8;
};
