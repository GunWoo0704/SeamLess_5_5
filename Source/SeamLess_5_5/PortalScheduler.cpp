#include "PortalScheduler.h"
#include "PortalActor.h"

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
    UE_LOG(LogTemp, Warning, TEXT("[PortalSched] UNREGISTER (total=%d)"), RegisteredPortals.Num());
}

bool UPortalScheduler::ShouldCaptureThisFrame(APortalActor* Portal)
{
    // 포탈 0~1개면 그냥 매 프레임 캡처
    if (RegisteredPortals.Num() <= 1) return true;

    const uint64 CurrentFrame = GFrameCounter;

    // 새 프레임 진입 시 인덱스 advance + warmup 카운트다운
    if (CurrentFrame != LastScheduledFrame)
    {
        NextPortalIndex = (NextPortalIndex + 1) % RegisteredPortals.Num();
        LastScheduledFrame = CurrentFrame;
        if (WarmupFramesRemaining > 0) WarmupFramesRemaining--;
    }

    // Warmup 중이면 모든 포탈 캡처
    if (WarmupFramesRemaining > 0) return true;

    // 정상 동작: 현재 인덱스에 해당하는 포탈만 캡처
    return RegisteredPortals.IsValidIndex(NextPortalIndex)
        && RegisteredPortals[NextPortalIndex] == Portal;
}
