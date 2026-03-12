#include "PortalActor.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "IXRTrackingSystem.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"

APortalActor::APortalActor()
{
    PrimaryActorTick.bCanEverTick = true;

    PortalRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PortalRoot"));
    RootComponent = PortalRoot;

    PortalMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PortalMesh"));
    PortalMesh->SetupAttachment(PortalRoot);
    PortalMesh->SetMobility(EComponentMobility::Movable);
    PortalMesh->SetRelativeRotation(FRotator(0.0f, 0.0f, 0.0f));
    PortalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    SceneCaptureLeft = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCaptureLeft"));
    SceneCaptureLeft->SetupAttachment(PortalRoot);
    SetupCaptureComponent(SceneCaptureLeft);

    SceneCaptureRight = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCaptureRight"));
    SceneCaptureRight->SetupAttachment(PortalRoot);
    SetupCaptureComponent(SceneCaptureRight);

    TriggerVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerVolume"));
    TriggerVolume->SetupAttachment(PortalRoot);
    TriggerVolume->SetBoxExtent(FVector(50.0f, 200.0f, 200.0f));
    TriggerVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    TriggerVolume->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
    TriggerVolume->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
    TriggerVolume->SetGenerateOverlapEvents(true);
}

void APortalActor::SetupCaptureComponent(USceneCaptureComponent2D* Capture)
{
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
    Capture->bAlwaysPersistRenderingState = true;
    Capture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
    Capture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
    Capture->PostProcessSettings.bOverride_ReflectionMethod = true;
    Capture->PostProcessSettings.ReflectionMethod = EReflectionMethod::Lumen;
    Capture->PostProcessSettings.bOverride_LumenSceneDetail = true;
    Capture->PostProcessSettings.LumenSceneDetail = 1.0f;
    Capture->ShowFlags.SetGlobalIllumination(true);
}

void APortalActor::BeginPlay()
{
    Super::BeginPlay();

    if (!RenderTargetLeft)
    {
        RenderTargetLeft = NewObject<UTextureRenderTarget2D>(this);
        RenderTargetLeft->InitAutoFormat(1920, 1080);
        RenderTargetLeft->bAutoGenerateMips = false;
        RenderTargetLeft->RenderTargetFormat = RTF_RGBA16f;
    }

    if (!RenderTargetRight)
    {
        RenderTargetRight = NewObject<UTextureRenderTarget2D>(this);
        RenderTargetRight->InitAutoFormat(1920, 1080);
        RenderTargetRight->bAutoGenerateMips = false;
        RenderTargetRight->RenderTargetFormat = RTF_RGBA16f;
    }

    SceneCaptureLeft->TextureTarget = RenderTargetLeft;
    SceneCaptureRight->TextureTarget = RenderTargetRight;

    if (PortalMaterial)
    {
        DynamicMaterial = UMaterialInstanceDynamic::Create(PortalMaterial, this);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), RenderTargetLeft);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), RenderTargetRight);
        PortalMesh->SetMaterial(0, DynamicMaterial);
    }

    TriggerVolume->OnComponentBeginOverlap.AddDynamic(this, &APortalActor::OnOverlapBegin);
}

void APortalActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (LinkedPortal)
    {
        UpdateSceneCapture();
    }
}

FTransform APortalActor::GetPortalCameraTransform(const FVector& CameraLocation, const FRotator& CameraRotation)
{
    FTransform ThisTransform = GetActorTransform();
    FVector LocalPos = ThisTransform.InverseTransformPosition(CameraLocation);
    FQuat LocalRot = ThisTransform.InverseTransformRotation(CameraRotation.Quaternion());

    LocalPos.X = -LocalPos.X;
    LocalRot = FQuat(FVector::UpVector, PI) * LocalRot;

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector TargetPos = LinkedTransform.TransformPosition(LocalPos);
    FQuat TargetRot = LinkedTransform.TransformRotation(LocalRot);

    return FTransform(TargetRot, TargetPos);
}

void APortalActor::UpdateSceneCapture()
{
    if (!LinkedPortal) return;

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

    FVector CameraLocation = Camera->GetComponentLocation();
    FRotator CameraRotation = Camera->GetComponentRotation();

    FVector RightVector = CameraRotation.RotateVector(FVector::RightVector);
    float HalfIPD = IPD * 0.5f;

    // 哭率 传
    FVector LeftEyePos = CameraLocation - RightVector * HalfIPD;
    FTransform LeftTransform = GetPortalCameraTransform(LeftEyePos, CameraRotation);
    SceneCaptureLeft->SetWorldLocationAndRotation(LeftTransform.GetLocation(), LeftTransform.GetRotation().Rotator());

    // 坷弗率 传
    FVector RightEyePos = CameraLocation + RightVector * HalfIPD;
    FTransform RightTransform = GetPortalCameraTransform(RightEyePos, CameraRotation);
    SceneCaptureRight->SetWorldLocationAndRotation(RightTransform.GetLocation(), RightTransform.GetRotation().Rotator());

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
    if (CameraManager)
    {
        SceneCaptureLeft->FOVAngle = CameraManager->GetFOVAngle();
        SceneCaptureRight->FOVAngle = CameraManager->GetFOVAngle();
    }

    SceneCaptureLeft->CaptureScene();
    SceneCaptureRight->CaptureScene();
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
    FQuat LocalRotation = ThisTransform.InverseTransformRotation(Pawn->GetActorRotation().Quaternion());

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector NewPosition = LinkedTransform.TransformPosition(LocalPosition);
    FQuat NewRotation = LinkedTransform.TransformRotation(LocalRotation);

    FRotator FinalRotation = NewRotation.Rotator();
    FinalRotation.Yaw += 180.0f;

    Pawn->SetActorLocationAndRotation(NewPosition, FinalRotation);
}