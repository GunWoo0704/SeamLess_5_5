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
    static IConsoleVariable* CVarLumenSceneViewDistance =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.ViewDistance"));
    static IConsoleVariable* CVarLumenDistantScene =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.DistantScene.Enable"));

    if (CVarLumenSceneViewDistance)
        CVarLumenSceneViewDistance->Set(10000.0f, ECVF_SetByCode);
    if (CVarLumenDistantScene)
        CVarLumenDistantScene->Set(1, ECVF_SetByCode);

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

    return FMath::Clamp(MaxDist * 1.5f, 1000.0f, 8000.0f);
}

void FPortalViewExtension::PreRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{
    if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0]->bIsSceneCapture)
        return;

    FScopeLock Lock(&DataLock);

    extern RENDERER_API float GPortalFrustumMaxDistance;

    static IConsoleVariable* CVarLumenSceneViewDistance =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.ViewDistance"));
    static IConsoleVariable* CVarLumenDistantScene =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.DistantScene.Enable"));

    static float LastOptimalDistance = -1.0f;
    static float LastDistantScene = -1.0f;

    if (PortalFrustum.bIsValid && SceneActorBounds.Num() > 0)
    {
        FConvexVolume PortalVolume;
        BuildPortalConvexVolume(PortalFrustum.EyePosition, PortalFrustum.Corners, PortalVolume);

        float OptimalDistance = ComputeOptimalLumenDistance(PortalVolume, PortalFrustum.EyePosition);
        GPortalFrustumMaxDistance = OptimalDistance;

        if (FMath::Abs(OptimalDistance - LastOptimalDistance) > 100.0f)
        {
            if (CVarLumenSceneViewDistance)
                CVarLumenSceneViewDistance->Set(OptimalDistance, ECVF_SetByCode);
            LastOptimalDistance = OptimalDistance;
        }

        if (LastDistantScene != 0.0f)
        {
            if (CVarLumenDistantScene)
                CVarLumenDistantScene->Set(0, ECVF_SetByCode);
            LastDistantScene = 0.0f;
        }
    }
    else if (PortalVisibleBounds.Num() > 0)
    {
        float MaxRadius = 0.0f;
        for (const FBox& Box : PortalVisibleBounds)
            MaxRadius = FMath::Max(MaxRadius, Box.GetExtent().Size());

        float OptimalDistance = FMath::Clamp(MaxRadius * 2.0f, 1000.0f, 8000.0f);
        GPortalFrustumMaxDistance = OptimalDistance;

        if (FMath::Abs(OptimalDistance - LastOptimalDistance) > 100.0f)
        {
            if (CVarLumenSceneViewDistance)
                CVarLumenSceneViewDistance->Set(OptimalDistance, ECVF_SetByCode);
            LastOptimalDistance = OptimalDistance;
        }

        if (LastDistantScene != 0.0f)
        {
            if (CVarLumenDistantScene)
                CVarLumenDistantScene->Set(0, ECVF_SetByCode);
            LastDistantScene = 0.0f;
        }
    }
    else
    {
        GPortalFrustumMaxDistance = 0.0f;

        if (LastOptimalDistance != 10000.0f)
        {
            if (CVarLumenSceneViewDistance)
                CVarLumenSceneViewDistance->Set(10000.0f, ECVF_SetByCode);
            LastOptimalDistance = 10000.0f;
        }

        if (LastDistantScene != 1.0f)
        {
            if (CVarLumenDistantScene)
                CVarLumenDistantScene->Set(1, ECVF_SetByCode);
            LastDistantScene = 1.0f;
        }
    }
}

void FPortalViewExtension::PostRenderBasePassDeferred_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneView& InView,
    const FRenderTargetBindingSlots& RenderTargets,
    TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
    if (InView.bIsSceneCapture) return;
    // 다음 단계: 스텐실=1 영역에 스트리밍 레벨 렌더 구현
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