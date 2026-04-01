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
    GPortalFrustumMaxDistance = 0.0f;
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
    float MaxDist = 1000.0f;

    for (const FBoxSphereBounds& Bounds : SceneActorBounds)
    {
        if (PortalVolume.IntersectSphere(Bounds.Origin, Bounds.SphereRadius))
        {
            float Dist = FVector::Dist(EyePos, Bounds.Origin) + Bounds.SphereRadius;
            MaxDist = FMath::Max(MaxDist, Dist);
        }
    }

    return FMath::Clamp(MaxDist * 1.5f, 500.0f, 2000.0f);
}

void FPortalViewExtension::PreRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{
    if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0]->bIsSceneCapture)
        return;

    FScopeLock Lock(&DataLock);

    extern RENDERER_API float GPortalFrustumMaxDistance;

    static float LastOptimalDistance = -1.0f;

    if (PortalFrustum.bIsValid)
    {
        if (SceneActorBounds.Num() > 0)
        {
            FConvexVolume PortalVolume;
            BuildPortalConvexVolume(PortalFrustum.EyePosition, PortalFrustum.Corners, PortalVolume);
            float OptimalDistance = ComputeOptimalLumenDistance(PortalVolume, PortalFrustum.EyePosition);
            GPortalFrustumMaxDistance = OptimalDistance;
        }
        else
        {
            // 바운드 없으면 기본 제한값 적용
            GPortalFrustumMaxDistance = 1500.0f;
        }

        UE_LOG(LogTemp, Log, TEXT("PortalFrustum ACTIVE - GPortalFrustumMaxDistance: %f"), GPortalFrustumMaxDistance);
    }
    else if (PortalVisibleBounds.Num() > 0)
    {
        float MaxRadius = 0.0f;
        for (const FBox& Box : PortalVisibleBounds)
            MaxRadius = FMath::Max(MaxRadius, Box.GetExtent().Size());

        float OptimalDistance = FMath::Clamp(MaxRadius * 2.0f, 1000.0f, 8000.0f);
        GPortalFrustumMaxDistance = OptimalDistance;

        UE_LOG(LogTemp, Log, TEXT("PortalBounds ACTIVE - GPortalFrustumMaxDistance: %f"), GPortalFrustumMaxDistance);

        LastOptimalDistance = OptimalDistance;
    }
    else
    {
        GPortalFrustumMaxDistance = 0.0f;
        LastOptimalDistance = 0.0f;

        UE_LOG(LogTemp, Log, TEXT("Portal INACTIVE - GPortalFrustumMaxDistance: 0"));
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