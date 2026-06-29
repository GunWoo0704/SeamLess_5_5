// ════════════════════════════════════════════════════════════════
// PortalRTPool.cpp — Phase 3: RT 메모리 풀 (구현)
//
// [프레임 흐름 요약]
//   매 프레임, 각 포탈이 RequestHotRT(자기, 우선순위)를 호출한다.
//   1) 그 프레임의 "첫" 호출이 재할당을 트리거:
//      직전 프레임에 모인 우선순위(PendingPriorities)로 상위 K개를 뽑아
//      슬롯 주인(CurrentSlotOwners)을 갈아끼움. 직전 점유 상태는
//      PreviousSlotOwners로 백업 (Hot→Cold 전환 감지용).
//   2) 이후 호출들은 "내가 슬롯을 받았나?"만 조회해서 RT 또는 null 반환.
//   즉, 슬롯 배정은 항상 1프레임 늦은 우선순위 기준으로 결정된다.
//
// [호출자] PortalActor (Tick에서 RequestHotRT, EndPlay에서 UnregisterPortal).
//          우선순위 값 자체는 UPortalScheduler 쪽 계산 결과를 받아 옴.
// ════════════════════════════════════════════════════════════════
#include "PortalRTPool.h"
#include "PortalActor.h"                      // APortalActor 정의 (헤더에서는 전방 선언만 했음)
#include "Engine/TextureRenderTarget2D.h"     // UTextureRenderTarget2D 정의

// [왜 존재] WorldSubsystem 시작 훅. 풀 RT들을 게임 시작 시점에 미리 만들어
//           런타임 도중의 RT 생성 히치(끊김)를 없앤다.
// [언제/누가] 월드 생성 시 엔진이 자동 호출. 직접 부르지 않음.
void UPortalRTPool::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);   // 부모 초기화 먼저 — override 시 필수 관례
    InitializePool();                // RT K개 즉시 생성 (선할당)
}

// [왜 존재] 월드 종료 시 풀이 들고 있던 모든 상태를 비우는 정리 훅.
// [언제/누가] 월드 파괴 시 엔진이 자동 호출.
void UPortalRTPool::Deinitialize()
{
    // Empty = 배열/맵의 모든 원소 제거.
    // PoolRTs를 비우면 UPROPERTY strong ref가 사라져 RT들이 GC 대상이 됨.
    PoolRTs.Empty();
    CurrentSlotOwners.Empty();
    PreviousSlotOwners.Empty();
    PendingPriorities.Empty();
    Super::Deinitialize();
}

// [왜 존재] 공유 Hot RT K개를 실제로 만드는 함수. 풀의 "선할당" 부분.
// [언제/누가] Initialize에서 1회 호출. (이후 다시 불려도 가드 때문에 no-op.)
void UPortalRTPool::InitializePool()
{
    // [가드] 이미 풀이 채워져 있으면 중복 생성 방지 (멱등성 보장)
    if (PoolRTs.Num() > 0) return;

    // Reserve = 배열의 내부 메모리만 미리 K칸 확보 (원소는 아직 0개) → 재할당 방지
    PoolRTs.Reserve(PoolSize);
    // Init(값, 개수) = 배열을 "nullptr K개"로 채움 → 모든 슬롯이 '비어 있음' 상태로 시작
    CurrentSlotOwners.Init(nullptr, PoolSize);
    PreviousSlotOwners.Init(nullptr, PoolSize);

    // RT를 K개 생성 — 이 K개가 곧 VRAM 사용 상한 (포탈이 100개여도 RT는 K개뿐)
    for (int32 i = 0; i < PoolSize; i++)
    {
        // NewObject<T>(Outer) = UObject 동적 생성. Outer를 this(서브시스템)로 주면
        // 소유 관계가 잡혀서 서브시스템 수명과 함께 관리됨.
        UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this);
        // InitAutoFormat = 해상도를 주면 픽셀 포맷을 엔진이 자동 선택해
        //                  GPU 리소스까지 생성하는 초기화 함수.
        RT->InitAutoFormat(HotRTWidth, HotRTHeight);
        // 밉맵(축소본 체인) 자동 생성 끔 — 포탈 화면은 원본 해상도로만 쓰므로
        // 밉맵 생성 비용/메모리 낭비를 막음.
        RT->bAutoGenerateMips = false;
        // RGBA16f = 채널당 16비트 부동소수점(HDR 가능), 픽셀당 8바이트.
        // (주의: InitAutoFormat "이후"에 포맷을 바꾸고 UpdateResource를 안 부르면
        //  GPU 리소스에 반영 안 될 수 있음 — 파일 하단 "특이사항" 보고 참고.)
        RT->RenderTargetFormat = RTF_RGBA16f;
        PoolRTs.Add(RT);
    }

    // 생성 결과 로그 (슬롯 수, 해상도, 이론상 총 VRAM)
    UE_LOG(LogTemp, Warning,
        TEXT("[RTPool] Initialized: %d slots @ %dx%d (%.1f MB total)"),
        PoolSize, HotRTWidth, HotRTHeight, GetTotalPoolMemoryMB());
}

// [왜 존재] 포탈과 풀 사이의 유일한 프레임 단위 접점.
//           "우선순위 신고"와 "슬롯 결과 조회"를 한 번의 호출로 처리한다.
// [언제/누가] 모든 활성 PortalActor가 매 프레임 Tick에서 호출.
// [반환] Hot 슬롯을 받았으면 그 슬롯의 공유 RT, 못 받았으면 nullptr(=Cold).
UTextureRenderTarget2D* UPortalRTPool::RequestHotRT(APortalActor* Portal, float Priority)
{
    // [가드] null 포탈이 장부(PendingPriorities)에 끼는 것을 차단
    if (!Portal) return nullptr;

    // GFrameCounter = 엔진 전역 프레임 번호 (매 프레임 1씩 증가하는 uint64).
    // "이번 프레임에 재할당을 이미 했나?"를 이 번호 비교로 판단 →
    // 포탈이 몇 개든 재할당은 프레임당 정확히 1번만 실행된다.
    const uint64 CurFrame = GFrameCounter;
    if (CurFrame != LastReallocFrame)
    {
        // 프레임 진입 — 직전 프레임 우선순위로 슬롯 재할당
        // (이 블록은 그 프레임의 "첫 번째로 호출한 포탈"이 대표로 실행)
        PreviousSlotOwners = CurrentSlotOwners;  // 현 점유 상태를 백업 → Hot→Cold 전환 감지 근거
        ReallocateSlots();                       // 지난 프레임에 모인 우선순위로 새 주인 결정
        LastReallocFrame = CurFrame;             // "이번 프레임 재할당 완료" 도장
        PendingPriorities.Reset();               // 장부 초기화 — 이번 프레임 신고를 새로 받기 시작
        // (Reset = Empty와 같지만 내부 메모리는 유지 → 매 프레임 재할당 비용 절약)
    }

    // 이번 프레임 우선순위 기록 (다음 프레임 할당에 사용)
    // FindOrAdd = 키가 있으면 기존 값 참조를, 없으면 새 엔트리를 만들어 참조 반환
    //             → 같은 포탈이 두 번 불러도 마지막 값으로 덮어써짐.
    PendingPriorities.FindOrAdd(Portal) = Priority;

    // 이번 프레임 슬롯 점유 여부 확인
    // TArray::Find = 값을 선형 검색해 인덱스 반환, 없으면 INDEX_NONE(-1).
    // 슬롯 인덱스 == RT 인덱스이므로 그대로 PoolRTs 조회에 사용.
    const int32 SlotIdx = CurrentSlotOwners.Find(Portal);
    if (SlotIdx != INDEX_NONE)
    {
        return PoolRTs[SlotIdx];   // Hot: 이 RT에 SceneCapture 하면 됨
    }
    return nullptr;                // Cold: 이번 프레임은 자기 ColdRT(이전 결과)로 버티기
}

// [왜 존재] 포탈이 "방금 Hot에서 밀려났는지"를 알아야 마지막 Hot 캡처본을
//           ColdRT로 옮겨 둘 수 있다. 그 전환 감지용 조회 함수.
// [언제/누가] PortalActor가 RequestHotRT에서 null을 받은 직후 호출.
bool UPortalRTPool::WasHotLastFrame(APortalActor* Portal) const
{
    // Contains = 배열 선형 검색으로 존재 여부만 bool 반환.
    // 직전 프레임 점유 백업(PreviousSlotOwners)에 있으면 "지난 프레임엔 Hot이었다".
    return PreviousSlotOwners.Contains(Portal);
}

// [왜 존재] "누가 Hot이 될 것인가"를 결정하는 정책 함수. 지난 프레임에
//           신고된 우선순위를 정렬해 상위 K개에게만 슬롯을 준다.
// [언제/누가] RequestHotRT가 프레임당 1번, 프레임의 첫 호출 시점에 내부 호출.
void UPortalRTPool::ReallocateSlots()
{
    // [가드] 우선순위 정보 없으면 슬롯 비움
    // (지난 프레임에 아무 포탈도 RequestHotRT를 안 불렀다는 뜻 —
    //  죽은 포탈이 슬롯을 계속 쥐고 있지 않도록 전부 nullptr로 리셋)
    if (PendingPriorities.Num() == 0)
    {
        for (int32 i = 0; i < CurrentSlotOwners.Num(); i++)
            CurrentSlotOwners[i] = nullptr;
        return;
    }

    // 유효한 포탈만 추려서 우선순위 내림차순 정렬
    // TPair = (키, 값) 묶음. TMap은 정렬이 안 되므로 배열로 옮겨서 정렬한다.
    TArray<TPair<APortalActor*, float>> Sorted;
    Sorted.Reserve(PendingPriorities.Num());   // 미리 칸 확보 (재할당 방지)
    for (auto& Pair : PendingPriorities)
    {
        // IsValid = 파괴 대기 중(PendingKill)인 포탈을 걸러냄 —
        // 신고 후 같은 프레임에 파괴된 포탈이 슬롯을 받는 사고 방지.
        if (IsValid(Pair.Key))
            Sorted.Add(Pair);
    }
    // Sort + 람다 비교자: A.Value > B.Value → 우선순위 "큰" 것이 앞으로 (내림차순)
    Sorted.Sort([](const auto& A, const auto& B) { return A.Value > B.Value; });

    // 상위 PoolSize개에 슬롯 할당 (LRU + Priority 결합)
    // 정렬 결과의 i번째 포탈에게 슬롯 i를 배정.
    // 후보가 슬롯보다 적으면 남는 슬롯은 nullptr(빈 슬롯)로 둔다.
    // (참고: 같은 포탈이 매 프레임 같은 순위면 같은 슬롯을 유지하지만,
    //  순위가 바뀌면 "슬롯 번호"가 바뀔 수 있음 → RT 내용도 다른 슬롯 것으로 바뀜.
    //  같은 레벨 벤치마크에선 무해하지만 다른 레벨 혼용 시 화면이 섞이는 원인.)
    for (int32 i = 0; i < CurrentSlotOwners.Num(); i++)
    {
        CurrentSlotOwners[i] = (i < Sorted.Num()) ? Sorted[i].Key : nullptr;
    }
}

// [왜 존재] 슬롯 소유자 배열들은 GC 추적이 안 되는 raw 포인터라서,
//           포탈이 파괴될 때 직접 흔적을 지워야 dangling 포인터를 막는다.
// [언제/누가] PortalActor::EndPlay에서 호출.
void UPortalRTPool::UnregisterPortal(APortalActor* Portal)
{
    // 1) 우선순위 장부에서 제거 — 다음 재할당 후보에서 빠짐
    //    (TMap::Remove = 키가 있으면 삭제, 없으면 아무 일 없음)
    PendingPriorities.Remove(Portal);
    // 2) 현재 슬롯 점유에서 제거 — 빈 슬롯(nullptr)으로 되돌림
    for (int32 i = 0; i < CurrentSlotOwners.Num(); i++)
    {
        if (CurrentSlotOwners[i] == Portal) CurrentSlotOwners[i] = nullptr;
    }
    // PreviousSlotOwners는 별도 배열이므로 인덱스 범위를 따로 검사 (out-of-bounds 방어)
    // 3) 직전 프레임 백업에서도 제거 — WasHotLastFrame이 죽은 포탈로
    //    true를 돌려주는 일이 없도록.
    for (int32 i = 0; i < PreviousSlotOwners.Num(); i++)
    {
        if (PreviousSlotOwners[i] == Portal) PreviousSlotOwners[i] = nullptr;
    }
}

// [왜 존재] "지금 Hot 슬롯이 몇 개나 차 있나" 디버그/HUD 표시용 카운터.
// [언제/누가] 디버그 HUD·벤치마크 코드가 필요할 때 호출. 상태 변경 없음(const).
int32 UPortalRTPool::GetActivePortalCount() const
{
    // nullptr가 아닌 슬롯(=점유 중)만 센다
    int32 Count = 0;
    for (APortalActor* P : CurrentSlotOwners) if (P) Count++;
    return Count;
}

// [왜 존재] 풀이 차지하는 이론상 VRAM을 MB로 보여주는 계산기 (로그/논문 수치용).
// [언제/누가] InitializePool 로그와 디버그 HUD에서 호출.
float UPortalRTPool::GetTotalPoolMemoryMB() const
{
    // RGBA16f = 8 bytes per pixel
    // (채널 4개 x 16비트 = 64비트 = 8바이트. 1920x1080이면 RT 1장 ≈ 15.8MB)
    const float BytesPerRT = HotRTWidth * HotRTHeight * 8.0f;
    // 총 바이트 → MB 변환 (1MB = 1024*1024 바이트). K=4면 약 63MB 고정.
    return (PoolSize * BytesPerRT) / (1024.0f * 1024.0f);
}
