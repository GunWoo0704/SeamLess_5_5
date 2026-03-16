#include "PortalViewExtension.h"
#include "RenderGraphBuilder.h"
#include "HAL/IConsoleManager.h"

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
}

void FPortalViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
    // 비워둠
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

    // Near 평면
    FVector PortalForward = (Center - EyePos).GetSafeNormal();
    OutVolume.Planes.Add(FPlane(EyePos + PortalForward * 10.0f, -PortalForward));

    OutVolume.Init();
}

float FPortalViewExtension::ComputeOptimalLumenDistance(
    const FConvexVolume& PortalVolume,
    const FVector& EyePos)
{
    float MaxDist = 1000.0f;
    int32 InFrustum = 0;

    for (const FBoxSphereBounds& Bounds : SceneActorBounds)
    {
        // Portal Frustum과 교차하는 액터만 처리 (핵심 컬링)
        if (PortalVolume.IntersectSphere(Bounds.Origin, Bounds.SphereRadius))
        {
            InFrustum++;
            float Dist = FVector::Dist(EyePos, Bounds.Origin) + Bounds.SphereRadius;
            MaxDist = FMath::Max(MaxDist, Dist);
        }
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("Portal Frustum Culling: %d/%d actors in frustum, MaxDist=%.1f"),
        InFrustum, SceneActorBounds.Num(), MaxDist);

    return FMath::Clamp(MaxDist * 1.5f, 1000.0f, 8000.0f);
}

void FPortalViewExtension::PreRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{


    if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0]->bIsSceneCapture)
        return;

    FScopeLock Lock(&DataLock);

    static IConsoleVariable* CVarLumenSceneViewDistance =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.ViewDistance"));
    static IConsoleVariable* CVarLumenDistantScene =
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.DistantScene.Enable"));

    if (PortalFrustum.bIsValid && SceneActorBounds.Num() > 0)
    {
        FConvexVolume PortalVolume;
        BuildPortalConvexVolume(
            PortalFrustum.EyePosition,
            PortalFrustum.Corners,
            PortalVolume);

        // 진짜 절두체 컬링: Portal Frustum 안 액터만 기준으로 거리 계산
        float OptimalDistance = ComputeOptimalLumenDistance(
            PortalVolume,
            PortalFrustum.EyePosition);

        if (CVarLumenSceneViewDistance)
            CVarLumenSceneViewDistance->Set(OptimalDistance, ECVF_SetByCode);
        if (CVarLumenDistantScene)
            CVarLumenDistantScene->Set(0, ECVF_SetByCode);
    }
    else if (PortalVisibleBounds.Num() > 0)
    {
        float MaxRadius = 0.0f;
        for (const FBox& Box : PortalVisibleBounds)
        {
            float Radius = Box.GetExtent().Size();
            MaxRadius = FMath::Max(MaxRadius, Radius);
        }
        if (CVarLumenSceneViewDistance)
        {
            float OptimalDistance = FMath::Clamp(MaxRadius * 2.0f, 1000.0f, 8000.0f);
            CVarLumenSceneViewDistance->Set(OptimalDistance, ECVF_SetByCode);
        }
        if (CVarLumenDistantScene)
            CVarLumenDistantScene->Set(0, ECVF_SetByCode);
    }
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