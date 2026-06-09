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

// 동시에 렌더+Tick 활성으로 둘 레벨(포탈) 수. 우선순위(시선·거리) 상위 N개.
//   모여 있는 다른-레벨 다중 포탈에서 동시 렌더 레벨 수를 N으로 묶어 CPU·GPU 절감.
//   값이 작을수록 빠르지만, 처음 보는(상위 밖) 포탈은 stale/검정으로 보임.
static TAutoConsoleVariable<int32> CVarPortalActiveLevels(
    TEXT("r.Portal.ActiveLevels"),
    3,
    TEXT("동시에 렌더/Tick 활성으로 둘 포탈 레벨 수(우선순위 상위 N). 기본 3.\n")
    TEXT("나머지 레벨은 숨기고 Tick 꺼서 비용 절감, 포탈은 마지막 캡처 유지."),
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
    ActiveLevelsThisFrame.Remove(Portal);
    LastCaptureFrame.Remove(Portal);
    UE_LOG(LogTemp, Warning, TEXT("[PortalSched] UNREGISTER (total=%d)"), RegisteredPortals.Num());
}

void UPortalScheduler::RebuildSelectionIfNewFrame()
{
    const uint64 CurrentFrame = GFrameCounter;
    if (CurrentFrame == LastScheduledFrame) return;
    LastScheduledFrame = CurrentFrame;

    ChosenThisFrame.Reset();
    ActiveLevelsThisFrame.Reset();

    const int32 ActiveLevels = FMath::Max(1, CVarPortalActiveLevels.GetValueOnGameThread());

    // Warmup: 첫 몇 프레임은 모든 포탈 캡처 + 모든 레벨 활성 (RT 초기화)
    if (WarmupFramesRemaining > 0)
    {
        WarmupFramesRemaining--;
        for (APortalActor* P : RegisteredPortals)
        {
            if (!IsValid(P)) continue;
            ChosenThisFrame.Add(P);
            ActiveLevelsThisFrame.Add(P);
            LastCaptureFrame.Add(P, CurrentFrame);
        }
        return;
    }

    const int32 Budget = FMath::Max(1, CVarPortalBudgetCount.GetValueOnGameThread());
    const float AgingW = CVarPortalAgingWeight.GetValueOnGameThread();

    // ── 각 포탈의 우선순위(raw) 와 urgency(aging 포함) 계산 ──
    TArray<TPair<float, APortalActor*>> RankedByUrgency;   // 캡처 선택용
    TArray<TPair<float, APortalActor*>> RankedByPriority;  // 활성 레벨 선택용(안정적)
    RankedByUrgency.Reserve(RegisteredPortals.Num());
    RankedByPriority.Reserve(RegisteredPortals.Num());
    for (APortalActor* P : RegisteredPortals)
    {
        if (!IsValid(P)) continue;

        const float Priority = P->GetCapturePriority();

        // 미갱신 프레임 수 (한 번도 캡처 안 됐으면 큰 값 → 곧 선택되게)
        const uint64 Last = LastCaptureFrame.FindRef(P);  // 없으면 0
        const uint64 FramesSince = (Last == 0) ? 1000 : (CurrentFrame - Last);

        const float Urgency = Priority * (1.0f + AgingW * static_cast<float>(FramesSince));
        RankedByUrgency.Add(TPair<float, APortalActor*>(Urgency, P));
        RankedByPriority.Add(TPair<float, APortalActor*>(Priority, P));
    }

    auto SortDesc = [](const TPair<float, APortalActor*>& A, const TPair<float, APortalActor*>& B)
    {
        return A.Key > B.Key;
    };

    // 캡처 대상: urgency 상위 Budget개
    RankedByUrgency.Sort(SortDesc);
    const int32 NumChosen = FMath::Min(Budget, RankedByUrgency.Num());
    for (int32 i = 0; i < NumChosen; i++)
    {
        ChosenThisFrame.Add(RankedByUrgency[i].Value);
        LastCaptureFrame.Add(RankedByUrgency[i].Value, CurrentFrame);
    }

    // 활성 레벨: raw 우선순위(시선·거리) 상위 ActiveLevels개 (aging 미포함 → 안정적, 토글 깜빡임 적음)
    RankedByPriority.Sort(SortDesc);
    const int32 NumActive = FMath::Min(ActiveLevels, RankedByPriority.Num());
    for (int32 i = 0; i < NumActive; i++)
    {
        ActiveLevelsThisFrame.Add(RankedByPriority[i].Value);
    }
}

bool UPortalScheduler::ShouldLevelBeActive(APortalActor* Portal)
{
    const int32 ActiveLevels = FMath::Max(1, CVarPortalActiveLevels.GetValueOnGameThread());

    // 포탈 수가 활성 한도 이하면 전부 활성
    if (RegisteredPortals.Num() <= ActiveLevels) return true;

    RebuildSelectionIfNewFrame();
    return ActiveLevelsThisFrame.Contains(Portal);
}

bool UPortalScheduler::ShouldCaptureThisFrame(APortalActor* Portal)
{
    const int32 Budget = FMath::Max(1, CVarPortalBudgetCount.GetValueOnGameThread());

    // 포탈 수가 예산 이하면 전부 캡처 가능 (스케줄링 불필요)
    if (RegisteredPortals.Num() <= Budget) return true;

    RebuildSelectionIfNewFrame();
    return ChosenThisFrame.Contains(Portal);
}
