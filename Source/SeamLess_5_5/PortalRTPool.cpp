#include "PortalRTPool.h"
#include "PortalActor.h"
#include "Engine/TextureRenderTarget2D.h"

void UPortalRTPool::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    InitializePool();
}

void UPortalRTPool::Deinitialize()
{
    PoolRTs.Empty();
    CurrentSlotOwners.Empty();
    PreviousSlotOwners.Empty();
    PendingPriorities.Empty();
    Super::Deinitialize();
}

void UPortalRTPool::InitializePool()
{
    if (PoolRTs.Num() > 0) return;

    PoolRTs.Reserve(PoolSize);
    CurrentSlotOwners.Init(nullptr, PoolSize);
    PreviousSlotOwners.Init(nullptr, PoolSize);

    for (int32 i = 0; i < PoolSize; i++)
    {
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this);
        RT->InitAutoFormat(HotRTWidth, HotRTHeight);
        RT->bAutoGenerateMips = false;
        RT->RenderTargetFormat = RTF_RGBA16f;
        PoolRTs.Add(RT);
    }

    UE_LOG(LogTemp, Warning,
        TEXT("[RTPool] Initialized: %d slots @ %dx%d (%.1f MB total)"),
        PoolSize, HotRTWidth, HotRTHeight, GetTotalPoolMemoryMB());
}

UTextureRenderTarget2D* UPortalRTPool::RequestHotRT(APortalActor* Portal, float Priority)
{
    if (!Portal) return nullptr;

    const uint64 CurFrame = GFrameCounter;
    if (CurFrame != LastReallocFrame)
    {
        // 프레임 진입 — 직전 프레임 우선순위로 슬롯 재할당
        PreviousSlotOwners = CurrentSlotOwners;
        ReallocateSlots();
        LastReallocFrame = CurFrame;
        PendingPriorities.Reset();
    }

    // 이번 프레임 우선순위 기록 (다음 프레임 할당에 사용)
    PendingPriorities.FindOrAdd(Portal) = Priority;

    // 이번 프레임 슬롯 점유 여부 확인
    const int32 SlotIdx = CurrentSlotOwners.Find(Portal);
    if (SlotIdx != INDEX_NONE)
    {
        return PoolRTs[SlotIdx];
    }
    return nullptr;
}

bool UPortalRTPool::WasHotLastFrame(APortalActor* Portal) const
{
    return PreviousSlotOwners.Contains(Portal);
}

void UPortalRTPool::ReallocateSlots()
{
    // 우선순위 정보 없으면 슬롯 비움
    if (PendingPriorities.Num() == 0)
    {
        for (int32 i = 0; i < CurrentSlotOwners.Num(); i++)
            CurrentSlotOwners[i] = nullptr;
        return;
    }

    // 유효한 포탈만 추려서 우선순위 내림차순 정렬
    TArray<TPair<APortalActor*, float>> Sorted;
    Sorted.Reserve(PendingPriorities.Num());
    for (auto& Pair : PendingPriorities)
    {
        if (IsValid(Pair.Key))
            Sorted.Add(Pair);
    }
    Sorted.Sort([](const auto& A, const auto& B) { return A.Value > B.Value; });

    // 상위 PoolSize개에 슬롯 할당 (LRU + Priority 결합)
    for (int32 i = 0; i < CurrentSlotOwners.Num(); i++)
    {
        CurrentSlotOwners[i] = (i < Sorted.Num()) ? Sorted[i].Key : nullptr;
    }
}

void UPortalRTPool::UnregisterPortal(APortalActor* Portal)
{
    PendingPriorities.Remove(Portal);
    for (int32 i = 0; i < CurrentSlotOwners.Num(); i++)
    {
        if (CurrentSlotOwners[i] == Portal) CurrentSlotOwners[i] = nullptr;
    }
    // PreviousSlotOwners는 별도 배열이므로 인덱스 범위를 따로 검사 (out-of-bounds 방어)
    for (int32 i = 0; i < PreviousSlotOwners.Num(); i++)
    {
        if (PreviousSlotOwners[i] == Portal) PreviousSlotOwners[i] = nullptr;
    }
}

int32 UPortalRTPool::GetActivePortalCount() const
{
    int32 Count = 0;
    for (APortalActor* P : CurrentSlotOwners) if (P) Count++;
    return Count;
}

float UPortalRTPool::GetTotalPoolMemoryMB() const
{
    // RGBA16f = 8 bytes per pixel
    const float BytesPerRT = HotRTWidth * HotRTHeight * 8.0f;
    return (PoolSize * BytesPerRT) / (1024.0f * 1024.0f);
}
