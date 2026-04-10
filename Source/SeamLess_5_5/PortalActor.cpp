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
    bool bSuccess = false;
    StreamingLevel = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(
        GetWorld(),
        TargetLevel,
        TargetViewTransform.GetLocation(),
        TargetViewTransform.GetRotation().Rotator(),
        bSuccess
    );

    if (StreamingLevel && bSuccess)
    {
        StreamingLevel->OnLevelLoaded.AddDynamic(this, &APortalActor::OnTargetLevelLoaded);
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
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

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
