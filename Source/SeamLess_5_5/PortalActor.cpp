#include "PortalActor.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LevelStreamingDynamic.h"
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

    // 기본 카메라 위치 (눈높이)
    TargetCaptureLocation = FVector::ZeroVector;
}

void APortalActor::BeginPlay()
{
    Super::BeginPlay();

    ViewExtension = FSceneViewExtensions::NewExtension<FPortalViewExtension>();

    PortalMesh->SetRenderCustomDepth(true);
    PortalMesh->SetCustomDepthStencilValue(1);

    CacheSceneActorBounds();

    if (!RenderTarget)
    {
        RenderTarget = NewObject<UTextureRenderTarget2D>(this);
        RenderTarget->InitAutoFormat(1920, 1080);
        RenderTarget->bAutoGenerateMips = false;
        RenderTarget->RenderTargetFormat = RTF_RGBA16f;
    }

    SceneCapture->TextureTarget = RenderTarget;
    SceneCapture->ShowFlags.SetAtmosphere(true);
    SceneCapture->ShowFlags.SetSkyLighting(true);
    SceneCapture->ShowFlags.SetFog(true);
    SceneCapture->ShowFlags.SetVolumetricFog(true);
    SceneCapture->ShowFlags.SetDynamicShadows(true);

    if (PortalMaterial)
    {
        DynamicMaterial = UMaterialInstanceDynamic::Create(PortalMaterial, this);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), RenderTarget);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), RenderTarget);
        PortalMesh->SetMaterial(0, DynamicMaterial);
    }

    TriggerVolume->OnComponentBeginOverlap.AddDynamic(this, &APortalActor::OnOverlapBegin);

    if (!TargetLevel.IsNull())
        LoadTargetLevel();
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

void APortalActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!ViewExtension.IsValid()) return;

    ActorBoundsCacheTimer += DeltaTime;
    if (ActorBoundsCacheTimer >= ActorBoundsCacheInterval)
    {
        ActorBoundsCacheTimer = 0.0f;
        CacheSceneActorBounds();
    }

    UpdatePortalFrustumData();

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        // TargetCaptureLocation 위치에서 플레이어 회전 방향으로 찍기
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

void APortalActor::CacheSceneActorBounds()
{
    CachedActorBounds.Reset();
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        AActor* Actor = *It;
        if (Actor && Actor != this && Actor->GetRootComponent())
        {
            FBox Box = Actor->GetComponentsBoundingBox();
            if (Box.IsValid)
                CachedActorBounds.Add(FBoxSphereBounds(Box));
        }
    }
    ViewExtension->UpdateSceneActorBounds(CachedActorBounds);
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

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
    if (CameraManager)
        SceneCapture->FOVAngle = CameraManager->GetFOVAngle();
}

void APortalActor::OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult& SweepResult)
{
    APawn* Pawn = Cast<APawn>(OtherActor);
    if (!Pawn || !LinkedPortal) return;

    FVector ToPlayer = Pawn->GetActorLocation() - GetActorLocation();
    float Dot = FVector::DotProduct(ToPlayer, GetActorForwardVector());
    if (Dot < 0.0f) return;

    FTransform ThisTransform = GetActorTransform();
    FVector LocalPosition = ThisTransform.InverseTransformPosition(Pawn->GetActorLocation());
    FQuat   LocalRotation = ThisTransform.InverseTransformRotation(Pawn->GetActorRotation().Quaternion());

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector NewPosition = LinkedTransform.TransformPosition(LocalPosition);
    FQuat   NewRotation = LinkedTransform.TransformRotation(LocalRotation);

    FRotator FinalRotation = NewRotation.Rotator();
    FinalRotation.Yaw += 180.0f;

    Pawn->SetActorLocationAndRotation(NewPosition, FinalRotation);
}