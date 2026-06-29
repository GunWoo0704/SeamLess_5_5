// ═══════════════════════════════════════════════════════════════════════════
// PortalActor.h — VR 다중 포탈 시스템의 핵심 액터 (헤더)
//
// 이 파일의 역할:
//   레벨에 배치되는 "포탈" 액터 1개의 정의. 포탈은 평면 메시(PortalMesh) 위에
//   SceneCapture2D가 찍은 "포탈 너머 레벨"의 모습을 RenderTarget(RT) 텍스처로
//   표시한다. 즉, 포탈 = (카메라 1대 + 그 카메라 영상을 트는 스크린 1장).
//
// 다른 파일과의 관계 (누가 이 액터와 통신하는가):
//   - PortalScheduler.h  : Phase2 예산 스케줄러. 매 틱 ShouldCaptureThisFrame()으로
//                          "이번 프레임에 캡처해도 되는지"를 물어보고, 우선순위는
//                          이 클래스의 GetCapturePriority()를 읽어간다.
//   - PortalRTPool.h     : Phase3 RT 공유 풀. Hot 슬롯(고해상도 공유 RT)을 요청하고,
//                          못 받으면 자기 ColdRenderTarget(저해상도)로 버틴다.
//   - PortalLevelManager.h: 같은 TargetLevel을 보는 포탈들이 레벨 인스턴스 1벌을
//                          공유하도록 refcount 방식으로 Acquire/Release 관리.
//   - PortalViewExtension.h: 포탈 개구부 절두체(Frustum) 데이터를 렌더 스레드로
//                          넘겨, 포탈 뷰에서 개구부 밖 지오메트리를 컬링(컷)한다.
//
// 연구 맥락: Quest 3 VR에서 포탈 N개를 띄워도 프레임이 유지되도록,
//   Phase1(캡처 경량화) / Phase2(프레임 예산) / Phase3(RT 풀) /
//   FrustumCulling / 레벨 상주 관리 / 활성 레벨 게이팅을 단계적으로 적용한다.
// ═══════════════════════════════════════════════════════════════════════════

// #pragma once: 이 헤더가 한 번만 include되도록 보장 (중복 정의 방지, UE 표준)
#pragma once

#include "CoreMinimal.h"                          // UE 기본 타입(FVector, FString 등) 모음
#include "GameFramework/Actor.h"                  // 부모 클래스 AActor
#include "Components/StaticMeshComponent.h"       // 포탈 표면 메시(스크린)용
#include "Components/SceneCaptureComponent2D.h"   // 씬을 텍스처로 찍는 "가상 카메라"
#include "Components/BoxComponent.h"              // 텔레포트 감지용 박스 트리거
#include "Engine/TextureRenderTarget2D.h"         // 캡처 결과가 저장되는 GPU 텍스처(RT)
#include "Materials/MaterialInterface.h"          // 머티리얼 에셋의 공통 부모 타입
#include "Materials/MaterialInstanceDynamic.h"    // 런타임에 파라미터를 바꿀 수 있는 머티리얼 인스턴스(MID)
#include "Engine/Scene.h"                         // FPostProcessSettings 등 PP 설정 구조체
#include "Engine/LevelStreamingDynamic.h"         // 런타임 동적 레벨 스트리밍(LoadLevelInstance)
#include "SceneViewExtension.h"                   // 렌더 파이프라인에 끼어드는 확장 인터페이스
#include "PortalViewExtension.h"                  // 우리가 만든 포탈 절두체 컬링 확장
#include "PortalActor.generated.h"                // UHT(언리얼 헤더 툴)가 자동 생성하는 리플렉션 코드. 항상 마지막 include

// 전방 선언: 헤더에서는 포인터 타입으로만 쓰므로 전체 include 대신 이름만 알려줌 (컴파일 시간 절약)
class UCameraComponent;

// UCLASS(): 이 클래스를 UE 리플렉션 시스템에 등록 (에디터 배치/GC/블루프린트 노출 가능)
UCLASS()
class SEAMLESS_5_5_API APortalActor : public AActor   // SEAMLESS_5_5_API = DLL 외부 노출 매크로 (모듈 간 링크용)
{
    GENERATED_BODY()   // UHT가 생성한 보일러플레이트 코드를 이 자리에 삽입하는 매크로

public:
    // 생성자: 컴포넌트 생성과 기본값 설정 (게임 시작 전, CDO 생성 시에도 호출됨)
    APortalActor();

    // ===== 컴포넌트 구성 =====
    // UPROPERTY(...) = 이 포인터를 GC(가비지 컬렉터)가 추적 + 에디터 디테일 패널에 노출.
    // VisibleAnywhere = 에디터에서 보이지만 교체 불가, BlueprintReadOnly = BP에서 읽기만 가능.

    /** 포탈의 루트(기준점) 컴포넌트. 다른 컴포넌트들이 모두 여기에 붙는다. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    USceneComponent* PortalRoot;

    /** 포탈 "스크린" 역할의 평면 메시. 캡처된 RT 텍스처를 머티리얼로 표시. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    UStaticMeshComponent* PortalMesh;

    /** 포탈 너머 풍경을 찍는 가상 카메라. 결과는 TextureTarget(RT)에 기록됨. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    USceneCaptureComponent2D* SceneCapture;

    /** 플레이어 진입(텔레포트) 감지용 박스 콜리전. OnOverlapBegin이 바인딩됨. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    UBoxComponent* TriggerVolume;

    // EditAnywhere = 에디터에서 값 변경 가능, BlueprintReadWrite = BP에서 읽기/쓰기 가능.

    /** 포탈 표면에 RT를 표시할 머티리얼 (에디터에서 할당).
     *  BeginPlay에서 이걸 바탕으로 다이내믹 인스턴스(DynamicMaterial)를 만든다. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    UMaterialInterface* PortalMaterial;

    /** [모드 2] 같은 레벨 안의 다른 포탈과 짝지을 때 사용 (전통적 포탈 페어).
     *  TargetLevel이 비어 있을 때만 이 모드로 동작. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    APortalActor* LinkedPortal;

    /** 캡처 결과가 기록되는 렌더 타겟(GPU 텍스처). 비워두면 BeginPlay에서 1920x1080으로 자동 생성. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    UTextureRenderTarget2D* RenderTarget;

    /** Phase 3: Cold RT — 저해상도 fallback 텍스처.
     *  포탈이 Hot 슬롯 못 받았을 때 보여줄 마지막 캡처 백업. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Phase3")
    UTextureRenderTarget2D* ColdRenderTarget;

    /** Phase 3: Cold RT 해상도 (기본 256x144, 메모리 ~300KB) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Phase3")
    int32 ColdRTWidth = 256;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Phase3")
    int32 ColdRTHeight = 144;

    /** [모드 1] 포탈 너머로 보여줄 "다른 레벨" 에셋.
     *  TSoftObjectPtr = 경로만 들고 있는 약참조 — 에셋을 미리 메모리에 올리지 않고,
     *  필요할 때(LoadTargetLevel) 스트리밍으로 로드한다. IsNull()이면 LinkedPortal 모드. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    TSoftObjectPtr<UWorld> TargetLevel;

    /** 스트리밍 레벨을 월드 어디에/어떤 회전으로 스폰할지 (위치+회전+스케일 묶음).
     *  텔레포트 시 도착 회전으로도 사용됨. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FTransform TargetViewTransform;

    /** 스폰된 레벨 안에서 SceneCapture 카메라를 둘 위치 (에디터에서 직접 지정).
     *  텔레포트(ExecuteTeleport) 시 도착 지점으로도 사용됨. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FVector TargetCaptureLocation;

    /** 포탈 카메라(SceneCapture)의 회전 오프셋.
     *  플레이어 머리 회전 위에 합성되어 적용됨.
     *  ZeroRotator면 순수 플레이어 머리 회전만 사용(기본 동작).
     *  예) Yaw=90 → 플레이어가 정면을 봐도 카메라는 오른쪽을 보는 상태에서 시작. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FRotator TargetCaptureRotation = FRotator::ZeroRotator;

    /** 스트리밍 레벨(Downtown_Alley)을 스폰한 뒤 추가로 회전시킬 값.
     *  TargetViewTransform.Rotation 위에 덧붙여서 적용됨.
     *  레벨 안 건물/소품 전체가 이 값만큼 돌아감 (SceneCapture/시야는 영향 X). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FRotator TargetLevelRotation = FRotator::ZeroRotator;

    // ===== 디버그 =====

    /** true 시 SceneCapture 샘플 레이 + 포탈 프러스텀을 씬에 선으로 그림 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Debug")
    bool bDebugLumenRays = false;

    // ===== 스텐실 버퍼 포탈 마스킹 =====

    /** 스텐실 기반 포탈 마스킹용 Post Process 머티리얼 (에디터에서 할당) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Stencil")
    UMaterialInterface* StencilPostProcessMaterial;

    /** 포탈 메시의 Custom Stencil 값 (기본 1) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Stencil")
    int32 PortalStencilValue = 1;

    // ===== 디테일 창 기능 토글 (체크박스) =====
    // 디테일 패널에서 체크를 켜고/끄면 대응되는 r.Portal.* 콘솔 변수가 즉시 적용된다.
    // 체크 = 기능 ON(1), 체크 해제 = 기능 OFF(0).

    /** 포탈 전체 활성화 (r.Portal.Enable) — 끄면 벤치마크 Baseline */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnablePortal = true;

    /** Phase 1: SceneCapture2D 공식 최적화 (r.Portal.Phase1) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnablePhase1 = true;

    /** Phase 2: Frame Budget Allocator (r.Portal.Phase2) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnablePhase2 = true;

    /** Phase 3: RT Memory Pool (r.Portal.Phase3) — 기본 OFF.
     *  포탈마다 다른 레벨을 보면 공유 풀이 화면을 뒤섞으므로, 단일 레벨 공유 벤치마크에서만 켤 것. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnablePhase3 = false;

    /** Frustum Culling (r.Portal.FrustumCulling) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnableFrustumCulling = true;

    /** 체크박스 값들을 콘솔 변수에 한꺼번에 반영 (BeginPlay / 에디터 변경 시 호출) */
    UFUNCTION(BlueprintCallable, Category = "Portal|기능 토글")
    void ApplyFeatureToggles();

    /** 이 포탈의 캡처 우선순위(urgency 기본값). 거리·화면면적·시선 기반.
     *  PortalScheduler가 프레임 예산 배분에 사용. 클수록 먼저 캡처. */
    float GetCapturePriority() const;

// WITH_EDITOR: 에디터 빌드에서만 컴파일되는 블록 (패키징된 게임에는 포함 안 됨)
#if WITH_EDITOR
    /** 에디터 디테일 패널에서 프로퍼티(체크박스 등)가 바뀔 때 엔진이 호출.
     *  기능 토글 체크박스 변경을 즉시 CVar에 반영하기 위해 오버라이드. */
    virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    // AActor 라이프사이클 오버라이드 (엔진이 자동 호출)
    /** 매 프레임 1회 호출. 포탈의 모든 런타임 로직(캡처 판단/레벨 상주/우선순위)의 중심. */
    virtual void Tick(float DeltaTime) override;
    /** 게임 시작(또는 액터 스폰) 시 1회 호출. RT 생성, 캡처 설정, 스케줄러 등록 등 초기화. */
    virtual void BeginPlay() override;
    /** 게임 종료/액터 파괴 시 1회 호출. 스케줄러·RT풀·레벨 참조 정리(등록 해제). */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
    // UFUNCTION(): 델리게이트(이벤트)에 바인딩하려면 리플렉션 등록이 필수라서 붙임
    /** TriggerVolume에 무언가 겹치기 시작할 때 엔진이 호출하는 콜백.
     *  Pawn(플레이어)이고 앞면 진입이면 텔레포트 실행. */
    UFUNCTION()
    void OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
        AActor* OtherActor,
        UPrimitiveComponent* OtherComp,
        int32 OtherBodyIndex,
        bool bFromSweep,
        const FHitResult& SweepResult);

    /** TargetLevel 스트리밍 로드가 끝났을 때 호출되는 콜백 (OnLevelLoaded 델리게이트).
     *  레벨 액터 목록 캐싱 + Phase1 ShowOnlyActors 적용 + 라이트 정리를 수행. */
    UFUNCTION()
    void OnTargetLevelLoaded();

private:
    /** 이름으로 콘솔 변수를 찾아 bOn(1/0)으로 설정하고, 상태를 화면/로그에 표시. */
    void ApplyCVar(const TCHAR* CVarName, bool bOn, const FString& DisplayLabel);

    /** 플레이어 뷰 카메라 (스케줄러 캐시 경유 — 프레임당 1회만 실제 조회). */
    class UCameraComponent* GetViewCamera() const;

    /** [LinkedPortal 모드 전용] 플레이어 카메라 위치를 "이 포탈 기준 상대좌표"로 바꾼 뒤
     *  LinkedPortal 기준으로 다시 펼쳐서 SceneCapture를 거울처럼 배치. Tick에서 호출. */
    void UpdateSceneCapture();
    /** 포탈 개구부 4꼭짓점+눈 위치를 ViewExtension에 전달 (FrustumCulling 데이터 생산).
     *  매 Tick 호출 — 렌더 스레드가 이 데이터로 개구부 밖 지오메트리를 컷한다. */
    void UpdatePortalFrustumData();
    /** (선언만 있고 정의·호출이 없는 함수 — 현재 미사용) */
    void CacheSceneActorBounds();
    /** PortalLevelManager를 통해 TargetLevel을 (공유) 로드하고 완료 콜백을 건다. */
    void LoadTargetLevel();
    /** 거리 기반으로 TargetLevel을 동적 로드/언로드 (히스테리시스).
     *  가까우면 Acquire, 멀어지면 Release → 상주 레벨 수를 근처 몇 개로 제한. */
    void UpdateLevelResidency();
    /** TargetLevel 참조 해제(언로드). RenderTarget은 그대로 둬 마지막 캡처 유지. */
    void ReleaseTargetLevel();
    /** 레벨을 렌더+Tick 활성/비활성 토글 (상태 변화 시에만).
     *  비활성: 레벨 숨김(SetShouldBeVisible false) + 액터 Tick off → CPU·GPU 절감.
     *  포탈은 마지막 캡처(RenderTarget)를 계속 표시. */
    void SetLevelActive(bool bActive);

    /** M2: 회전 리프로젝션 — 캡처 시점 대비 머리 회전 델타를 UV 오프셋으로 머티리얼에 전달.
     *  갱신 안 한(stale) 포탈도 머리 회전에 맞춰 워프돼 끊김이 줄어듦.
     *  @param bCapturedThisFrame true면 최신 캡처라 오프셋 0 + 기준 회전 갱신. */
    void UpdateReprojection(class UCameraComponent* Camera, bool bCapturedThisFrame);
    /** 스트리밍 레벨 액터들의 바운딩 박스를 모아 ViewExtension에 전달.
     *  (주의: 현재 아무 데서도 호출되지 않음 — Tick 안에 같은 로직이 인라인으로 중복 존재) */
    void UpdateStreamingLevelBounds();
    /** 월드에 디렉셔널 라이트를 1개만 남기고 나머지(스트리밍 레벨 중복분 등)는 끈다.
     *  Lights 비용 절감 + 모든 뷰가 같은 태양을 공유하도록. 레벨 로드 후 호출. */
    void EnsureSingleDirectionalLight();
    /** 스텐실 PP 머티리얼을 레벨의 PostProcessVolume에 연결 (현재 BeginPlay에서 호출 주석 처리됨). */
    void BindStencilMaterialToVolume();
    /** 카메라가 포탈 평면을 앞→뒤로 넘었는지 매 틱 검사 (VR에서 Overlap이 잘 안 잡혀서 직접 감지). */
    void CheckPortalCrossing(UCameraComponent* Camera);
    /** 실제 순간이동 수행: TargetLevel 모드면 레벨 안 지정 위치로, LinkedPortal 모드면 상대좌표 변환으로. */
    void ExecuteTeleport(APawn* Pawn);
    /** 디버그 시각화: 캡처 샘플 레이/포탈 절두체/법선/텔레포트 반경을 선으로 그림 (bDebugLumenRays 시). */
    void DrawLumenDebug(UCameraComponent* Camera);

    // ===== Phase 1: SceneCapture2D 공식 최적화 =====
    /** ShowFlags / LOD / MaxViewDistance / PostProcess 한꺼번에 토글 */
    void ApplyPhase1ShowFlags(bool bEnable);
    /** PrimitiveRenderMode + ShowOnlyActors 토글 (StreamingLevelActors가 채워진 후 호출) */
    void ApplyPhase1ShowOnlyActors(bool bEnable);

    // CVar 토글 감지용 — 이전 프레임 상태 저장 (-1: 초기화 전)
    int32 LastPhase1State = -1;

    // ===== Phase 3: RT Memory Pool =====
    /** 포탈의 현재 우선순위 계산 (거리/화면비율 기반). 풀에 전달용. */
    float ComputePortalPriority() const;
    /** Hot→Cold 전환 시 마지막 ColdRT 캡처 (1회) */
    void DoFinalColdCapture();

    /** 직전 프레임 Hot 슬롯에 있던 RT (Hot→Cold 전환 감지용) */
    UPROPERTY()
    UTextureRenderTarget2D* LastHotRT = nullptr;

    // 포탈 평면 통과 감지용 — 이전 프레임 부호 저장
    // (+1=포탈 앞, -1=포탈 뒤, 0=멀어서 판정 안 함. +1→-1로 바뀌는 순간이 "통과")
    int32 LastDotSign = 0;

    // 디버그 레이가 이미 그려졌는지 (persistent 라인이라 매 프레임 다시 그리면 중복됨)
    bool bDebugLinesDrawn = false;

    /** PortalMaterial로 만든 다이내믹 인스턴스 — 런타임에 RT 텍스처 파라미터 교체용.
     *  UPROPERTY() 만 붙은 건 에디터 노출 없이 GC 추적만 하겠다는 뜻. */
    UPROPERTY()
    UMaterialInstanceDynamic* DynamicMaterial;

    /** 스텐실 PP 머티리얼의 다이내믹 인스턴스 (현재 스텐실 경로 비활성이라 사실상 미사용) */
    UPROPERTY()
    UMaterialInstanceDynamic* StencilPPInstance = nullptr;

    /** 동적으로 로드한 TargetLevel의 스트리밍 핸들. 로드 상태 조회/가시성 토글에 사용. */
    UPROPERTY()
    ULevelStreamingDynamic* StreamingLevel = nullptr;

    /** 이 포탈이 현재 TargetLevel 참조를 잡고 있는지 (중복 Acquire/Release 방지) */
    bool bLevelAcquired = false;

    /** 레벨이 현재 렌더+Tick 활성 상태인지 (SetLevelActive 토글 추적) */
    bool bLevelActive = true;

    /** M2: 마지막 캡처가 일어난 시점의 머리(카메라) 회전. 리프로젝션 델타 기준. */
    FRotator CaptureViewRotation = FRotator::ZeroRotator;

    /** 이 포탈이 유효한 캡처(내용)를 한 번이라도 가졌는지.
     *  false면 게이팅/예산을 무시하고 강제로 로드·활성·캡처해서 검은 포탈을 막는다. */
    bool bHasCapturedOnce = false;

    /** 스트리밍 레벨 액터 캐시.
     *  반드시 UPROPERTY(GC 추적)여야 함 — raw 포인터로 두면 레벨 언로드/액터 파괴 시
     *  댕글링 포인터가 되어 GetComponentsBoundingBox 등에서 access violation 크래시 발생.
     *  UPROPERTY면 파괴된 액터 항목이 자동으로 null 처리되고 IsValid()로 안전하게 걸러짐. */
    UPROPERTY()
    TArray<TObjectPtr<AActor>> StreamingLevelActors;

    /** 렌더 파이프라인 확장(절두체 컬링 데이터 전달 통로).
     *  TSharedPtr<..., ESPMode::ThreadSafe> = 게임/렌더 스레드 양쪽에서 안전하게
     *  참조 카운트를 다루는 공유 포인터 (UObject가 아니라서 UPROPERTY 불가). */
    TSharedPtr<FPortalViewExtension, ESPMode::ThreadSafe> ViewExtension;

    // 아래 3개는 액터 바운드 캐싱용으로 선언됐지만 현재 .cpp에서 사용처 없음 (미사용 잔재)
    TArray<FBoxSphereBounds> CachedActorBounds;
    float ActorBoundsCacheTimer = 0.0f;
    static constexpr float ActorBoundsCacheInterval = 0.5f;   // constexpr = 컴파일 타임 상수
};
