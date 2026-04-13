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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    TSoftObjectPtr<UWorld> TargetLevel;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FTransform TargetViewTransform;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FVector TargetCaptureLocation;

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

    virtual void Tick(float DeltaTime) override;
    virtual void BeginPlay() override;

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
    void UpdateSceneCapture();
    void UpdatePortalFrustumData();
    void CacheSceneActorBounds();
    void LoadTargetLevel();
    void UpdateStreamingLevelBounds();
    void BindStencilMaterialToVolume();
    void CheckPortalCrossing(UCameraComponent* Camera);
    void ExecuteTeleport(APawn* Pawn);
    void DrawLumenDebug(UCameraComponent* Camera);

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

    TArray<AActor*> StreamingLevelActors;

    TSharedPtr<FPortalViewExtension, ESPMode::ThreadSafe> ViewExtension;

    TArray<FBoxSphereBounds> CachedActorBounds;
    float ActorBoundsCacheTimer = 0.0f;
    static constexpr float ActorBoundsCacheInterval = 0.5f;
};
