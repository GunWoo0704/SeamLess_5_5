//#include "PortalSceneRenderer.h"
//#include "PortalActor.h"
//#include "RenderGraphBuilder.h"
//#include "Engine/World.h"
//#include "EngineUtils.h"
//
//FPortalSceneRenderer::FPortalSceneRenderer(
//    const FSceneViewFamily* InViewFamily,
//    FHitProxyConsumer* HitProxyConsumer
//)
//    : FDeferredShadingSceneRenderer(InViewFamily, HitProxyConsumer)
//{
//}
//
//void FPortalSceneRenderer::Render(FRDGBuilder& GraphBuilder)
//{
//    // 1. Portal Frustum 계산
//    ComputePortalFrustums();
//
//    // 2. 기본 Deferred 렌더링 실행
//    FDeferredShadingSceneRenderer::Render(GraphBuilder);
//}
//
//void FPortalSceneRenderer::ComputePortalFrustums()
//{
//    ActivePortals.Reset();
//
//    // View 기준 카메라 위치
//    if (Views.Num() == 0) return;
//    const FViewInfo& View = Views[0];
//    FVector CameraPos = View.ViewMatrices.GetViewOrigin();
//
//    // 씬에서 APortalActor 수집
//    // 실제 씬 접근은 GameThread에서 해야 하므로
//    // 여기선 구조체만 준비, 데이터는 PortalActor에서 푸시 방식으로 받을 예정
//}
//
//void FPortalSceneRenderer::RenderPortalView(
//    FRDGBuilder& GraphBuilder,
//    const FPortalInfo& Portal
//)
//{
//    // TODO: Portal Frustum 기반 Lumen Surface Cache 범위 제한
//    // TODO: Binocular Cache Sharing
//}