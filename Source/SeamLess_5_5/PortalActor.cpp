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

    // 포탈 면을 클립 플레인으로 설정 — 포탈 너머 세계만 캡처
    SceneCapture->bEnableClipPlane = true;

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

    StreamingLevelActors.Reset();
    for (AActor* Actor : LoadedLevel->Actors)
    {
        if (!Actor) continue;
        StreamingLevelActors.Add(Actor);
    }

    UE_LOG(LogTemp, Log, TEXT("PortalActor: TargetLevel loaded, actors: %d"),
        StreamingLevelActors.Num());
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

    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        SceneCapture->FOVAngle = 104.0f;

        // 클립 플레인: 포탈 면의 위치와 법선 (포탈 너머만 캡처)
        SceneCapture->ClipPlaneBase = TargetCaptureLocation;
        SceneCapture->ClipPlaneNormal = -GetActorForwardVector();

        FRotator PlayerRot = Camera->GetComponentRotation();
        SceneCapture->SetWorldLocationAndRotation(
            TargetCaptureLocation,
            PlayerRot);
        SceneCapture->CaptureScene();
    }
    else if (LinkedPortal)
    {
        UpdateSceneCapture();

        float DistToPortal = FVector::Dist(
            PlayerPawn->GetActorLocation(),
            GetActorLocation());

        if (DistToPortal < 500.0f)
            SceneCapture->CaptureScene();
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

void APortalActor::OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult& SweepResult)
{
    APawn* Pawn = Cast<APawn>(OtherActor);
    if (!Pawn) return;

    // 포탈 앞면에서 진입했는지 확인 (뒤에서 진입 방지)
    FVector ToPlayer = Pawn->GetActorLocation() - GetActorLocation();
    float Dot = FVector::DotProduct(ToPlayer, GetActorForwardVector());
    if (Dot < 0.0f) return;

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
