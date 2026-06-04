#include "PortalScheduler.h"
#include "PortalActor.h"
#include "HAL/IConsoleManager.h"

// ───────────────────────────────────────────────────────────────
// 캡처 예산: 프레임당 풀 캡처를 허용할 포탈 수. N과 무관한 상수 → 프레임 O(1).
//   1 이면 라운드로빈과 동일한 비용(단, 선택은 우선순위 기반).
static TAutoConsoleVariable<int32> CVarPortalBudgetCount(
    TEXT("r.Portal.BudgetCount"),
    1,
    TEXT("프레임당 풀 캡처를 허용할 포탈 수(예산). 기본 1.\n")
    TEXT("나머지 포탈은 이전 RT 재사용/리프로젝션으로 처리."),
    ECVF_Default
);

// aging 가중치: 오래 갱신 안 된 포탈의 urgency를 끌어올려 starvation 방지.
//   urgency = priority × (1 + AgingWeight × 미갱신프레임수)
static TAutoConsoleVariable<float> CVarPortalAgingWeight(
    TEXT("r.Portal.AgingWeight"),
    0.5f,
    TEXT("미갱신 포탈 우선순위 가산 가중치(starvation 방지). 기본 0.5.\n")
    TEXT("0이면 순수 우선순위만, 크면 라운드로빈에 가까워짐."),
    ECVF_Default
);

void UPortalScheduler::RegisterPortal(APortalActor* Portal)
{
    if (Portal && !RegisteredPortals.Contains(Portal))
    {
        RegisteredPortals.Add(Portal);
        UE_LOG(LogTemp, Warning, TEXT("[PortalSched] REGISTER (total=%d)"), RegisteredPortals.Num());
    }
}

void UPortalScheduler::UnregisterPortal(APortalActor* Portal)
{
    RegisteredPortals.Remove(Portal);
    ChosenThisFrame.Remove(Portal);
    LastCaptureFrame.Remove(Portal);
    UE_LOG(LogTemp, Warning, TEXT("[PortalSched] UNREGISTER (total=%d)"), RegisteredPortals.Num());
}

void UPortalScheduler::RebuildSelectionIfNewFrame()
{
    const uint64 CurrentFrame = GFrameCounter;
    if (CurrentFrame == LastScheduledFrame) return;
    LastScheduledFrame = CurrentFrame;

    ChosenThisFrame.Reset();

    // Warmup: 첫 몇 프레임은 모든 포탈 캡처 (RT 초기화)
    if (WarmupFramesRemaining > 0)
    {
        WarmupFramesRemaining--;
        for (APortalActor* P : RegisteredPortals)
        {
            if (!IsValid(P)) continue;
            ChosenThisFrame.Add(P);
            LastCaptureFrame.Add(P, CurrentFrame);
        }
        return;
    }

    const int32 Budget = FMath::Max(1, CVarPortalBudgetCount.GetValueOnGameThread());
    const float AgingW = CVarPortalAgingWeight.GetValueOnGameThread();

    // ── 각 포탈의 urgency 계산 ──
    TArray<TPair<float, APortalActor*>> Ranked;
    Ranked.Reserve(RegisteredPortals.Num());
    for (APortalActor* P : RegisteredPortals)
    {
        if (!IsValid(P)) continue;

        const float Priority = P->GetCapturePriority();

        // 미갱신 프레임 수 (한 번도 캡처 안 됐으면 큰 값 → 곧 선택되게)
        const uint64 Last = LastCaptureFrame.FindRef(P);  // 없으면 0
        const uint64 FramesSince = (Last == 0) ? 1000 : (CurrentFrame - Last);

        const float Urgency = Priority * (1.0f + AgingW * static_cast<float>(FramesSince));
        Ranked.Add(TPair<float, APortalActor*>(Urgency, P));
    }

    // urgency 내림차순 정렬 후 상위 Budget개 선택
    Ranked.Sort([](const TPair<float, APortalActor*>& A, const TPair<float, APortalActor*>& B)
    {
        return A.Key > B.Key;
    });

    const int32 NumChosen = FMath::Min(Budget, Ranked.Num());
    for (int32 i = 0; i < NumChosen; i++)
    {
        ChosenThisFrame.Add(Ranked[i].Value);
        LastCaptureFrame.Add(Ranked[i].Value, CurrentFrame);
    }
}

bool UPortalScheduler::ShouldCaptureThisFrame(APortalActor* Portal)
{
    const int32 Budget = FMath::Max(1, CVarPortalBudgetCount.GetValueOnGameThread());

    // 포탈 수가 예산 이하면 전부 캡처 가능 (스케줄링 불필요)
    if (RegisteredPortals.Num() <= Budget) return true;

    RebuildSelectionIfNewFrame();
    return ChosenThisFrame.Contains(Portal);
}
