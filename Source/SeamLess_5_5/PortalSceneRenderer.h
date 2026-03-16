//#pragma once
//
//#include "CoreMinimal.h"
//#include "SceneView.h"
//#include "DeferredShadingRenderer.h"
//
//struct FPortalInfo
//{
//    FMatrix PortalTransform;
//    FMatrix LinkedTransform;
//    FConvexVolume PortalFrustum;
//    FBox PortalVisibleBounds;
//    TWeakObjectPtr<class APortalActor> PortalActor;
//};
//
//class FPortalSceneRenderer : public FDeferredShadingSceneRenderer
//{
//public:
//    FPortalSceneRenderer(
//        const FSceneViewFamily* InViewFamily,
//        FHitProxyConsumer* HitProxyConsumer
//    );
//
//    virtual ~FPortalSceneRenderer() {}
//
//    virtual void Render(FRDGBuilder& GraphBuilder) override;
//
//private:
//    void ComputePortalFrustums();
//    void RenderPortalView(FRDGBuilder& GraphBuilder, const FPortalInfo& Portal);
//
//    TArray<FPortalInfo> ActivePortals;
//};