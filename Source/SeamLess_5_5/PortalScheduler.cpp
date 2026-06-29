#include "PortalScheduler.h"
#include "PortalActor.h"
#include "HAL/IConsoleManager.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "TimerManager.h"
#include "Engine/World.h"

// CSV 카테고리 "Portal" — csv.Start 캡처 시 Portal/* 열로 기록됨.
// G4(갱신 주기 vs N) 그래프와 스케줄러 동작 검증의 데이터 소스.
CSV_DEFINE_CATEGORY(Portal, true);

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

// 활성 집합 히스테리시스: 직전 프레임 활성이던 레벨의 우선순위에 곱해주는 보너스.
//   1보다 크면 "한번 활성이면 잘 안 바뀜" → 경계에서 깜빡임(프레임 스파이크) 방지.
static TAutoConsoleVariable<float> CVarPortalActiveHysteresis(
    TEXT("r.Portal.ActiveHysteresis"),
    1.5f,
    TEXT("직전 활성 레벨에 주는 우선순위 보너스 배율(끈적임). 기본 1.5. 1.0=히스테리시스 없음."),
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
    PrevActiveLevels.Remove(Portal);
    LastCaptureFrame.Remove(Portal);
    UE_LOG(LogTemp, Warning, TEXT("[PortalSched] UNREGISTER (total=%d)"), RegisteredPortals.Num());
}

void UPortalScheduler::RebuildSelectionIfNewFrame()
{
    const uint64 CurrentFrame = GFrameCounter;
    if (CurrentFrame == LastScheduledFrame) return;
    LastScheduledFrame = CurrentFrame;

    // 직전 활성 집합 보존(히스테리시스) 후 리셋
    PrevActiveLevels = ActiveLevelsThisFrame;
    ChosenThisFrame.Reset();
    ActiveLevelsThisFrame.Reset();

    const int32 ActiveLevels = FMath::Max(1, CVarPortalActiveLevels.GetValueOnGameThread());
    const float Hysteresis = CVarPortalActiveHysteresis.GetValueOnGameThread();

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
        RecordCsvStats(CurrentFrame);
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

        // 활성 레벨 랭킹엔 히스테리시스 적용: 직전 활성이면 우선순위에 보너스 → 잘 안 바뀜
        const float PriorityForActive = PrevActiveLevels.Contains(P) ? (Priority * Hysteresis) : Priority;
        RankedByPriority.Add(TPair<float, APortalActor*>(PriorityForActive, P));
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

    RecordCsvStats(CurrentFrame);
}

// ───────────────────────────────────────────────────────────────
// CSV 계측 (csv.Start 중일 때만 비용 발생, 평상시 no-op)
//   Portal/NumPortals       등록 포탈 수 (측정 셀 검증용)
//   Portal/NumCaptured      이번 프레임 풀 캡처 수 (= Budget 준수 확인)
//   Portal/NumActiveLevels  이번 프레임 활성 레벨 수 (= ActiveLevels 준수 확인)
//   Portal/MaxStaleFrames   가장 오래 갱신 안 된 포탈의 미갱신 프레임 수 ← M2 목표 스펙
//   Portal/AvgStaleFrames   평균 미갱신 프레임 수 (이론치 N/Budget 과 비교)
void UPortalScheduler::RecordCsvStats(uint64 CurrentFrame)
{
#if CSV_PROFILER
    int32 MaxStale = 0;
    float SumStale = 0.0f;
    int32 Count = 0;
    for (APortalActor* P : RegisteredPortals)
    {
        if (!IsValid(P)) continue;
        const uint64 Last = LastCaptureFrame.FindRef(P);
        // Warmup이 전 포탈을 캡처하므로 Last==0(미캡처)은 등록 직후 한 프레임뿐 → 0 처리
        const int32 Stale = (Last == 0) ? 0 : static_cast<int32>(CurrentFrame - Last);
        MaxStale = FMath::Max(MaxStale, Stale);
        SumStale += static_cast<float>(Stale);
        Count++;
    }
    CSV_CUSTOM_STAT(Portal, NumPortals,      Count,                                      ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Portal, NumCaptured,     ChosenThisFrame.Num(),                      ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Portal, NumActiveLevels, ActiveLevelsThisFrame.Num(),                ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Portal, MaxStaleFrames,  MaxStale,                                   ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Portal, AvgStaleFrames,  (Count > 0) ? SumStale / Count : 0.0f,      ECsvCustomStatOp::Set);
#endif
}

// ───────────────────────────────────────────────────────────────
// Sprint 0-3: 측정 1셀 자동 실행 커맨드
//
//   Portal.Benchmark <BudgetCount> <ActiveLevels> [WarmupSec=10] [CaptureSec=30]
//
//   1) CVar 설정 → 2) WarmupSec 대기(Lumen 수렴·스케줄러 워밍업)
//   3) CSV 캡처 시작 → 4) CaptureSec 후 종료
//   출력: Saved/Profiling/CSV/Portal_N{포탈수}_B{예산}_A{활성레벨}_{시각}.csv
//   → 측정계획.md의 셀 1개 = 커맨드 1회. 파일명만으로 분석 스크립트가 조건 식별.
#if CSV_PROFILER
static FTimerHandle GPortalBenchWarmupHandle;
static FTimerHandle GPortalBenchStopHandle;

static FAutoConsoleCommandWithWorldAndArgs CmdPortalBenchmark(
    TEXT("Portal.Benchmark"),
    TEXT("측정 1셀 자동 실행: Portal.Benchmark <BudgetCount> <ActiveLevels> [WarmupSec=10] [CaptureSec=30]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (!World) return;

            const int32 Budget   = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 1;
            const int32 Active   = (Args.Num() > 1) ? FCString::Atoi(*Args[1]) : 3;
            const float WarmupS  = (Args.Num() > 2) ? FCString::Atof(*Args[2]) : 10.0f;
            const float CaptureS = (Args.Num() > 3) ? FCString::Atof(*Args[3]) : 30.0f;

            CVarPortalBudgetCount.AsVariable()->Set(Budget, ECVF_SetByConsole);
            CVarPortalActiveLevels.AsVariable()->Set(Active, ECVF_SetByConsole);

            const UPortalScheduler* Sched = World->GetSubsystem<UPortalScheduler>();
            const int32 NumPortals = Sched ? Sched->GetNumPortals() : -1;

            // Phase2(스케줄러) OFF = Naive 베이스라인 → 파일명에 태그 (분석 스크립트가 조건 식별)
            bool bNaive = false;
            if (const IConsoleVariable* Phase2 = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Portal.Phase2")))
            {
                bNaive = (Phase2->GetInt() == 0);
            }

            const FString FileName = FString::Printf(TEXT("Portal_N%d_B%d_A%d_%s%s.csv"),
                NumPortals, Budget, Active,
                bNaive ? TEXT("naive_") : TEXT(""),
                *FDateTime::Now().ToString(TEXT("%m%d-%H%M%S")));

            UE_LOG(LogTemp, Warning,
                TEXT("[PortalBench] N=%d Budget=%d Active=%d | warmup %.0fs → capture %.0fs → %s.csv"),
                NumPortals, Budget, Active, WarmupS, CaptureS, *FileName);

            TWeakObjectPtr<UWorld> WeakWorld = World;
            World->GetTimerManager().SetTimer(GPortalBenchWarmupHandle,
                FTimerDelegate::CreateLambda([WeakWorld, FileName, CaptureS]()
                {
                    if (!WeakWorld.IsValid()) return;

                    FCsvProfiler::Get()->BeginCapture(-1, FString(), FileName);
                    UE_LOG(LogTemp, Warning, TEXT("[PortalBench] CSV capture START (%s)"), *FileName);

                    WeakWorld->GetTimerManager().SetTimer(GPortalBenchStopHandle,
                        FTimerDelegate::CreateLambda([]()
                        {
                            FCsvProfiler::Get()->EndCapture();
                            UE_LOG(LogTemp, Warning,
                                TEXT("[PortalBench] CSV capture END → Saved/Profiling/CSV/"));
                        }),
                        CaptureS, false);
                }),
                WarmupS, false);
        })
);
#endif // CSV_PROFILER

UCameraComponent* UPortalScheduler::GetActiveCamera()
{
    const uint64 Frame = GFrameCounter;
    if (Frame == CachedCameraFrame)
    {
        return CachedCamera.Get();  // 같은 프레임이면 캐시 반환 (비싼 조회 생략)
    }
    CachedCameraFrame = Frame;
    CachedCamera = nullptr;

    if (UWorld* World = GetWorld())
    {
        if (APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0))
        {
            CachedCamera = Pawn->FindComponentByClass<UCameraComponent>();
        }
    }
    return CachedCamera.Get();
}

bool UPortalScheduler::ShouldLevelBeActive(APortalActor* Portal)
{
    // CSV 계측이 매 프레임 기록되도록 early-return 전에 선택 갱신
    // (N ≤ 한도면 Rebuild가 전부 선택하므로 결과는 동일)
    RebuildSelectionIfNewFrame();

    const int32 ActiveLevels = FMath::Max(1, CVarPortalActiveLevels.GetValueOnGameThread());

    // 포탈 수가 활성 한도 이하면 전부 활성
    if (RegisteredPortals.Num() <= ActiveLevels) return true;

    return ActiveLevelsThisFrame.Contains(Portal);
}

bool UPortalScheduler::ShouldCaptureThisFrame(APortalActor* Portal)
{
    RebuildSelectionIfNewFrame();

    const int32 Budget = FMath::Max(1, CVarPortalBudgetCount.GetValueOnGameThread());

    // 포탈 수가 예산 이하면 전부 캡처 가능 (스케줄링 불필요)
    if (RegisteredPortals.Num() <= Budget) return true;

    return ChosenThisFrame.Contains(Portal);
}
