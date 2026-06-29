// ============================================================================
// RotatingLightActor.cpp — 테스트용 회전 광원 구현부
// ----------------------------------------------------------------------------
// 매 Tick마다 씬의 모든 DirectionalLight(태양광) 액터를 찾아 피치(Pitch) 축으로
// 조금씩 회전시킨다. 해가 하늘을 가로지르는 것과 같은 효과 → 동적 그림자가
// 매 프레임 재계산되도록 강제하여 벤치마크용 GPU 부하를 만든다.
// 이 파일의 모든 코드는 게임 스레드에서 실행된다.
// ============================================================================

#include "RotatingLightActor.h"
#include "Kismet/GameplayStatics.h"   // UGameplayStatics::GetAllActorsOfClass (액터 검색)
#include "Engine/DirectionalLight.h"  // ADirectionalLight (방향광 액터 클래스)

// 생성자. [게임 스레드] 매 프레임 Tick이 돌도록 활성화만 한다.
ARotatingLightActor::ARotatingLightActor()
{
    PrimaryActorTick.bCanEverTick = true;
}

// 게임 시작 시 1회. [게임 스레드] 경과 시간 리셋.
// (ElapsedTime은 현재 다른 곳에서 안 쓰여서 사실상 의미 없는 초기화)
void ARotatingLightActor::BeginPlay()
{
    Super::BeginPlay();
    ElapsedTime = 0.0f;
}

// 매 프레임 호출. [게임 스레드]
// 씬 전체에서 DirectionalLight를 검색해 프레임당 회전량만큼 돌린다.
// DeltaTime을 곱하므로 프레임레이트가 달라도 회전 속도(초당 각도)는 일정하다.
void ARotatingLightActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // RotationDuration이 0(또는 음수/비정상)이면 360/0 = inf → NaN 회전 → 크래시.
    // KINDA_SMALL_NUMBER = UE가 정의한 아주 작은 양수(1e-4). "사실상 0" 판정용
    if (!FMath::IsFinite(RotationDuration) || RotationDuration <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    // RotationDuration초에 360도 도는 초당 회전 각도 계산
    // 예) 20초 설정 → 360/20 = 초당 18도
    float DegreesPerSecond = 360.0f / RotationDuration;
    // 이번 프레임에 돌 각도 = 초당 각도 × 이번 프레임 소요 시간
    float DeltaAngle = DegreesPerSecond * DeltaTime;

    // DeltaTime 스파이크 등으로 비정상 값이 나오면 회전하지 않음
    if (!FMath::IsFinite(DeltaAngle))
    {
        return;
    }

    // 씬의 모든 DirectionalLight 액터 수집.
    // 주의: GetAllActorsOfClass는 월드의 액터를 전수 순회하는 비싼 함수인데
    // 매 Tick 호출하고 있다 → BeginPlay에서 한 번만 찾아 캐싱하는 게 정석.
    // (광원 1~2개짜리 테스트 씬이라 실측 영향은 미미해서 그대로 둔 것)
    TArray<AActor*> DirectionalLights;
    UGameplayStatics::GetAllActorsOfClass(
        GetWorld(),
        ADirectionalLight::StaticClass(),
        DirectionalLights);

    for (AActor* Light : DirectionalLights)
    {
        // (아래 깨진 한글 주석 원문: "절대값 누적 대신 매 프레임 조금씩 로컬 회전")
        // FRotator(Pitch, Yaw, Roll) — 피치(고도각)를 -DeltaAngle만큼:
        // 해가 지평선에서 떠올라 하늘을 가로질러 넘어가는 움직임이 된다.
        // AddActorLocalRotation = 현재 방향 기준 상대 회전(누적 방식)
        // ���밪 ���� ��� �� ������ ���ݾ� ���� ȸ��
        Light->AddActorLocalRotation(FRotator(-DeltaAngle, 0.0f, 0.0f));
    }
}