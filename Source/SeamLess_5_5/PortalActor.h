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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal|Stereo")
    USceneCaptureComponent2D* SceneCaptureLeft;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal|Stereo")
    USceneCaptureComponent2D* SceneCaptureRight;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Portal")
    UBoxComponent* TriggerVolume;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    UMaterialInterface* PortalMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal")
    APortalActor* LinkedPortal;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Stereo")
    UTextureRenderTarget2D* RenderTargetLeft;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Stereo")
    UTextureRenderTarget2D* RenderTargetRight;

    // IPD (동공간 거리 cm, 기본값 6.4cm)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Portal|Stereo")
    float IPD = 6.4f;

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

private:
    void UpdateSceneCapture();
    void SetupCaptureComponent(USceneCaptureComponent2D* Capture);
    FTransform GetPortalCameraTransform(const FVector& CameraLocation, const FRotator& CameraRotation);

    UPROPERTY()
    UMaterialInstanceDynamic* DynamicMaterial;
};