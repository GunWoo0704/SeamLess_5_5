// ════════════════════════════════════════════════════════════════
// PortalLevelManager.h — Phase 0: 레벨 인스턴스 공유 매니저 (헤더)
//
// [이 파일의 역할]
//   여러 포탈이 "같은 TargetLevel"을 바라볼 때, 포탈마다
//   LoadLevelInstanceBySoftObjectPtr를 따로 부르면 동일 레벨이 N벌
//   로드되어 메모리가 O(N)으로 늘어난다. 이 서브시스템은 레벨을
//   딱 1벌만 로드하고 "참조 카운트(refcount)"로 공유시켜 O(1)로 만든다.
//
// [다른 파일과의 관계]
//   - PortalActor.cpp : 이 매니저의 소비자. 포탈 BeginPlay 시 AcquireLevel,
//                       EndPlay/파괴 시 ReleaseLevel을 호출한다.
//   - PortalRTPool    : Phase 3의 RT 메모리 공유 담당 (별개 축의 최적화).
//   - UWorldSubsystem 기반이므로 월드(맵)당 1개 인스턴스가 엔진에 의해
//     자동 생성/파괴된다. 직접 new 하지 않고 GetWorld()->GetSubsystem<>()로 얻음.
// ════════════════════════════════════════════════════════════════
#pragma once

#include "CoreMinimal.h"                      // UE 기본 타입(FString, TMap 등) 모음 헤더
#include "Subsystems/WorldSubsystem.h"        // UWorldSubsystem 부모 클래스 정의
#include "Engine/LevelStreamingDynamic.h"     // ULevelStreamingDynamic (런타임 레벨 스트리밍 객체)
#include "PortalLevelManager.generated.h"     // UHT(리플렉션 코드 생성기)가 만든 헤더 — 항상 마지막 include

/**
 * 로드된 레벨 1개의 캐시 엔트리.
 * USTRUCT로 reflect돼야 TMap<...,FLoadedLevelEntry>에 UPROPERTY를 붙여
 * GC로부터 Level 포인터를 지킬 수 있다.
 *
 * (단순 raw pointer를 쓰면 World가 다른 경로로 StreamingLevels를 정리할 때
 *  dangling pointer가 될 수 있어서 strong ref + UPROPERTY 구조가 안전.)
 */
USTRUCT()
struct FPortalLoadedLevelEntry
{
    GENERATED_BODY()  // UHT가 리플렉션용 보일러플레이트 코드를 여기에 삽입

    // 실제 로드된 레벨 스트리밍 객체.
    // TObjectPtr = UE5식 UObject 포인터 래퍼 (raw 포인터와 호환되지만
    // 에디터에서 접근 추적 가능). UPROPERTY가 붙어 있어 GC가 이 객체를
    // "참조되고 있음"으로 인식 → 멋대로 회수하지 않음.
    UPROPERTY()
    TObjectPtr<ULevelStreamingDynamic> Level = nullptr;

    // 이 레벨을 현재 몇 개의 포탈이 쓰고 있는지 세는 카운터.
    // Acquire마다 +1, Release마다 -1. 0이 되면 레벨 언로드.
    UPROPERTY()
    int32 RefCount = 0;
};

/**
 * 같은 TargetLevel을 가진 여러 포탈이 단 1번만 레벨을 로드하도록 캐시하는 WorldSubsystem.
 *
 * 교수님 피드백 — "한정된 메모리로 포탈을 여러 개 만들어도..." 의 핵심 구현체.
 *
 * 사용 흐름:
 *   1) 포탈 생성 시: AcquireLevel()  → refcount++ (필요하면 로드)
 *   2) 포탈 파괴 시: ReleaseLevel()  → refcount-- (0이 되면 unload)
 *
 * 기존 단순 캐시(GetOrLoadLevel)와 차이:
 *   - 참조 카운트로 마지막 포탈이 사라지면 자동 메모리 해제
 *   - 명시적 ReleaseLevel API로 PIE 종료/액터 파괴 시 정리 가능
 *   - 디버그 로그로 메모리 추적 (LOAD / REUSE / RELEASE / UNLOAD)
 */
UCLASS()
class SEAMLESS_5_5_API UPortalLevelManager : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    // ── WorldSubsystem 라이프사이클 ─────────────────────────
    // 월드가 내려갈 때(PIE 종료, 맵 전환 등) 엔진이 자동 호출.
    // 캐시에 남아 있는 모든 레벨을 강제 언로드해서 누수 방지 (cpp 참고).
    virtual void Deinitialize() override;

    // ── 신규 권장 API: refcount 기반 ─────────────────────────

    /**
     * 레벨을 참조하고 인스턴스를 반환.
     * 같은 레벨이 이미 로드돼 있으면 refcount++만 하고 캐시 반환.
     * 없으면 새로 로드한 뒤 refcount=1로 등록.
     *
     * @note Location/Rotation은 새로 로드할 때만 사용됨.
     *       이미 로드된 레벨을 공유할 경우 기존 위치/회전이 그대로 쓰임.
     */
    ULevelStreamingDynamic* AcquireLevel(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

    /**
     * 레벨 참조 해제. refcount==0이 되면 즉시 unload하고 캐시에서 제거.
     * 같은 레벨을 참조하던 마지막 포탈이 사라지는 시점에 메모리 회수.
     */
    void ReleaseLevel(const TSoftObjectPtr<UWorld>& TargetLevel);

    // ── 호환: 기존 코드 깨지지 않도록 유지 (refcount 1짜리 Acquire로 위임) ──
    UE_DEPRECATED(5.5, "Use AcquireLevel/ReleaseLevel for proper refcount memory management.")
    ULevelStreamingDynamic* GetOrLoadLevel(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

    // ── 디버그/모니터링 ─────────────────────────────────────
    // 현재 캐시에 들어 있는 "서로 다른 레벨"의 개수.
    // 포탈이 10개라도 모두 같은 레벨을 보면 1이어야 정상 (= 공유 성공 지표).
    int32 GetLoadedLevelCount() const { return LoadedLevels.Num(); }
    // 특정 레벨의 현재 refcount 조회 (벤치마크/테스트 검증용).
    int32 GetRefCount(const TSoftObjectPtr<UWorld>& TargetLevel) const;
    /** 모든 캐시된 레벨 강제 unload (PIE 종료 등에 사용) */
    void ForceUnloadAll();

private:
    /**
     * 캐시: 키(레벨 경로) → 엔트리(strong ref + refcount).
     * UPROPERTY로 reflect돼 있어 GC가 Level을 회수하지 않음.
     */
    UPROPERTY()
    TMap<FString, FPortalLoadedLevelEntry> LoadedLevels;
    // ↑ 키: 레벨 애셋의 소프트 경로 문자열 (예: "/Game/Maps/RoomA.RoomA")
    //   값: 위에서 정의한 엔트리 (레벨 포인터 + refcount)
    //   FString 키를 쓰는 이유: TSoftObjectPtr 자체보다 경로 문자열이
    //   비교/해시가 단순하고 로그 출력에도 그대로 쓸 수 있어서.

    /** 내부: 캐시 키 생성 일관성 보장 */
    // 모든 API가 이 함수 하나로 키를 만들기 때문에
    // "같은 레벨인데 키가 달라서 중복 로드"되는 사고를 막는다.
    static FString MakeKey(const TSoftObjectPtr<UWorld>& TargetLevel);

    /** 내부: 실제 LoadLevelInstanceBySoftObjectPtr 호출 */
    ULevelStreamingDynamic* DoLoad(
        const TSoftObjectPtr<UWorld>& TargetLevel,
        const FVector& Location,
        const FRotator& Rotation);

    /** 내부: 실제 SetIsRequestingUnloadAndRemoval 호출 */
    void DoUnload(ULevelStreamingDynamic* Level, const FString& Key);
};
