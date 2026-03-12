//// Source/YourProject/PortalSceneRenderer.h
//
//#pragma once
//
//#include "CoreMinimal.h"
//#include "SceneView.h"
//#include "DeferredShadingRenderer.h"
//
//// Portal 정보 구조체
//struct FPortalInfo
//{
//    FMatrix Transform;
//    FSceneViewStateInterface* ViewState;
//    TWeakObjectPtr<class APortalActor> PortalActor;
//};
//
//// Custom Renderer
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
//    // 메인 렌더링 오버라이드
//    virtual void Render(FRDGBuilder& GraphBuilder) override;
//
//    // Portal 렌더링
//    void RenderPortalView(
//        FRDGBuilder& GraphBuilder,
//        const FPortalInfo& Portal
//    );
//
//private:
//    TArray<FPortalInfo> ActivePortals;
//};