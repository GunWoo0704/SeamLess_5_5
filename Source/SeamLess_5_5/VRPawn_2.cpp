// ============================================================================
// VRPawn_2.cpp — VR 플레이어 폰 구현부
// ----------------------------------------------------------------------------
// [구성] (1) r.Portal.ForceScalabilityLevel CVar 정의 — 벤치마크 일관성을 위해
//   실행 시 그래픽 품질(sg.* 스칼라빌리티)을 강제 고정하는 스위치.
//   (2) 생성자 — 캡슐/VROrigin/카메라 컴포넌트 조립.
//   (3) BeginPlay — 스칼라빌리티 강제 적용 + Enhanced Input 매핑 컨텍스트 등록.
//   (4) Move/Turn — 조이스틱 이동, 스냅 턴.
// [스레드] 이 파일의 모든 코드는 게임 스레드에서만 실행된다.
// [주의] 일부 옛 주석이 인코딩 깨짐(CP949→UTF-8)으로 ��� 로 보임 — 원문 의미는
//   새로 단 한글 주석에 풀어 적었다.
// ============================================================================

#include "VRPawn_2.h"
#include "EnhancedInputComponent.h"      // UEnhancedInputComponent: 액션 바인딩
#include "EnhancedInputSubsystems.h"     // 로컬 플레이어별 입력 서브시스템 (매핑 컨텍스트 등록)
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Scalability.h"                 // Scalability::Get/SetQualityLevels (sg.* 품질 일괄 제어)
#include "HAL/IConsoleManager.h"         // TAutoConsoleVariable (콘솔 변수)

// 실행 시 그래픽 품질(Engine Scalability)을 강제할 레벨.
//   -1 = 강제 안 함(엔진/사용자 설정 그대로), 0=Low, 1=Medium, 2=High, 3=Epic
//   패키지 빌드에서 GameUserSettings 하드웨어 자동감지가 덮어쓰는 걸 막아 Medium 고정.
static TAutoConsoleVariable<int32> CVarForceScalabilityLevel(
    TEXT("r.Portal.ForceScalabilityLevel"),
    1,
    TEXT("실행 시 강제할 그래픽 품질 레벨. -1=강제 안 함, 0=Low, 1=Medium, 2=High, 3=Epic. 기본 1(Medium)."),
    ECVF_Default
);

// 생성자. [게임 스레드] 컴포넌트 트리를 조립한다.
// CreateDefaultSubobject = 생성자 안에서만 쓸 수 있는 컴포넌트 생성 함수
// (블루프린트 파생/CDO에서도 동일 구조가 보장됨).
AVRPawn_2::AVRPawn_2()
{
    // 매 프레임 Tick() 호출 허용 (현재 Tick은 비어 있지만 확장 대비)
    PrimaryActorTick.bCanEverTick = true;

    // 충돌 캡슐 = 루트. 반경 40cm, 절반 높이 96cm (총 키 약 192cm)
    CapsuleComp = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CapsuleComp"));
    RootComponent = CapsuleComp;
    CapsuleComp->SetCapsuleSize(40.0f, 96.0f);
    // QueryAndPhysics: 레이캐스트(Query)도 맞고 물리 충돌(Physics)도 함
    CapsuleComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

    // 트래킹 기준점. 캡슐(루트)에 붙어서 폰이 움직이면 같이 따라감
    VROrigin = CreateDefaultSubobject<USceneComponent>(TEXT("VROrigin"));
    VROrigin->SetupAttachment(RootComponent);

    // HMD 카메라. VROrigin 밑에 붙임 (트래킹 좌표가 VROrigin 기준 상대값이라서)
    VRCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("VRCamera"));
    VRCamera->SetupAttachment(VROrigin);
    // 아래 bLockToHmd: 매 프레임 HMD의 실제 위치/회전을 카메라에 자동 반영
    // (뒤의 깨진 주석 원문: "HMD 추적 활성화")
    VRCamera->bLockToHmd = true;  // HMD ���� Ȱ��ȭ
}

// 게임 시작(스폰) 시 1회 호출. [게임 스레드]
// (1) 벤치마크용 그래픽 품질 강제 고정, (2) Enhanced Input 매핑 컨텍스트 등록.
void AVRPawn_2::BeginPlay()
{
    Super::BeginPlay();

    // ── 그래픽 품질(Engine Scalability) 강제 ──
    // 패키지 빌드에서 GameUserSettings 자동감지가 ConsoleVariables.ini의 sg.* 를
    // 덮어쓰는 걸 막아, 실행 시 항상 지정 레벨(기본 Medium)로 고정 → 벤치마크 일관성.
    // GetValueOnGameThread(): 게임 스레드에서 CVar 값을 읽는 올바른 방법
    const int32 ForceLevel = CVarForceScalabilityLevel.GetValueOnGameThread();
    if (ForceLevel >= 0) // -1이면 강제하지 않음 (엔진/사용자 설정 존중)
    {
        // FQualityLevels = sg.ViewDistanceQuality, sg.ShadowQuality 등
        // 스칼라빌리티 항목 전체를 담는 구조체. 현재 값을 가져와서
        // SetFromSingleQualityLevel()로 모든 항목을 같은 레벨로 일괄 변경 후 적용.
        Scalability::FQualityLevels Q = Scalability::GetQualityLevels();
        Q.SetFromSingleQualityLevel(ForceLevel);
        Scalability::SetQualityLevels(Q);
        UE_LOG(LogTemp, Warning, TEXT("[VRPawn] Engine Scalability 강제 적용: 레벨 %d (1=Medium)"), ForceLevel);
    }

    // Enhanced Input ���� ���ؽ�Ʈ ���
    // (위 깨진 한글 주석 원문: "Enhanced Input 매핑 컨텍스트 등록")
    // 입력 매핑표(IMC_VR)를 이 플레이어의 입력 서브시스템에 등록해야
    // 비로소 조이스틱 입력이 IA_Move/IA_Turn 액션으로 변환되기 시작한다.
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        // 서브시스템 = 엔진이 플레이어/월드 단위로 하나씩 자동 생성해 주는 싱글톤 객체
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            if (IMC_VR) // 에디터에서 에셋을 안 꽂아두면 nullptr → 건너뜀
                Subsystem->AddMappingContext(IMC_VR, 0); // 0 = 우선순위 (여러 IMC가 겹칠 때 사용)
        }
    }
}

// 매 프레임 호출. [게임 스레드] 현재 추가 로직 없음 — 카메라는 bLockToHmd가,
// 이동/회전은 입력 콜백(Move/Turn)이 알아서 처리하므로 여기서 할 일이 없다.
void AVRPawn_2::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
}

// 입력 액션 ↔ 처리 함수 연결. [게임 스레드] 컨트롤러가 폰에 빙의할 때 엔진이 1회 호출.
void AVRPawn_2::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    // 프로젝트가 Enhanced Input을 쓰면 PlayerInputComponent의 실제 타입이
    // UEnhancedInputComponent이므로 Cast가 성공한다 (구식 입력이면 nullptr)
    if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
    {
        // ETriggerEvent::Triggered = 입력이 활성인 동안 매 프레임 콜백
        // (조이스틱을 기울이고 있는 내내 Move/Turn이 반복 호출됨)
        if (IA_Move)
            EIC->BindAction(IA_Move, ETriggerEvent::Triggered, this, &AVRPawn_2::Move);
        if (IA_Turn)
            EIC->BindAction(IA_Turn, ETriggerEvent::Triggered, this, &AVRPawn_2::Turn);
    }
}

// 이동 처리. [게임 스레드] 왼쪽 조이스틱이 기울어져 있는 동안 매 프레임 호출.
// "HMD가 바라보는 방향" 기준으로 수평 이동한다 (머리를 돌리면 앞 방향도 바뀜).
void AVRPawn_2::Move(const FInputActionValue& Value)
{
    // 입력값을 2D 벡터로 꺼냄. X=좌우 기울기, Y=앞뒤 기울기 (-1~+1)
    FVector2D MoveInput = Value.Get<FVector2D>();
    if (MoveInput.IsNearlyZero()) return; // 데드존: 거의 0이면 무시

    // ī�޶� �ٶ󺸴� ���� �������� �̵�
    // (위 깨진 한글 주석 원문: "카메라가 바라보는 방향 기준으로 이동")
    // HMD 카메라의 앞/오른쪽 방향 벡터를 가져옴 (월드 좌표계 기준)
    FVector Forward = VRCamera->GetForwardVector();
    FVector Right = VRCamera->GetRightVector();

    // ���� �̵��� (Z�� ����)
    // (위 깨진 한글 주석 원문: "수평 이동만 (Z축 제거)")
    // 고개를 숙이고 있어도 땅속/하늘로 가지 않도록 Z 성분을 0으로 만들고,
    // Z를 깎으면서 길이가 줄어든 벡터를 다시 단위 길이로 정규화
    Forward.Z = 0.0f;
    Right.Z = 0.0f;
    Forward.Normalize();
    Right.Normalize();

    // 스틱 입력을 방향에 곱해 최종 이동 방향 합성 (Y=전후, X=좌우)
    FVector MoveDir = (Forward * MoveInput.Y) + (Right * MoveInput.X);
    // 속도 * DeltaSeconds = 프레임레이트와 무관하게 일정한 초당 이동량.
    // 마지막 인자 true = 스윕(sweep) 이동: 경로상 충돌을 검사해 벽을 뚫지 않음
    AddActorWorldOffset(MoveDir * MoveSpeed * GetWorld()->GetDeltaSeconds(), true);
}

// 스냅 턴 처리. [게임 스레드] 오른쪽 조이스틱 입력 동안 매 프레임 호출.
// 부드러운 회전은 VR 멀미를 유발하므로 45도씩 "딱딱" 끊어 도는 방식 사용.
// 히스테리시스(0.7로 발동 / 0.3 미만으로 해제) 구조라 스틱을 꺾고 있는 동안
// 연속으로 빙글빙글 돌지 않고, 한 번 꺾을 때 정확히 1회만 회전한다.
void AVRPawn_2::Turn(const FInputActionValue& Value)
{
    FVector2D TurnInput = Value.Get<FVector2D>();

    // ���� ��: �Է��� �Ӱ谪 ������ �� ���� ȸ��
    // (위 깨진 한글 주석 원문: "스냅 턴: 입력이 임계값 넘었을 때 한 번만 회전")
    // 스틱 좌우(X)가 70% 이상 꺾였고, 아직 잠금이 안 걸려 있으면 1회 회전
    if (FMath::Abs(TurnInput.X) > 0.7f && bCanSnapTurn)
    {
        // 꺾은 방향에 따라 +45도(우) 또는 -45도(좌)
        float TurnDir = TurnInput.X > 0.0f ? SnapTurnAngle : -SnapTurnAngle;
        // FRotator(Pitch, Yaw, Roll) — Yaw(수평 회전)만 적용
        AddActorWorldRotation(FRotator(0.0f, TurnDir, 0.0f));
        bCanSnapTurn = false; // 잠금: 스틱이 복귀하기 전까지 재회전 금지
    }
    else if (FMath::Abs(TurnInput.X) < 0.3f)
    {
        // 스틱이 중앙 근처(30% 미만)로 돌아오면 잠금 해제 → 다음 스냅 턴 허용
        // (뒤의 깨진 주석 원문: "조이스틱 중립 복귀 시 재활성화")
        bCanSnapTurn = true;  // ���̽�ƽ �߸� ���� �� ��Ȱ��ȭ
    }
}