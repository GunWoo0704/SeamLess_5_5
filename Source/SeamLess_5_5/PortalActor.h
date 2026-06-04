#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/BoxComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Scene.h"
#include "Engine/LevelStreamingDynamic.h"
#include "SceneViewExtension.h"
#include "PortalViewExtension.h"
#include "PortalActor.generated.h"

class UCameraComponent;

UCLASS()
class SEAMLESS_5_5_API APortalActor : public AActor
{
    GENERATED_BODY()

public:
    APortalActor();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    USceneComponent* PortalRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    UStaticMeshComponent* PortalMesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    USceneCaptureComponent2D* SceneCapture;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    UBoxComponent* TriggerVolume;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    UMaterialInterface* PortalMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    APortalActor* LinkedPortal;

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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    TSoftObjectPtr<UWorld> TargetLevel;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FTransform TargetViewTransform;

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

    /** Phase 3: RT Memory Pool (r.Portal.Phase3) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnablePhase3 = true;

    /** Frustum Culling (r.Portal.FrustumCulling) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|기능 토글")
    bool bEnableFrustumCulling = true;

    /** 체크박스 값들을 콘솔 변수에 한꺼번에 반영 (BeginPlay / 에디터 변경 시 호출) */
    UFUNCTION(BlueprintCallable, Category = "Portal|기능 토글")
    void ApplyFeatureToggles();

    /** 이 포탈의 캡처 우선순위(urgency 기본값). 거리·화면면적·시선 기반.
     *  PortalScheduler가 프레임 예산 배분에 사용. 클수록 먼저 캡처. */
    float GetCapturePriority() const;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    virtual void Tick(float DeltaTime) override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
    UFUNCTION()
    void OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
        AActor* OtherActor,
        UPrimitiveComponent* OtherComp,
        int32 OtherBodyIndex,
        bool bFromSweep,
        const FHitResult& SweepResult);

    UFUNCTION()
    void OnTargetLevelLoaded();

private:
    /** 이름으로 콘솔 변수를 찾아 bOn(1/0)으로 설정하고, 상태를 화면/로그에 표시. */
    void ApplyCVar(const TCHAR* CVarName, bool bOn, const FString& DisplayLabel);

    void UpdateSceneCapture();
    void UpdatePortalFrustumData();
    void CacheSceneActorBounds();
    void LoadTargetLevel();
    void UpdateStreamingLevelBounds();
    void BindStencilMaterialToVolume();
    void CheckPortalCrossing(UCameraComponent* Camera);
    void ExecuteTeleport(APawn* Pawn);
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
    int32 LastDotSign = 0;

    // 디버그 레이가 이미 그려졌는지
    bool bDebugLinesDrawn = false;

    UPROPERTY()
    UMaterialInstanceDynamic* DynamicMaterial;

    UPROPERTY()
    UMaterialInstanceDynamic* StencilPPInstance = nullptr;

    UPROPERTY()
    ULevelStreamingDynamic* StreamingLevel = nullptr;

    /** 스트리밍 레벨 액터 캐시.
     *  반드시 UPROPERTY(GC 추적)여야 함 — raw 포인터로 두면 레벨 언로드/액터 파괴 시
     *  댕글링 포인터가 되어 GetComponentsBoundingBox 등에서 access violation 크래시 발생.
     *  UPROPERTY면 파괴된 액터 항목이 자동으로 null 처리되고 IsValid()로 안전하게 걸러짐. */
    UPROPERTY()
    TArray<TObjectPtr<AActor>> StreamingLevelActors;

    TSharedPtr<FPortalViewExtension, ESPMode::ThreadSafe> ViewExtension;

    TArray<FBoxSphereBounds> CachedActorBounds;
    float ActorBoundsCacheTimer = 0.0f;
    static constexpr float ActorBoundsCacheInterval = 0.5f;
};
