#include "PortalViewExtension.h"
#include "RenderGraphBuilder.h"
#include "HAL/IConsoleManager.h"
#include "RendererInterface.h"
#include "SceneView.h"

// 벤치마크용 토글 — 콘솔에서 r.Portal.FrustumCulling 0/1 로 켜고 끄기
static TAutoConsoleVariable<int32> CVarPortalFrustumCulling(
    TEXT("r.Portal.FrustumCulling"),
    1,
    TEXT("0: Portal Frustum Culling 비활성화 (Baseline)\n1: Portal Frustum Culling 활성화 (기본값)"),
    ECVF_RenderThreadSafe
);

FPortalViewExtension::FPortalViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

FPortalViewExtension::~FPortalViewExtension()
{
    extern RENDERER_API float GPortalFrustumMaxDistance;
    extern RENDERER_API int32 GPortalFrustumPlaneCount;
    GPortalFrustumMaxDistance = 0.0f;
    GPortalFrustumPlaneCount = 0;
}

void FPortalViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FPortalViewExtension::BuildPortalConvexVolume(
    const FVector& EyePos,
    const FVector Corners[4],
    FConvexVolume& OutVolume)
{
    OutVolume.Planes.Reset();

    FVector Center = (Corners[0] + Corners[1] + Corners[2] + Corners[3]) / 4.0f;

    for (int32 i = 0; i < 4; i++)
    {
        const FVector& A = Corners[i];
        const FVector& B = Corners[(i + 1) % 4];

        FVector EdgeA = (A - EyePos).GetSafeNormal();
        FVector EdgeB = (B - EyePos).GetSafeNormal();
        FVector Normal = FVector::CrossProduct(EdgeA, EdgeB).GetSafeNormal();

        FVector ToCenter = (Center - EyePos).GetSafeNormal();
        if (FVector::DotProduct(Normal, ToCenter) < 0.0f)
            Normal = -Normal;

        OutVolume.Planes.Add(FPlane(A, Normal));
    }

    FVector PortalForward = (Center - EyePos).GetSafeNormal();
    OutVolume.Planes.Add(FPlane(EyePos + PortalForward * 10.0f, -PortalForward));
    OutVolume.Init();
}

float FPortalViewExtension::ComputeOptimalLumenDistance(
    const FConvexVolume& PortalVolume,
    const FVector& EyePos)
{
    float MaxDist = 500.0f;

    for (const FBoxSphereBounds& Bounds : SceneActorBounds)
    {
        if (PortalVolume.IntersectSphere(Bounds.Origin, Bounds.SphereRadius))
        {
            float Dist = Bounds.SphereRadius * 2.0f;
            MaxDist = FMath::Max(MaxDist, Dist);
        }
    }

    return FMath::Clamp(MaxDist, 300.0f, 1500.0f);
}

void FPortalViewExtension::PreRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{
    extern RENDERER_API float GPortalFrustumMaxDistance;
    extern RENDERER_API FPlane GPortalFrustumPlanes[5];
    extern RENDERER_API int32 GPortalFrustumPlaneCount;

    if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0]->bIsSceneCapture)
    {
        // SceneCapture 렌더링 중에는 프러스텀 컬링 반드시 비활성화
        // 메인 카메라가 설정한 플레인이 SceneCapture 렌더링에도 영향을 주기 때문
        // return만 하면 이전 프레임 플레인이 그대로 남아 타겟 레벨 오브젝트 전부 컬링됨
        GPortalFrustumPlaneCount = 0;
        GPortalFrustumMaxDistance = 0.0f;
        return;
    }

    FScopeLock Lock(&DataLock);

    // 벤치마크 토글: r.Portal.FrustumCulling 0 이면 Culling 비활성화
    if (CVarPortalFrustumCulling.GetValueOnRenderThread() == 0)
    {
        GPortalFrustumMaxDistance = 0.0f;
        GPortalFrustumPlaneCount = 0;
        return;
    }

    if (PortalFrustum.bIsValid)
    {
        FConvexVolume PortalVolume;
        BuildPortalConvexVolume(PortalFrustum.EyePosition, PortalFrustum.Corners, PortalVolume);

        // ��� �����͸� ���� ������ ����
        int32 PlaneCount = FMath::Min(PortalVolume.Planes.Num(), 5);
        for (int32 i = 0; i < PlaneCount; i++)
        {
            GPortalFrustumPlanes[i] = PortalVolume.Planes[i];
        }
        GPortalFrustumPlaneCount = PlaneCount;

        // �Ÿ� ���ѵ� �Բ� ����
        if (SceneActorBounds.Num() > 0)
        {
            float OptimalDistance = ComputeOptimalLumenDistance(PortalVolume, PortalFrustum.EyePosition);
            GPortalFrustumMaxDistance = OptimalDistance;
        }
        else
        {
            GPortalFrustumMaxDistance = 1500.0f;
        }

        // (디버그 로그 제거: 매 렌더 프레임 호출돼 로그가 도배되고 렌더 스레드 부담)
    }
    else
    {
        GPortalFrustumMaxDistance = 0.0f;
        GPortalFrustumPlaneCount = 0;
    }
}

void FPortalViewExtension::PostRenderBasePassDeferred_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneView& InView,
    const FRenderTargetBindingSlots& RenderTargets,
    TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
    if (InView.bIsSceneCapture) return;
}

void FPortalViewExtension::UpdatePortalData(const TArray<FBox>& InPortalBounds)
{
    FScopeLock Lock(&DataLock);
    PortalVisibleBounds = InPortalBounds;
}

void FPortalViewExtension::UpdatePortalFrustum(const FPortalFrustumData& InFrustumData)
{
    FScopeLock Lock(&DataLock);
    PortalFrustum = InFrustumData;
}

void FPortalViewExtension::UpdateSceneActorBounds(const TArray<FBoxSphereBounds>& InBounds)
{
    FScopeLock Lock(&DataLock);
    SceneActorBounds = InBounds;
}