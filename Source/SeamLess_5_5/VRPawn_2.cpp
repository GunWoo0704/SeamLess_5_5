#include "VRPawn_2.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"

AVRPawn_2::AVRPawn_2()
{
    PrimaryActorTick.bCanEverTick = true;

    CapsuleComp = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CapsuleComp"));
    RootComponent = CapsuleComp;
    CapsuleComp->SetCapsuleSize(40.0f, 96.0f);
    CapsuleComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

    VROrigin = CreateDefaultSubobject<USceneComponent>(TEXT("VROrigin"));
    VROrigin->SetupAttachment(RootComponent);

    VRCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("VRCamera"));
    VRCamera->SetupAttachment(VROrigin);
    VRCamera->bLockToHmd = true;  // HMD 추적 활성화
}

void AVRPawn_2::BeginPlay()
{
    Super::BeginPlay();

    // Enhanced Input 매핑 컨텍스트 등록
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            if (IMC_VR)
                Subsystem->AddMappingContext(IMC_VR, 0);
        }
    }
}

void AVRPawn_2::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
}

void AVRPawn_2::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        if (IA_Move)
            EIC->BindAction(IA_Move, ETriggerEvent::Triggered, this, &AVRPawn_2::Move);
        if (IA_Turn)
            EIC->BindAction(IA_Turn, ETriggerEvent::Triggered, this, &AVRPawn_2::Turn);
    }
}

void AVRPawn_2::Move(const FInputActionValue& Value)
{
    FVector2D MoveInput = Value.Get<FVector2D>();
    if (MoveInput.IsNearlyZero()) return;

    // 카메라가 바라보는 방향 기준으로 이동
    FVector Forward = VRCamera->GetForwardVector();
    FVector Right = VRCamera->GetRightVector();

    // 수평 이동만 (Z축 무시)
    Forward.Z = 0.0f;
    Right.Z = 0.0f;
    Forward.Normalize();
    Right.Normalize();

    FVector MoveDir = (Forward * MoveInput.Y) + (Right * MoveInput.X);
    AddActorWorldOffset(MoveDir * MoveSpeed * GetWorld()->GetDeltaSeconds(), true);
}

void AVRPawn_2::Turn(const FInputActionValue& Value)
{
    FVector2D TurnInput = Value.Get<FVector2D>();

    // 스냅 턴: 입력이 임계값 넘으면 한 번만 회전
    if (FMath::Abs(TurnInput.X) > 0.7f && bCanSnapTurn)
    {
        float TurnDir = TurnInput.X > 0.0f ? SnapTurnAngle : -SnapTurnAngle;
        AddActorWorldRotation(FRotator(0.0f, TurnDir, 0.0f));
        bCanSnapTurn = false;
    }
    else if (FMath::Abs(TurnInput.X) < 0.3f)
    {
        bCanSnapTurn = true;  // 조이스틱 중립 복귀 시 재활성화
    }
}