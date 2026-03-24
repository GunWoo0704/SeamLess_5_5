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

    // 타겟 레벨 로드 기준 위치/회전
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FTransform TargetViewTransform;

    // 포탈 안에서 SceneCapture가 찍을 위치 (카메라 눈높이)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Target")
    FVector TargetCaptureLocation;

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

    UPROPERTY()
    UMaterialInstanceDynamic* DynamicMaterial;

    UPROPERTY()
    ULevelStreamingDynamic* StreamingLevel = nullptr;

    TArray<AActor*> StreamingLevelActors;

    TSharedPtr<FPortalViewExtension, ESPMode::ThreadSafe> ViewExtension;

    TArray<FBoxSphereBounds> CachedActorBounds;
    float ActorBoundsCacheTimer = 0.0f;
    static constexpr float ActorBoundsCacheInterval = 0.5f;
};