#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "ConvexVolume.h"

struct FPortalFrustumData
{
    FVector EyePosition;
    FVector Corners[4];
    FBox    PortalBounds;
    bool    bIsValid = false;
};

class SEAMLESS_5_5_API FPortalViewExtension : public FSceneViewExtensionBase
{
public:
    FPortalViewExtension(const FAutoRegister& AutoRegister);
    virtual ~FPortalViewExtension();

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override {}

    virtual void PreRenderViewFamily_RenderThread(
        FRDGBuilder& GraphBuilder,
        FSceneViewFamily& InViewFamily) override;

    virtual void PostRenderBasePassDeferred_RenderThread(
        FRDGBuilder& GraphBuilder,
        FSceneView& InView,
        const FRenderTargetBindingSlots& RenderTargets,
        TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

    void UpdatePortalData(const TArray<FBox>& InPortalBounds);
    void UpdatePortalFrustum(const FPortalFrustumData& InFrustumData);
    void UpdateSceneActorBounds(const TArray<FBoxSphereBounds>& InBounds);

private:
    void BuildPortalConvexVolume(
        const FVector& EyePos,
        const FVector Corners[4],
        FConvexVolume& OutVolume);

    float ComputeOptimalLumenDistance(
        const FConvexVolume& PortalVolume,
        const FVector& EyePos);

    TArray<FBox>             PortalVisibleBounds;
    TArray<FBoxSphereBounds> SceneActorBounds;
    FPortalFrustumData       PortalFrustum;
    FCriticalSection         DataLock;
};