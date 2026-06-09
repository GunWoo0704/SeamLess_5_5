#include "VRPawn_2.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Scalability.h"
#include "HAL/IConsoleManager.h"

// 실행 시 그래픽 품질(Engine Scalability)을 강제할 레벨.
//   -1 = 강제 안 함(엔진/사용자 설정 그대로), 0=Low, 1=Medium, 2=High, 3=Epic
//   패키지 빌드에서 GameUserSettings 하드웨어 자동감지가 덮어쓰는 걸 막아 Medium 고정.
static TAutoConsoleVariable<int32> CVarForceScalabilityLevel(
    TEXT("r.Portal.ForceScalabilityLevel"),
    1,
    TEXT("실행 시 강제할 그래픽 품질 레벨. -1=강제 안 함, 0=Low, 1=Medium, 2=High, 3=Epic. 기본 1(Medium)."),
    ECVF_Default
);

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
    VRCamera->bLockToHmd = true;  // HMD ���� Ȱ��ȭ
}

void AVRPawn_2::BeginPlay()
{
    Super::BeginPlay();

    // ── 그래픽 품질(Engine Scalability) 강제 ──
    // 패키지 빌드에서 GameUserSettings 자동감지가 ConsoleVariables.ini의 sg.* 를
    // 덮어쓰는 걸 막아, 실행 시 항상 지정 레벨(기본 Medium)로 고정 → 벤치마크 일관성.
    const int32 ForceLevel = CVarForceScalabilityLevel.GetValueOnGameThread();
    if (ForceLevel >= 0)
    {
        Scalability::FQualityLevels Q = Scalability::GetQualityLevels();
        Q.SetFromSingleQualityLevel(ForceLevel);
        Scalability::SetQualityLevels(Q);
        UE_LOG(LogTemp, Warning, TEXT("[VRPawn] Engine Scalability 강제 적용: 레벨 %d (1=Medium)"), ForceLevel);
    }

    // Enhanced Input ���� ���ؽ�Ʈ ���
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

    // ī�޶� �ٶ󺸴� ���� �������� �̵�
    FVector Forward = VRCamera->GetForwardVector();
    FVector Right = VRCamera->GetRightVector();

    // ���� �̵��� (Z�� ����)
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

    // ���� ��: �Է��� �Ӱ谪 ������ �� ���� ȸ��
    if (FMath::Abs(TurnInput.X) > 0.7f && bCanSnapTurn)
    {
        float TurnDir = TurnInput.X > 0.0f ? SnapTurnAngle : -SnapTurnAngle;
        AddActorWorldRotation(FRotator(0.0f, TurnDir, 0.0f));
        bCanSnapTurn = false;
    }
    else if (FMath::Abs(TurnInput.X) < 0.3f)
    {
        bCanSnapTurn = true;  // ���̽�ƽ �߸� ���� �� ��Ȱ��ȭ
    }
}