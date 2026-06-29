// ════════════════════════════════════════════════════════════════
// PortalRTPool.h — Phase 3: RenderTarget(RT) 메모리 풀 (헤더)
//
// [이 파일의 역할]
//   포탈마다 1920x1080 RGBA16f RT를 독점시키면 RT VRAM이 포탈 수 N에
//   비례(O(N))해서 폭증한다. 이 서브시스템은 고해상도 "Hot" RT를
//   고정 K개(PoolSize=4)만 만들어 두고, 매 프레임 우선순위 상위 K개
//   포탈에게만 빌려준다 → VRAM이 N과 무관하게 O(K) 상수.
//
// [다른 파일과의 관계]
//   - PortalActor.cpp : 소비자. 매 프레임 RequestHotRT(자기, 우선순위)를
//     호출해 Hot RT를 받으면 SceneCapture 타깃으로 쓰고, null이면
//     자기 전용 저해상도 ColdRT의 "이전 캡처 결과"를 계속 표시.
//   - UPortalScheduler : 우선순위(Priority) 값을 계산하는 쪽.
//   - PortalLevelManager : Phase 0의 레벨 공유 (별개 축).
//
// [주의 — 기본 OFF인 이유]
//   서로 "다른 레벨"을 보는 포탈들이 같은 RT 슬롯을 번갈아 쓰면
//   화면 내용이 뒤섞이므로, r.Portal.Phase3 기본값 0.
//   같은 레벨을 보는 벤치마크 시나리오 전용 기능.
// ════════════════════════════════════════════════════════════════
#pragma once

#include "CoreMinimal.h"                // UE 기본 타입 모음
#include "Subsystems/WorldSubsystem.h"  // UWorldSubsystem 부모 클래스
#include "PortalRTPool.generated.h"     // UHT 생성 헤더 — 항상 마지막 include

// 전방 선언(forward declaration): 헤더에서는 포인터 타입만 쓰므로
// 무거운 헤더를 include하지 않고 "이런 클래스가 있다"고만 알려줌 → 컴파일 시간 절약.
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
    // WorldSubsystem 라이프사이클 — 엔진이 월드 생성/파괴 시 자동 호출.
    // Initialize에서 RT K개를 미리 생성(풀 채우기), Deinitialize에서 모두 해제.
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** 이번 프레임의 우선순위 등록 + Hot 슬롯 RT 반환.
     *  null 반환 = 이번 프레임 Hot 슬롯 못 받음 (Cold 상태). */
    // 핵심 API. 포탈이 매 프레임 Tick에서 호출한다.
    // 이번 프레임의 Priority를 "다음 프레임 할당용"으로 적립하고,
    // "직전 프레임 우선순위로 이미 정해진" 슬롯 배정 결과를 돌려준다
    // (즉 슬롯 배정은 항상 1프레임 지연된 우선순위 기준 — 한 프레임 안에서
    //  모든 포탈의 우선순위를 다 모은 뒤에야 비교가 가능하기 때문).
    UTextureRenderTarget2D* RequestHotRT(APortalActor* Portal, float Priority);

    /** 직전 프레임 이 포탈이 Hot이었는지 (Hot→Cold 전환 감지용) */
    // 포탈이 "방금 Hot에서 Cold로 떨어졌다"를 알아야 Hot RT의 마지막
    // 캡처 내용을 자기 ColdRT로 복사해 둘 수 있다. 그 판정에 사용.
    bool WasHotLastFrame(APortalActor* Portal) const;

    /** 포탈 등록 해제 (EndPlay에서 호출) */
    // 파괴되는 포탈의 흔적(우선순위/슬롯 점유)을 풀에서 지워
    // dangling 포인터가 다음 프레임 할당에 끼어드는 것을 방지.
    void UnregisterPortal(APortalActor* Portal);

    /** 디버그 통계 */
    int32 GetPoolSize() const { return PoolRTs.Num(); }   // 실제 생성된 RT 개수 (정상이면 PoolSize와 동일)
    int32 GetActivePortalCount() const;                   // 현재 Hot 슬롯을 점유 중인 포탈 수
    float GetTotalPoolMemoryMB() const;                   // 풀 전체 RT의 이론상 VRAM(MB)

    /** 풀 설정 (CVar로 토글 가능하게 할 수도 있음) */
    // static constexpr = 컴파일 타임 상수 (인스턴스마다가 아니라 클래스에 1개).
    static constexpr int32 PoolSize = 4;       // K: 동시 Hot 포탈 최대 수 = VRAM 상한 결정
    static constexpr int32 HotRTWidth = 1920;  // Hot RT 해상도 (가로)
    static constexpr int32 HotRTHeight = 1080; // Hot RT 해상도 (세로)

private:
    /** 공유 Hot RT 풀 (K개) */
    // UPROPERTY가 붙어 있어 GC가 RT들을 회수하지 않음 (strong ref).
    // 인덱스 i가 곧 "슬롯 번호" — CurrentSlotOwners[i]와 1:1 대응.
    UPROPERTY()
    TArray<UTextureRenderTarget2D*> PoolRTs;

    /** 현재 프레임에 각 슬롯을 점유한 포탈 */
    // CurrentSlotOwners[i] == P 이면 "포탈 P가 PoolRTs[i]를 쓰는 중", nullptr = 빈 슬롯.
    // 주의: UPROPERTY가 아닌 raw 포인터 배열 — GC 추적 대상이 아니므로
    // 포탈 파괴 시 UnregisterPortal로 반드시 직접 지워줘야 dangling을 막는다.
    TArray<APortalActor*> CurrentSlotOwners;

    /** 직전 프레임 슬롯 점유 (Hot→Cold 전환 감지) */
    // 매 프레임 재할당 직전에 CurrentSlotOwners를 통째로 복사해 보관.
    // WasHotLastFrame이 이 배열을 검색한다.
    TArray<APortalActor*> PreviousSlotOwners;

    /** 이번 프레임 누적된 우선순위 (다음 프레임 재할당용) */
    // 키: 포탈, 값: 그 포탈이 이번 프레임에 신고한 우선순위.
    // 프레임마다 Reset되고 다시 쌓인다 (1프레임짜리 임시 장부).
    TMap<APortalActor*, float> PendingPriorities;

    // 마지막으로 슬롯 재할당을 수행한 프레임 번호 (GFrameCounter 기준).
    // "프레임당 재할당 1회" 보장용. Max()로 초기화 → 첫 호출 시
    // 어떤 프레임 번호와도 다르므로 반드시 1번 재할당이 돈다.
    uint64 LastReallocFrame = TNumericLimits<uint64>::Max();

    void InitializePool();    // RT K개 생성 (Initialize에서 1회)
    void ReallocateSlots();   // 우선순위 상위 K개에 슬롯 배정 (프레임당 1회)
};
