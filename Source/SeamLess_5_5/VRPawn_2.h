// ============================================================================
// VRPawn_2.h — VR 플레이어 폰(Pawn) 선언
// ----------------------------------------------------------------------------
// [역할] Quest 3용 VR 플레이어 캐릭터. 컴포넌트 구조는
//   캡슐(루트, 충돌 담당) → VROrigin(트래킹 기준점) → VRCamera(HMD 추적 카메라).
//   Enhanced Input으로 조이스틱 이동(Move)·스냅 턴(Turn)을 처리한다.
// [연구와의 관계] .cpp에 r.Portal.ForceScalabilityLevel CVar가 정의되어 있어,
//   BeginPlay에서 그래픽 품질(Engine Scalability)을 강제 고정 → 벤치마크 측정의
//   일관성을 보장한다 (패키지 빌드의 하드웨어 자동감지 덮어쓰기 방지).
// [관계] 포탈 시스템(PortalActor/PortalViewExtension)과 직접 연결되진 않지만,
//   이 폰의 VRCamera(=HMD 눈 위치)가 포탈 절두체의 EyePosition 출처가 된다.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"          // APawn: 플레이어/AI가 빙의(Possess)할 수 있는 액터
#include "Camera/CameraComponent.h"      // UCameraComponent: 시점 카메라
#include "Components/CapsuleComponent.h" // UCapsuleComponent: 캡슐 모양 충돌체
#include "InputActionValue.h"            // Enhanced Input의 입력값 래퍼 (축/벡터 등)
#include "VRPawn_2.generated.h"          // UHT(리플렉션 코드 생성기) 산출물 — 항상 마지막 include

UCLASS()
class SEAMLESS_5_5_API AVRPawn_2 : public APawn
{
    GENERATED_BODY()

public:
    // 생성자. [게임 스레드] 컴포넌트 생성·계층 조립·기본값 설정 (CDO 생성 시에도 호출됨)
    AVRPawn_2();

    // 매 프레임 호출. [게임 스레드] 현재는 부모 호출 외 하는 일 없음 (확장용 자리)
    virtual void Tick(float DeltaTime) override;
    // 플레이어 입력 바인딩. [게임 스레드] 컨트롤러가 이 폰에 빙의할 때 엔진이 1회 호출
    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:
    // 게임 시작/스폰 시 1회 호출. [게임 스레드] 스칼라빌리티 강제 + 입력 매핑 등록
    virtual void BeginPlay() override;

    // �̵� ó��
    // (위 깨진 한글 주석 원문: "이동 처리")
    // 조이스틱 입력(2D 벡터) → 카메라 기준 수평 이동. IA_Move가 Triggered될 때마다 호출
    void Move(const FInputActionValue& Value);

    // ȸ�� ó�� (���� ��)
    // (위 깨진 한글 주석 원문: "회전 처리 (스냅 턴)")
    // 스냅 턴: VR 멀미 방지를 위해 부드러운 회전 대신 45도씩 끊어서 도는 방식
    void Turn(const FInputActionValue& Value);

public:
    // 루트 컴포넌트. 플레이어 몸통 역할의 충돌 캡슐 (벽 통과 방지)
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VR")
    UCapsuleComponent* CapsuleComp;

    // VR 트래킹 공간의 기준점(바닥). HMD/컨트롤러 좌표는 이 컴포넌트 기준 상대좌표로 들어옴
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VR")
    USceneComponent* VROrigin;

    // HMD를 따라다니는 카메라 (bLockToHmd=true로 매 프레임 머리 위치/회전과 동기화)
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VR")
    UCameraComponent* VRCamera;

    // Input Actions (�����Ϳ��� �Ҵ�)
    // (위 깨진 한글 주석 원문: "Input Actions (에디터에서 할당)")
    // Enhanced Input 에셋들 — 코드가 아니라 에디터(블루프린트 디테일 패널)에서
    // 지정한다. 비워 두면 해당 입력이 그냥 동작하지 않음(널 체크로 크래시 방지).
    // IMC = Input Mapping Context: "어떤 물리 버튼/스틱이 어떤 액션인지" 매핑표
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
    class UInputMappingContext* IMC_VR;

    // 이동 액션 (보통 왼쪽 조이스틱, Axis2D)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
    class UInputAction* IA_Move;

    // 회전(스냅 턴) 액션 (보통 오른쪽 조이스틱, Axis2D)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
    class UInputAction* IA_Turn;

    // �̵� �ӵ�
    // (위 깨진 한글 주석 원문: "이동 속도")
    // 초당 이동 거리. UE 단위는 cm → 300 = 3m/s (사람 빠른 걸음 정도)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VR")
    float MoveSpeed = 300.0f;

    // ���� �� ����
    // (위 깨진 한글 주석 원문: "스냅 턴 각도")
    // 스냅 턴 1회당 회전 각도(도 단위)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VR")
    float SnapTurnAngle = 45.0f;

private:
    // 스냅 턴 잠금 플래그. 스틱을 한 번 꺾었을 때 1회만 돌고, 스틱이 중앙 근처로
    // 돌아와야 다시 돌 수 있게 하는 래치(latch). false = 아직 스틱 복귀 전
    bool bCanSnapTurn = true;
};