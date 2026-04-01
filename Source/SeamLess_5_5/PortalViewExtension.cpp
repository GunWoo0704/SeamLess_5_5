#include "PortalViewExtension.h"
#include "RenderGraphBuilder.h"
#include "HAL/IConsoleManager.h"
#include "RendererInterface.h"
#include "SceneView.h"

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
    if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0]->bIsSceneCapture)
        return;

    FScopeLock Lock(&DataLock);

    extern RENDERER_API float GPortalFrustumMaxDistance;
    extern RENDERER_API FPlane GPortalFrustumPlanes[5];
    extern RENDERER_API int32 GPortalFrustumPlaneCount;

    if (PortalFrustum.bIsValid)
    {
        FConvexVolume PortalVolume;
        BuildPortalConvexVolume(PortalFrustum.EyePosition, PortalFrustum.Corners, PortalVolume);

        // 평면 데이터를 전역 변수에 전달
        int32 PlaneCount = FMath::Min(PortalVolume.Planes.Num(), 5);
        for (int32 i = 0; i < PlaneCount; i++)
        {
            GPortalFrustumPlanes[i] = PortalVolume.Planes[i];
        }
        GPortalFrustumPlaneCount = PlaneCount;

        // 거리 제한도 함께 설정
        if (SceneActorBounds.Num() > 0)
        {
            float OptimalDistance = ComputeOptimalLumenDistance(PortalVolume, PortalFrustum.EyePosition);
            GPortalFrustumMaxDistance = OptimalDistance;
        }
        else
        {
            GPortalFrustumMaxDistance = 1500.0f;
        }

        UE_LOG(LogTemp, Log, TEXT("PortalFrustum ACTIVE - Planes: %d, MaxDist: %f"),
            GPortalFrustumPlaneCount, GPortalFrustumMaxDistance);
    }
    else
    {
        GPortalFrustumMaxDistance = 0.0f;
        GPortalFrustumPlaneCount = 0;

        UE_LOG(LogTemp, Log, TEXT("Portal INACTIVE"));
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