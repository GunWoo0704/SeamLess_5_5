#include "PortalActor.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PostProcessVolume.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"

// ───────────────────────────────────────────────────────────────
// 벤치마크용 토글 — 콘솔에서 r.Portal.Enable 0/1 로 켜고 끄기
//   0: 포탈 전체 비활성 (SceneCapture 중단 + 메시 숨김 + Frustum 갱신 중단)
//      → Baseline 측정용
//   1: 포탈 정상 동작 (기본값)
//
// FrustumCulling 토글은 PortalViewExtension.cpp 에 별도 존재:
//   r.Portal.FrustumCulling 0/1
//
// 3가지 벤치마크 시나리오:
//   ① r.Portal.Enable 0                           → Baseline
//   ② r.Portal.Enable 1, r.Portal.FrustumCulling 0 → 최적화 미적용
//   ③ r.Portal.Enable 1, r.Portal.FrustumCulling 1 → 최적화 적용 (저희 기여)
// ───────────────────────────────────────────────────────────────
static TAutoConsoleVariable<int32> CVarPortalEnable(
    TEXT("r.Portal.Enable"),
    1,
    TEXT("0: Portal 전체 비활성화 (벤치마크 Baseline)\n")
    TEXT("1: Portal 정상 동작 (기본값)"),
    ECVF_Default
);

APortalActor::APortalActor()
{
    PrimaryActorTick.bCanEverTick = true;

    PortalRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PortalRoot"));
    RootComponent = PortalRoot;

    PortalMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PortalMesh"));
    PortalMesh->SetupAttachment(PortalRoot);
    PortalMesh->SetMobility(EComponentMobility::Movable);
    PortalMesh->SetRelativeRotation(FRotator(0.0f, 0.0f, 90.0f));
    PortalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
    SceneCapture->SetupAttachment(PortalRoot);
    // Lumen GI 누적을 위해 bCaptureEveryFrame은 Tick에서 동적으로 관리
    // (근거리: true → Lumen Surface Cache 축적, 원거리: false → 성능)
    SceneCapture->bCaptureEveryFrame = false;
    SceneCapture->bCaptureOnMovement = false;
    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
    SceneCapture->bAlwaysPersistRenderingState = true;

    TriggerVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerVolume"));
    TriggerVolume->SetupAttachment(PortalRoot);
    TriggerVolume->SetBoxExtent(FVector(50.0f, 200.0f, 200.0f));
    TriggerVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    TriggerVolume->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
    TriggerVolume->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
    TriggerVolume->SetGenerateOverlapEvents(true);

    TargetCaptureLocation = FVector::ZeroVector;
}

void APortalActor::BeginPlay()
{
    Super::BeginPlay();

    ViewExtension = FSceneViewExtensions::NewExtension<FPortalViewExtension>();

    // 포탈 메시: Custom Depth + Stencil 활성화
    PortalMesh->SetRenderCustomDepth(true);
    PortalMesh->SetCustomDepthStencilValue(PortalStencilValue);

    // 렌더 타겟 생성
    if (!RenderTarget)
    {
        RenderTarget = NewObject<UTextureRenderTarget2D>(this);
        RenderTarget->InitAutoFormat(1920, 1080);
        RenderTarget->bAutoGenerateMips = false;
        RenderTarget->RenderTargetFormat = RTF_RGBA16f;
    }

    SceneCapture->TextureTarget = RenderTarget;
    SceneCapture->FOVAngle = 104.0f;  // Quest 3 HMD FOV
    SceneCapture->ShowFlags.SetAtmosphere(true);
    SceneCapture->ShowFlags.SetSkyLighting(true);
    SceneCapture->ShowFlags.SetFog(true);
    SceneCapture->ShowFlags.SetVolumetricFog(true);
    SceneCapture->ShowFlags.SetDynamicShadows(true);
    SceneCapture->ShowFlags.SetGlobalIllumination(true);
    SceneCapture->ShowFlags.SetLumenGlobalIllumination(true);  // ← 핵심: 이게 없으면 ShouldRenderLumenDiffuseGI가 false 반환
    SceneCapture->ShowFlags.SetLighting(true);
    SceneCapture->ShowFlags.SetPostProcessing(true);
    SceneCapture->ShowFlags.SetAmbientOcclusion(true);
    SceneCapture->ShowFlags.SetLumenReflections(true);
    SceneCapture->ShowFlags.SetIndirectLightingCache(true);
    SceneCapture->ShowFlags.SetSkyLighting(true);

    // Lumen GI 방식 명시적 설정 — FinalPostProcessSettings.DynamicGlobalIlluminationMethod == Lumen 조건 충족
    SceneCapture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
    SceneCapture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;

    // 노출 설정 — SceneCapture는 기본적으로 auto exposure가 꺼져 있어서 어둡게 나옴
    // Min/Max를 같은 값으로 잠그면 씬 밝기와 어긋남 → 적절한 범위로 열어줌
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    SceneCapture->PostProcessSettings.AutoExposureMethod = AEM_Histogram;
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
    SceneCapture->PostProcessSettings.AutoExposureMinBrightness = 0.18f;  // 실내 씬 기준
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
    SceneCapture->PostProcessSettings.AutoExposureMaxBrightness = 8.0f;
    // SpeedUp/Down: SceneCapture는 매 프레임 재설정되므로 빠른 적응이 필요
    SceneCapture->PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
    SceneCapture->PostProcessSettings.AutoExposureSpeedUp = 10.0f;
    SceneCapture->PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
    SceneCapture->PostProcessSettings.AutoExposureSpeedDown = 10.0f;

    // 클립 플레인: Tick에서 모드에 따라 동적으로 설정
    SceneCapture->bEnableClipPlane = false;

    // 포탈 메시 머티리얼 설정
    if (PortalMaterial)
    {
        DynamicMaterial = UMaterialInstanceDynamic::Create(PortalMaterial, this);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), RenderTarget);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), RenderTarget);
        PortalMesh->SetMaterial(0, DynamicMaterial);
    }

    // 스텐실 PP는 일단 비활성화 - 메시 머티리얼로 포탈 표시 확인 후 활성화
    // BindStencilMaterialToVolume();

    TriggerVolume->OnComponentBeginOverlap.AddDynamic(this, &APortalActor::OnOverlapBegin);

    if (!TargetLevel.IsNull())
        LoadTargetLevel();
}

void APortalActor::BindStencilMaterialToVolume()
{
    if (!StencilPostProcessMaterial || !RenderTarget)
    {
        UE_LOG(LogTemp, Warning, TEXT("PortalActor: StencilPostProcessMaterial or RenderTarget not set"));
        return;
    }

    // PP 머티리얼 다이내믹 인스턴스 생성
    StencilPPInstance = UMaterialInstanceDynamic::Create(StencilPostProcessMaterial, this);
    StencilPPInstance->SetTextureParameterValue(FName("PortalRT"), RenderTarget);

    // 레벨에 있는 PostProcessVolume 찾기
    for (TActorIterator<APostProcessVolume> It(GetWorld()); It; ++It)
    {
        APostProcessVolume* PPVolume = *It;
        if (PPVolume && PPVolume->bUnbound)
        {
            PPVolume->Settings.WeightedBlendables.Array.Add(
                FWeightedBlendable(1.0f, StencilPPInstance));

            UE_LOG(LogTemp, Log, TEXT("PortalActor: Stencil PP bound to existing PostProcessVolume"));
            return;
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("PortalActor: No unbound PostProcessVolume found! Place one in the level with Infinite Extent enabled."));
}

void APortalActor::LoadTargetLevel()
{
    // TargetViewTransform.Rotation + TargetLevelRotation을 합성해
    // 스트리밍 레벨 전체를 회전시킴.
    // 예) TargetLevelRotation.Yaw = 90 → Downtown_Alley가 Z축 기준 90도 돌아서 스폰됨
    const FQuat ComposedQ =
        TargetLevelRotation.Quaternion() * TargetViewTransform.GetRotation();
    const FRotator SpawnRot = ComposedQ.Rotator();

    bool bSuccess = false;
    StreamingLevel = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(
        GetWorld(),
        TargetLevel,
        TargetViewTransform.GetLocation(),
        SpawnRot,
        bSuccess
    );

    if (StreamingLevel && bSuccess)
    {
        StreamingLevel->OnLevelLoaded.AddDynamic(this, &APortalActor::OnTargetLevelLoaded);
        UE_LOG(LogTemp, Warning, TEXT("[Portal] TargetLevel spawn rot=%s (LevelRot=%s)"),
            *SpawnRot.ToString(), *TargetLevelRotation.ToString());
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("PortalActor: TargetLevel load failed"));
    }
}

void APortalActor::OnTargetLevelLoaded()
{
    ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
    if (!LoadedLevel) return;

    // SceneCapture가 스트리밍 레벨을 볼 수 있도록 명시적으로 Visible 설정
    StreamingLevel->SetShouldBeVisible(true);

    StreamingLevelActors.Reset();
    for (AActor* Actor : LoadedLevel->Actors)
    {
        if (!Actor) continue;
        StreamingLevelActors.Add(Actor);
    }

    UE_LOG(LogTemp, Warning, TEXT("[Portal] TargetLevel loaded! Actors: %d | Level: %s"),
        StreamingLevelActors.Num(),
        *LoadedLevel->GetPathName());
}

void APortalActor::UpdateStreamingLevelBounds()
{
    if (!ViewExtension.IsValid()) return;

    TArray<FBoxSphereBounds> StreamingBounds;
    for (AActor* Actor : StreamingLevelActors)
    {
        if (!Actor || !Actor->GetRootComponent()) continue;
        FBox Box = Actor->GetComponentsBoundingBox(true);
        if (Box.IsValid && Box.GetExtent().SizeSquared() > 0.0f)
            StreamingBounds.Add(FBoxSphereBounds(Box));
    }

    ViewExtension->UpdateSceneActorBounds(StreamingBounds);

    UE_LOG(LogTemp, Log, TEXT("PortalActor: StreamingLevel bounds updated: %d actors"),
        StreamingBounds.Num());
}

void APortalActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // ── 벤치마크 토글 처리 ─────────────────────────────────────
    // r.Portal.Enable 0 일 때 포탈 렌더/캡처/프러스텀 갱신 전부 중단
    // 상태 변경 시에만 컴포넌트 가시성을 토글 (매 프레임 SetVisibility 방지)
    const bool bPortalEnabled = (CVarPortalEnable.GetValueOnGameThread() != 0);
    static bool bLastPortalEnabled = true;
    if (bPortalEnabled != bLastPortalEnabled)
    {
        if (PortalMesh) PortalMesh->SetVisibility(bPortalEnabled, true);
        if (SceneCapture)
        {
            SceneCapture->SetVisibility(bPortalEnabled, true);
            if (!bPortalEnabled) SceneCapture->bCaptureEveryFrame = false;
        }
        bLastPortalEnabled = bPortalEnabled;

        UE_LOG(LogTemp, Warning, TEXT("[Portal] r.Portal.Enable = %d"), bPortalEnabled ? 1 : 0);
    }

    if (!bPortalEnabled)
    {
        // 포탈 OFF 상태 — 디버그 라인만 정리하고 나머지 로직 전부 스킵
        if (bDebugLinesDrawn)
        {
            FlushPersistentDebugLines(GetWorld());
            bDebugLinesDrawn = false;
        }
        return;
    }

    // 디버그 레이: 켜면 한 번 그리고 유지, 끄면 제거
    if (bDebugLumenRays && !bDebugLinesDrawn)
    {
        APawn* DebugPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
        UCameraComponent* DebugCamera = DebugPawn
            ? DebugPawn->FindComponentByClass<UCameraComponent>()
            : nullptr;
        DrawLumenDebug(DebugCamera);
        bDebugLinesDrawn = true;
    }
    else if (!bDebugLumenRays && bDebugLinesDrawn)
    {
        FlushPersistentDebugLines(GetWorld());
        bDebugLinesDrawn = false;
    }

    if (!ViewExtension.IsValid()) return;

    // StreamingLevel 바운드 수집
    if (StreamingLevel && StreamingLevel->GetLoadedLevel() && ViewExtension.IsValid())
    {
        ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();

        if (StreamingLevelActors.Num() == 0)
        {
            for (AActor* Actor : LoadedLevel->Actors)
            {
                if (!Actor) continue;
                StreamingLevelActors.Add(Actor);
            }
        }

        if (StreamingLevelActors.Num() > 0)
        {
            TArray<FBoxSphereBounds> StreamingBounds;
            for (AActor* Actor : StreamingLevelActors)
            {
                if (!Actor || !Actor->GetRootComponent()) continue;
                FBox Box = Actor->GetComponentsBoundingBox(true);
                if (Box.IsValid && Box.GetExtent().SizeSquared() > 0.0f)
                    StreamingBounds.Add(FBoxSphereBounds(Box));
            }

            if (StreamingBounds.Num() > 0)
            {
                ViewExtension->UpdateSceneActorBounds(StreamingBounds);
            }
        }
    }

    UpdatePortalFrustumData();

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    UCameraComponent* Camera = PlayerPawn
        ? PlayerPawn->FindComponentByClass<UCameraComponent>()
        : nullptr;

    if (!PlayerPawn || !Camera) return;

    // VR에서는 Overlap이 안 잡히는 경우가 많아서 Tick에서 직접 감지
    CheckPortalCrossing(Camera);

    float DistToPortal = FVector::Dist(
        PlayerPawn->GetActorLocation(),
        GetActorLocation());

    // Lumen GI는 bCaptureEveryFrame=true 상태에서 연속 프레임을 통해 누적됨
    // 수동 CaptureScene()은 Lumen 입장에서 매번 새 뷰 → GI 누적 안 됨
    // 1000cm 이내: 매 프레임 캡처 (Lumen 누적), 이외: 캡처 정지 (성능)
    const float CaptureRange = 1000.0f;
    bool bShouldCapture = (DistToPortal < CaptureRange);
    SceneCapture->bCaptureEveryFrame = bShouldCapture;

    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        SceneCapture->FOVAngle = 104.0f;
        SceneCapture->bEnableClipPlane = false;

        // TargetCaptureLocation = 레벨 안 카메라 위치 (에디터에서 직접 설정)
        // TargetViewTransform = 레벨 스폰 위치 (BeginPlay에서만 사용)
        FRotator PlayerRot = Camera->GetComponentRotation();
        SceneCapture->SetWorldLocationAndRotation(TargetCaptureLocation, PlayerRot);

        // 디버그: 매 60프레임마다 상태 출력
        static int32 DebugFrame = 0;
        if (++DebugFrame % 60 == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[Portal] SceneCapture pos: %s | LoadedLevel actors: %d"),
                *TargetCaptureLocation.ToString(),
                StreamingLevel->GetLoadedLevel()->Actors.Num());
            UE_LOG(LogTemp, Warning, TEXT("[Portal] LevelSpawn(TargetViewTransform): %s"),
                *TargetViewTransform.GetLocation().ToString());
        }
    }
    else if (LinkedPortal)
    {
        // LinkedPortal 모드: 클립 플레인 활성화 (포탈 너머만 캡처)
        SceneCapture->bEnableClipPlane = true;
        UpdateSceneCapture();
    }
}

void APortalActor::UpdatePortalFrustumData()
{
    if (!ViewExtension.IsValid()) return;

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

    FPortalFrustumData FrustumData;
    FrustumData.EyePosition = Camera->GetComponentLocation();

    FVector PortalCenter = PortalMesh->GetComponentLocation();
    FVector Right = PortalMesh->GetRightVector();
    FVector Up = PortalMesh->GetUpVector();
    FVector Scale = PortalMesh->GetComponentScale();
    float HalfWidth = 50.0f * Scale.Y;
    float HalfHeight = 50.0f * Scale.Z;

    FrustumData.Corners[0] = PortalCenter + (Right * HalfWidth) + (Up * HalfHeight);
    FrustumData.Corners[1] = PortalCenter - (Right * HalfWidth) + (Up * HalfHeight);
    FrustumData.Corners[2] = PortalCenter - (Right * HalfWidth) - (Up * HalfHeight);
    FrustumData.Corners[3] = PortalCenter + (Right * HalfWidth) - (Up * HalfHeight);

    FrustumData.PortalBounds = PortalMesh->Bounds.GetBox();
    FrustumData.bIsValid = true;

    ViewExtension->UpdatePortalFrustum(FrustumData);
}

void APortalActor::UpdateSceneCapture()
{
    if (!LinkedPortal) return;

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

    FVector  CameraLocation = Camera->GetComponentLocation();
    FRotator CameraRotation = Camera->GetComponentRotation();

    FTransform ThisTransform = GetActorTransform();
    FVector LocalPos = ThisTransform.InverseTransformPosition(CameraLocation);
    FQuat   LocalRot = ThisTransform.InverseTransformRotation(CameraRotation.Quaternion());

    LocalPos.X = -LocalPos.X;
    LocalRot = FQuat(FVector::UpVector, PI) * LocalRot;

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector TargetPos = LinkedTransform.TransformPosition(LocalPos);
    FQuat   TargetRot = LinkedTransform.TransformRotation(LocalRot);

    SceneCapture->SetWorldLocationAndRotation(TargetPos, TargetRot.Rotator());

    SceneCapture->FOVAngle = 104.0f;

    // 클립 플레인: LinkedPortal 면 기준으로 너머만 캡처
    SceneCapture->ClipPlaneBase = LinkedPortal->GetActorLocation();
    SceneCapture->ClipPlaneNormal = -LinkedPortal->GetActorForwardVector();
}

void APortalActor::CheckPortalCrossing(UCameraComponent* Camera)
{
    if (!Camera) return;

    FVector CameraPos = Camera->GetComponentLocation();
    FVector PortalPos = GetActorLocation();
    FVector PortalNormal = GetActorForwardVector();

    // 포탈까지 거리가 너무 멀면 체크 안 함
    float DistToPortal = FVector::Dist(CameraPos, PortalPos);
    if (DistToPortal > 200.0f)
    {
        LastDotSign = 0;
        return;
    }

    // 포탈 평면 기준 카메라 부호 (양수=앞, 음수=뒤)
    FVector ToCamera = CameraPos - PortalPos;
    float Dot = FVector::DotProduct(ToCamera, PortalNormal);
    int32 CurrentSign = (Dot >= 0.0f) ? 1 : -1;

    // 앞(+)에서 뒤(-)로 넘어간 순간 = 포탈 통과
    if (LastDotSign == 1 && CurrentSign == -1)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Portal] Camera crossed portal plane → Teleporting"));
        ExecuteTeleport(UGameplayStatics::GetPlayerPawn(GetWorld(), 0));
    }

    LastDotSign = CurrentSign;
}

void APortalActor::ExecuteTeleport(APawn* Pawn)
{
    if (!Pawn) return;

    // 모드 1: TargetLevel
    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        FVector TeleportLocation = TargetCaptureLocation;
        FRotator TeleportRotation = TargetViewTransform.GetRotation().Rotator();
        Pawn->SetActorLocationAndRotation(TeleportLocation, TeleportRotation);
        LastDotSign = 0;

        UE_LOG(LogTemp, Warning, TEXT("[Portal] Teleported to TargetLevel at %s"),
            *TeleportLocation.ToString());
        return;
    }

    // 모드 2: LinkedPortal
    if (!LinkedPortal) return;

    FTransform ThisTransform = GetActorTransform();
    FVector LocalPosition = ThisTransform.InverseTransformPosition(Pawn->GetActorLocation());
    FQuat   LocalRotation = ThisTransform.InverseTransformRotation(Pawn->GetActorRotation().Quaternion());

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector NewPosition = LinkedTransform.TransformPosition(LocalPosition);
    FQuat   NewRotation = LinkedTransform.TransformRotation(LocalRotation);

    FRotator FinalRotation = NewRotation.Rotator();
    FinalRotation.Yaw += 180.0f;

    Pawn->SetActorLocationAndRotation(NewPosition, FinalRotation);
    LastDotSign = 0;

    UE_LOG(LogTemp, Warning, TEXT("[Portal] Teleported to LinkedPortal"));
}

void APortalActor::DrawLumenDebug(UCameraComponent* Camera)
{
    UWorld* World = GetWorld();
    if (!World) return;

    // ───────────────────────────────────────────────
    // 1) SceneCapture 샘플 레이 그리드 (Red)
    //    Lumen이 타겟 레벨을 어떤 방향으로 샘플링하는지 시각화
    // ───────────────────────────────────────────────
    FVector CapturePos     = SceneCapture->GetComponentLocation();
    FVector CaptureForward = SceneCapture->GetForwardVector();
    FVector CaptureRight   = SceneCapture->GetRightVector();
    FVector CaptureUp      = SceneCapture->GetUpVector();

    const int32 GridSize  = 20;    // 20x20 = 400개 레이
    const float Spread    = 0.8f;  // FOV 104도에 맞춰 넓게
    const float RayLength = 2000.f;

    for (int32 xi = 0; xi < GridSize; xi++)
    {
        for (int32 yi = 0; yi < GridSize; yi++)
        {
            float OffsetX = (xi - (GridSize - 1) * 0.5f) * Spread / (GridSize - 1);
            float OffsetY = (yi - (GridSize - 1) * 0.5f) * Spread / (GridSize - 1);

            FVector Dir = (CaptureForward
                         + CaptureRight * OffsetX
                         + CaptureUp    * OffsetY).GetSafeNormal();

            DrawDebugLine(
                World,
                CapturePos,
                CapturePos + Dir * RayLength,
                FColor::Red,
                true,    // bPersistentLines = true → FlushPersistentDebugLines로 명시적 제거
                -1.f,
                0,
                0.5f
            );
        }
    }

    // SceneCapture 위치 표시 (노란 구)
    DrawDebugSphere(World, CapturePos, 8.f, 8, FColor::Yellow, true, -1.f, 0, 1.5f);

    // ───────────────────────────────────────────────
    // 2) 포탈 프러스텀 (Green) — 카메라 → 포탈 4꼭짓점
    // ───────────────────────────────────────────────
    FVector PortalCenter = PortalMesh->GetComponentLocation();
    FVector Right = PortalMesh->GetRightVector();
    FVector Up    = PortalMesh->GetUpVector();
    FVector Scale = PortalMesh->GetComponentScale();
    float HalfW = 50.f * Scale.Y;
    float HalfH = 50.f * Scale.Z;

    FVector Corners[4] = {
        PortalCenter + Right * HalfW + Up * HalfH,
        PortalCenter - Right * HalfW + Up * HalfH,
        PortalCenter - Right * HalfW - Up * HalfH,
        PortalCenter + Right * HalfW - Up * HalfH
    };

    for (int32 i = 0; i < 4; i++)
        DrawDebugLine(World, Corners[i], Corners[(i + 1) % 4], FColor::Green, true, -1.f, 0, 2.f);

    if (Camera)
    {
        FVector CameraPos = Camera->GetComponentLocation();
        for (int32 i = 0; i < 4; i++)
            DrawDebugLine(World, CameraPos, Corners[i], FColor(0, 200, 0), true, -1.f, 0, 0.5f);
    }

    // ───────────────────────────────────────────────
    // 3) 포탈 법선 벡터 (Red 화살표)
    // ───────────────────────────────────────────────
    FVector PortalNormal = GetActorForwardVector();
    DrawDebugDirectionalArrow(
        World,
        PortalCenter,
        PortalCenter + PortalNormal * 120.f,
        30.f,
        FColor::Red,
        true, -1.f, 0, 2.f
    );

    // ───────────────────────────────────────────────
    // 4) 텔레포트 감지 거리 (Orange 구, 200cm)
    // ───────────────────────────────────────────────
    DrawDebugSphere(World, GetActorLocation(), 200.f, 16, FColor::Orange, true, -1.f, 0, 0.5f);
}

void APortalActor::OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult& SweepResult)
{
    UE_LOG(LogTemp, Warning, TEXT("[Teleport] OnOverlapBegin: OtherActor=%s"), *OtherActor->GetName());

    APawn* Pawn = Cast<APawn>(OtherActor);
    if (!Pawn)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Teleport] FAIL: OtherActor is not a Pawn"));
        return;
    }

    // 포탈 앞면에서 진입했는지 확인 (뒤에서 진입 방지)
    FVector ToPlayer = Pawn->GetActorLocation() - GetActorLocation();
    float Dot = FVector::DotProduct(ToPlayer, GetActorForwardVector());
    UE_LOG(LogTemp, Warning, TEXT("[Teleport] Dot product: %.2f (negative = back side, teleport blocked)"), Dot);
    if (Dot < 0.0f) return;

    UE_LOG(LogTemp, Warning, TEXT("[Teleport] TargetLevel.IsNull=%d, StreamingLevel=%d, LoadedLevel=%d"),
        (int32)TargetLevel.IsNull(),
        (int32)(StreamingLevel != nullptr),
        (int32)(StreamingLevel && StreamingLevel->GetLoadedLevel() != nullptr));

    // 모드 1: TargetLevel (스트리밍 레벨로 순간이동)
    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        FVector TeleportLocation = TargetViewTransform.GetLocation();
        FRotator TeleportRotation = TargetViewTransform.GetRotation().Rotator();

        Pawn->SetActorLocationAndRotation(TeleportLocation, TeleportRotation);

        UE_LOG(LogTemp, Log, TEXT("PortalActor: Teleported to TargetLevel at %s"),
            *TeleportLocation.ToString());
        return;
    }

    // 모드 2: LinkedPortal (같은 레벨 내 포탈 간 이동)
    if (!LinkedPortal) return;

    FTransform ThisTransform = GetActorTransform();
    FVector LocalPosition = ThisTransform.InverseTransformPosition(Pawn->GetActorLocation());
    FQuat   LocalRotation = ThisTransform.InverseTransformRotation(Pawn->GetActorRotation().Quaternion());

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector NewPosition = LinkedTransform.TransformPosition(LocalPosition);
    FQuat   NewRotation = LinkedTransform.TransformRotation(LocalRotation);

    FRotator FinalRotation = NewRotation.Rotator();
    FinalRotation.Yaw += 180.0f;

    Pawn->SetActorLocationAndRotation(NewPosition, FinalRotation);

    UE_LOG(LogTemp, Log, TEXT("PortalActor: Teleported to LinkedPortal"));
}
