// ============================================================================
// RotatingLightActor.h — 테스트용 회전 광원 액터 선언
// ----------------------------------------------------------------------------
// [역할] 벤치마크 부하 생성기. 레벨에 배치하면 씬의 모든 DirectionalLight
//   (태양광)를 매 프레임 조금씩 회전시킨다. 광원이 움직이면 엔진이 그림자
//   캐시를 재사용하지 못해 동적 그림자를 매 프레임 다시 계산하게 되고,
//   이것이 포탈 컬링 효과를 측정할 때의 "일관된 GPU 부하" 역할을 한다.
// [관계] 포탈 시스템 코드와 직접적인 의존성은 없다. 순수 측정 보조 도구.
//   자기 자신이 광원을 갖는 게 아니라, 레벨에 이미 있는 DirectionalLight
//   액터들을 매 Tick 검색해서 돌리는 방식임에 주의.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/DirectionalLightComponent.h" // (현재 직접 사용처는 없음 — .cpp에서 ADirectionalLight를 씀)
#include "RotatingLightActor.generated.h"

UCLASS()
class SEAMLESS_5_5_API ARotatingLightActor : public AActor
{
    GENERATED_BODY()

public:
    // 생성자. [게임 스레드] Tick 활성화만 수행
    ARotatingLightActor();

    // 게임 시작 시 1회. [게임 스레드] 경과 시간 초기화
    virtual void BeginPlay() override;
    // 매 프레임. [게임 스레드] 씬의 모든 DirectionalLight를 일정 각속도로 회전
    virtual void Tick(float DeltaTime) override;

    // ȸ�� �� �ð� (�⺻ 20��)
    // (위 깨진 한글 주석 원문: "회전 한 바퀴 시간 (기본 20초)")
    // 광원이 360도 한 바퀴 도는 데 걸리는 시간(초). 에디터에서 조절 가능.
    // 작게 줄일수록 빨리 돌아 그림자 갱신 부하가 커진다.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
    float RotationDuration = 20.0f;

private:
    // 누적 경과 시간. BeginPlay에서 0으로 리셋되지만 현재 Tick에서 누적/사용하는
    // 코드는 없음 (과거 구현의 잔재 — 사실상 미사용 변수)
    float ElapsedTime = 0.0f;
};