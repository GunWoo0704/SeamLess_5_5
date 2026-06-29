// ════════════════════════════════════════════════════════════════
// PortalLevelManager.cpp — Phase 0: 레벨 인스턴스 공유 매니저 (구현)
//
// [핵심 아이디어]
//   "레벨 경로 문자열 → (레벨 인스턴스, refcount)" 캐시(TMap) 하나로
//   같은 레벨의 중복 로드를 막는다.
//   - 포탈이 레벨을 원하면 AcquireLevel: 캐시에 있으면 refcount++만,
//     없으면 진짜 로드 후 refcount=1로 등록.
//   - 포탈이 사라지면 ReleaseLevel: refcount--, 0이 되면 언로드.
//
// [호출자]
//   PortalActor가 BeginPlay/EndPlay 시점에 호출한다.
//   Deinitialize는 엔진이 월드 종료 때 자동 호출.
// ════════════════════════════════════════════════════════════════
#include "PortalLevelManager.h"
#include "Engine/World.h"   // GetWorld() 반환 타입 UWorld의 완전한 정의가 필요해서 include

// ────────────────────────────────────────────────────────────────
// 키 헬퍼
// ────────────────────────────────────────────────────────────────

// [왜 존재] 캐시 TMap의 키를 만드는 유일한 통로. Acquire/Release/GetRefCount가
//           전부 이 함수를 거치므로 키 생성 방식이 어긋날 일이 없다.
// [언제 호출] 레벨을 키로 다루는 모든 내부 지점에서.
FString UPortalLevelManager::MakeKey(const TSoftObjectPtr<UWorld>& TargetLevel)
{
    // TSoftObjectPtr = "애셋의 경로만 들고 있는 지연 로딩 포인터"
    //   (실제 객체가 로드돼 있지 않아도 경로는 항상 알 수 있음).
    // ToSoftObjectPath() = 그 경로 객체(FSoftObjectPath)를 꺼냄.
    // ToString() = "/Game/Maps/RoomA.RoomA" 같은 문자열로 변환 → TMap 키로 사용.
    return TargetLevel.ToSoftObjectPath().ToString();
}

// ────────────────────────────────────────────────────────────────
// 신규 API: AcquireLevel / ReleaseLevel (refcount 기반)
// ────────────────────────────────────────────────────────────────

// [왜 존재] Phase 0의 심장. "레벨 N벌 로드 → 1벌 공유"를 실현하는 진입점.
// [언제/누가] PortalActor가 자기 TargetLevel을 표시해야 할 때(BeginPlay 등) 호출.
//             반환된 레벨 인스턴스를 포탈이 SceneCapture 등에 사용한다.
// [규약] 이 함수를 부른 쪽은 반드시 나중에 ReleaseLevel을 짝으로 불러야 함.
ULevelStreamingDynamic* UPortalLevelManager::AcquireLevel(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    // [가드] 에디터에서 TargetLevel을 안 채운 포탈을 거름.
    // IsNull() = 소프트 포인터에 경로 자체가 비어 있는지 검사
    //            (로드 여부와 무관 — "아예 지정 안 됨"만 true).
    if (TargetLevel.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("[PortalLevelMgr] AcquireLevel: TargetLevel is null"));
        return nullptr;
    }

    // 레벨 경로 문자열을 캐시 키로 사용
    const FString Key = MakeKey(TargetLevel);

    // 1) 이미 캐시에 있으면 refcount++ 후 반환
    // TMap::Find = 키가 있으면 값의 "포인터"를, 없으면 nullptr를 반환
    //              (포인터라서 Entry->RefCount++처럼 캐시 안의 원본을 직접 수정 가능).
    // if 안에 선언 = C++17 문법. Entry가 nullptr가 아닐 때만 블록 실행.
    if (FPortalLoadedLevelEntry* Entry = LoadedLevels.Find(Key))
    {
        // IsValid = UObject가 (a) nullptr 아니고 (b) PendingKill/GC 마킹이
        //           안 된 "살아있는" 상태인지 검사. 단순 null 체크보다 안전.
        if (IsValid(Entry->Level))
        {
            // 살아있는 캐시 적중 → 로드 없이 공유. 이게 메모리 O(1)의 핵심.
            Entry->RefCount++;
            UE_LOG(LogTemp, Warning,
                TEXT("[PortalLevelMgr] REUSE  refcount %d→%d  %s"),
                Entry->RefCount - 1, Entry->RefCount, *Key);
            return Entry->Level;
        }
        // 객체가 어떤 이유로든 dead면 캐시 정리하고 아래로 떨어져서 재로드
        // (주의: 이때 기존 RefCount 정보도 같이 버려짐. 죽은 레벨을 참조하던
        //  다른 포탈이 나중에 ReleaseLevel을 부르면 새 엔트리의 refcount를
        //  깎게 되는 시나리오가 이론상 가능 — "특이사항" 참고.)
        LoadedLevels.Remove(Key);
    }

    // 2) 새로 로드 — 캐시 미스(또는 dead 엔트리 정리 후)일 때만 실제 로드 발생
    ULevelStreamingDynamic* NewLevel = DoLoad(TargetLevel, Location, Rotation);
    if (NewLevel)
    {
        // 로드 성공 → refcount 1로 캐시에 등록 (지역 변수 NewEntry는
        // TMap::Add 시 복사되어 들어가므로 함수 끝나도 안전)
        FPortalLoadedLevelEntry NewEntry;
        NewEntry.Level = NewLevel;
        NewEntry.RefCount = 1;
        LoadedLevels.Add(Key, NewEntry);
        UE_LOG(LogTemp, Warning,
            TEXT("[PortalLevelMgr] LOAD   refcount 0→1  %s"), *Key);
    }
    // 로드 실패 시 NewLevel == nullptr 그대로 반환 (캐시에는 안 넣음)
    return NewLevel;
}

// [왜 존재] AcquireLevel의 짝. 포탈이 더 이상 레벨을 안 볼 때 refcount를
//           내려서, 마지막 사용자가 떠나는 순간 메모리를 회수하기 위함.
// [언제/누가] PortalActor의 EndPlay/Destroyed에서 호출.
void UPortalLevelManager::ReleaseLevel(const TSoftObjectPtr<UWorld>& TargetLevel)
{
    // [가드] 경로가 비어 있으면 애초에 Acquire도 안 됐을 것 → 조용히 무시
    if (TargetLevel.IsNull()) return;

    const FString Key = MakeKey(TargetLevel);

    // 캐시에서 엔트리 검색 (Find = 없으면 nullptr)
    FPortalLoadedLevelEntry* Entry = LoadedLevels.Find(Key);
    if (!Entry)
    {
        // [가드] Acquire 없이 Release가 불렸거나, 이미 unload된 키.
        // 짝이 안 맞는 호출을 잡아내기 위한 경고 로그 (crash 대신 경고만).
        UE_LOG(LogTemp, Warning,
            TEXT("[PortalLevelMgr] RELEASE called on unknown key  %s"), *Key);
        return;
    }

    // 참조 카운트 감소. 로그에서 "이전값→현재값"을 찍기 위해
    // 감소 후의 Entry->RefCount + 1 을 이전값으로 역산해서 출력.
    Entry->RefCount--;
    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] RELEASE refcount %d→%d  %s"),
        Entry->RefCount + 1, Entry->RefCount, *Key);

    // refcount 0 → 즉시 unload 및 캐시 제거
    // (<= 0 으로 검사: 과잉 Release로 음수가 돼도 안전하게 정리되도록 방어)
    if (Entry->RefCount <= 0)
    {
        // 주의: Remove를 부르면 Entry 포인터가 가리키던 메모리가 무효화되므로,
        // 레벨 포인터를 지역 변수 Lvl로 먼저 빼둔 뒤 사용한다.
        ULevelStreamingDynamic* Lvl = Entry->Level;
        DoUnload(Lvl, Key);          // 엔진에 언로드 요청 (실제 해제는 다음 tick)
        LoadedLevels.Remove(Key);    // 캐시에서도 제거 → 다음 Acquire는 새로 로드
        UE_LOG(LogTemp, Warning,
            TEXT("[PortalLevelMgr] UNLOAD (refcount=0)  %s"), *Key);
    }
}

// ────────────────────────────────────────────────────────────────
// 호환: 기존 GetOrLoadLevel — AcquireLevel로 위임
//   (콜러가 ReleaseLevel을 부르지 않으면 leak이지만,
//    기존 코드 깨지지 않도록 일단 유지)
// ────────────────────────────────────────────────────────────────

// [왜 존재] refcount 도입 이전의 옛 API. 옛 호출부가 컴파일 에러 없이
//           돌도록 남겨둔 어댑터일 뿐, 새 코드에서는 쓰면 안 된다.
// [언제/누가] 아직 마이그레이션 안 된 레거시 코드만 호출 (호출 시 경고 로그).
// PRAGMA_DISABLE_DEPRECATION_WARNINGS = 함수 자체가 UE_DEPRECATED라서
//   "자기 정의"에서 deprecation 경고가 나는 걸 이 구간만 꺼두는 매크로.
PRAGMA_DISABLE_DEPRECATION_WARNINGS
ULevelStreamingDynamic* UPortalLevelManager::GetOrLoadLevel(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] DEPRECATED: GetOrLoadLevel 호출. AcquireLevel/ReleaseLevel로 교체 권장."));
    // Acquire로 위임 → refcount가 1 올라가지만 짝이 되는 Release가 없으므로
    // 이 경로로 로드된 레벨은 ForceUnloadAll 전까지 해제되지 않음 (의도된 누수 허용).
    return AcquireLevel(TargetLevel, Location, Rotation);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

// ────────────────────────────────────────────────────────────────
// 라이프사이클 / 강제 정리
// ────────────────────────────────────────────────────────────────

// [왜 존재] 월드 종료 시 정리 훅. 포탈들이 Release를 못 부르고 죽는 경우
//           (PIE 강제 종료 등)에도 캐시가 누수되지 않게 전부 비운다.
// [언제/누가] 엔진이 WorldSubsystem 수명 종료 시점에 자동 호출. 직접 부르지 않음.
void UPortalLevelManager::Deinitialize()
{
    ForceUnloadAll();        // refcount 무시하고 캐시 전체 언로드
    Super::Deinitialize();   // 부모(UWorldSubsystem)의 기본 정리 — override 시 필수 관례
}

// [왜 존재] refcount와 무관하게 캐시된 레벨을 전부 언로드하는 비상 청소부.
// [언제/누가] Deinitialize가 자동으로 부르고, 디버그/테스트에서 수동 호출 가능.
void UPortalLevelManager::ForceUnloadAll()
{
    // [가드] 캐시가 비어 있으면 로그조차 안 찍고 끝 (조용한 no-op)
    if (LoadedLevels.Num() == 0) return;

    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] ForceUnloadAll: %d levels"), LoadedLevels.Num());

    // 캐시의 모든 엔트리를 순회하며 언로드 요청
    // (range-for의 Pair = TMap의 키/값 쌍. Pair.Key = 경로, Pair.Value = 엔트리)
    for (auto& Pair : LoadedLevels)
    {
        // 이미 죽은(GC된) 레벨은 건드리면 안 되므로 IsValid로 걸러냄
        if (IsValid(Pair.Value.Level))
        {
            DoUnload(Pair.Value.Level, Pair.Key);
        }
    }
    // Empty = TMap의 모든 원소 제거 → strong ref가 사라져 GC도 가능해짐
    LoadedLevels.Empty();
}

// [왜 존재] "지금 이 레벨을 몇 포탈이 공유 중인가"를 외부에서 확인하는 조회 함수.
// [언제/누가] 벤치마크 코드나 디버그 HUD가 공유 동작 검증용으로 호출. 상태 변경 없음(const).
int32 UPortalLevelManager::GetRefCount(const TSoftObjectPtr<UWorld>& TargetLevel) const
{
    // [가드] 경로가 비었으면 캐시에 있을 리 없음 → 0
    if (TargetLevel.IsNull()) return 0;
    // Find = 없으면 nullptr → 삼항 연산자로 "캐시 미스면 0" 처리
    const FPortalLoadedLevelEntry* Entry = LoadedLevels.Find(MakeKey(TargetLevel));
    return Entry ? Entry->RefCount : 0;
}

// ────────────────────────────────────────────────────────────────
// 내부 로드/언로드 헬퍼
// ────────────────────────────────────────────────────────────────

// [왜 존재] 엔진 로드 API를 한 곳에 모아둔 래퍼. 로드 실패 처리/로그를
//           AcquireLevel 본문에서 분리해 읽기 쉽게 한다.
// [언제/누가] AcquireLevel이 캐시 미스일 때만 호출 (= 진짜 메모리가 늘어나는 유일한 지점).
ULevelStreamingDynamic* UPortalLevelManager::DoLoad(
    const TSoftObjectPtr<UWorld>& TargetLevel,
    const FVector& Location,
    const FRotator& Rotation)
{
    // LoadLevelInstanceBySoftObjectPtr = UE가 제공하는 "레벨을 현재 월드에
    // 스트리밍 인스턴스로 끼워 넣기" 정적 함수.
    //   - GetWorld(): 어느 월드에 끼울지 (WorldSubsystem이라 자기 월드를 앎)
    //   - Location/Rotation: 인스턴스가 배치될 월드 좌표/회전 (레벨 전체가 이동됨)
    //   - bSuccess: 성공 여부를 out 파라미터로 돌려받음 (참조 전달)
    // 주의: 반환 직후 레벨이 "다 로드된" 게 아니라 비동기 스트리밍이 시작된 상태.
    bool bSuccess = false;
    ULevelStreamingDynamic* NewLevel = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(
        GetWorld(),
        TargetLevel,
        Location,
        Rotation,
        bSuccess);

    // [가드] 포인터와 성공 플래그를 둘 다 확인 — 경로 오타, 패키징 누락 등으로
    // 실패하면 Error 로그 남기고 nullptr (호출자 AcquireLevel은 캐시에 안 넣음).
    if (!NewLevel || !bSuccess)
    {
        UE_LOG(LogTemp, Error,
            TEXT("[PortalLevelMgr] LOAD FAILED  %s"),
            *TargetLevel.ToSoftObjectPath().ToString());
        return nullptr;
    }
    return NewLevel;
}

// [왜 존재] 언로드 방식을 한 곳에 통일하는 래퍼 (ReleaseLevel/ForceUnloadAll 공용).
// [언제/누가] refcount가 0이 됐을 때(ReleaseLevel), 또는 월드 종료 시(ForceUnloadAll).
void UPortalLevelManager::DoUnload(ULevelStreamingDynamic* Level, const FString& Key)
{
    // [가드] 이미 null이면 할 일 없음
    if (!Level) return;

    // SetIsRequestingUnloadAndRemoval → 다음 World tick에서 안전하게 언로드 + 제거
    // (즉시 파괴가 아니라 "요청 플래그"만 세움. 렌더링 중인 프레임을 건드리지
    //  않도록 엔진이 안전한 타이밍에 언로드 + StreamingLevels 목록에서 제거까지 수행.)
    Level->SetIsRequestingUnloadAndRemoval(true);
    UE_LOG(LogTemp, Warning,
        TEXT("[PortalLevelMgr] Requested unload  %s"), *Key);
}
