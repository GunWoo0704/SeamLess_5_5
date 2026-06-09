#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "PortalScheduler.generated.h"

class APortalActor;

/**
 * Frame Budget Allocator — 지각 기반 캡처 예산 스케줄러.
 *
 * 매 프레임, 등록된 N개 포탈 중 "예산(BudgetCount)" 개수만큼만 풀 캡처하도록 선택한다.
 * 선택 기준은 라운드로빈이 아니라 urgency = 우선순위 × (1 + AgingWeight × 미갱신프레임수).
 *   - 우선순위: APortalActor::GetCapturePriority() (거리·면적·시선)
 *   - aging   : 오래 갱신 안 된 포탈을 끌어올려 starvation 방지
 * 라운드로빈은 "모든 우선순위 동일 + 예산 1"인 특수 케이스이므로 본 스케줄러가 일반화한다.
 *
 * 캡처 비용은 BudgetCount(상수)에 묶이므로 포탈 수 N과 무관 → 프레임타임 O(1).
 *
 * CVar:
 *   r.Portal.BudgetCount  (기본 1)   프레임당 풀 캡처 허용 포탈 수
 *   r.Portal.AgingWeight  (기본 0.5) 미갱신 포탈 우선순위 가산 가중치
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

    /** 이번 프레임에 이 포탈이 풀 캡처할 차례인가?
     *  새 프레임이면 예산 기반 선택을 1회 갱신한 뒤, 선택된 집합 포함 여부를 반환. */
    bool ShouldCaptureThisFrame(APortalActor* Portal);

    /** 이 포탈의 레벨을 렌더/Tick 활성으로 둘 것인가?
     *  우선순위(시선·거리) 상위 ActiveLevels개만 true. 나머지는 숨김(stale 표시).
     *  → 모여 있는 다른-레벨 다중 포탈에서 동시 렌더 레벨 수를 제한해 CPU·GPU 절감. */
    bool ShouldLevelBeActive(APortalActor* Portal);

    /** 현재 등록된 포탈 수 */
    int32 GetNumPortals() const { return RegisteredPortals.Num(); }

private:
    /** 새 프레임이면 urgency 기준으로 이번 프레임 캡처 대상(top-K)을 다시 고른다. */
    void RebuildSelectionIfNewFrame();

    /** GC 추적 필요 — 매 프레임 순회하므로 댕글링 방지 */
    UPROPERTY()
    TArray<TObjectPtr<APortalActor>> RegisteredPortals;

    /** 이번 프레임 선택된 포탈 집합 (매 프레임 재구성, transient) */
    TSet<APortalActor*> ChosenThisFrame;

    /** 이번 프레임 레벨을 활성(렌더+Tick)으로 둘 포탈 집합 = 우선순위 상위 N */
    TSet<APortalActor*> ActiveLevelsThisFrame;

    /** 포탈별 마지막 캡처 프레임 (aging 계산용). 등록 해제 시 제거. */
    TMap<APortalActor*, uint64> LastCaptureFrame;

    /** 프레임 변화 감지 (GFrameCounter) */
    uint64 LastScheduledFrame = TNumericLimits<uint64>::Max();

    /** Warmup: 첫 몇 프레임은 모든 포탈 캡처해서 RT 초기화 */
    int32 WarmupFramesRemaining = 8;
};
