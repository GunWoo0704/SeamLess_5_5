// ============================================================================
// PortalViewExtension.h — 포탈 절두체(Frustum) 컬링용 SceneViewExtension 선언
// ----------------------------------------------------------------------------
// [역할] VR 다중 포탈 렌더링 최적화의 핵심 클래스. 엔진의 렌더링 파이프라인에
//   "끼어들 수 있는" 공식 확장점인 FSceneViewExtensionBase를 상속하여,
//   매 프레임 렌더 스레드에서 포탈 전용 절두체(눈 위치 + 포탈 개구부 4꼭짓점으로
//   만든 볼록 공간)를 계산하고, 수정된 엔진 Renderer 모듈의 전역변수
//   (GPortalFrustumPlanes 등)에 평면 데이터를 넘긴다. 엔진 쪽 컬링 코드가
//   이 평면들을 읽어 포탈 캡처(SceneCapture) 시 개구부 밖 지오메트리를 걸러낸다.
// [데이터 흐름] PortalActor::UpdatePortalFrustumData() (게임 스레드, 매 Tick)
//   → UpdatePortalFrustum() 으로 FPortalFrustumData 전달 (DataLock으로 보호)
//   → PreRenderViewFamily_RenderThread() (렌더 스레드) 가 읽어서 엔진 전역에 기록.
// [관계] 엔진 수정본(Renderer 모듈)의 extern 전역변수와 한 쌍으로 동작.
//   r.Portal.FrustumCulling CVar(.cpp에 정의)로 켜고 끌 수 있어 벤치마크에 사용.
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"   // FSceneViewExtensionBase: 렌더 파이프라인 훅 제공 베이스 클래스
#include "ConvexVolume.h"         // FConvexVolume: 평면(FPlane)들로 둘러싸인 볼록 공간. 컬링 판정에 사용

// 게임 스레드(PortalActor)가 만들어서 렌더 스레드로 넘기는 "포탈 절두체 재료" 묶음.
// 렌더 스레드에서 이 재료로 실제 FConvexVolume(절두체)을 조립한다.
struct FPortalFrustumData
{
    // 관찰자의 눈(VR이면 HMD 카메라) 월드 좌표. 절두체의 꼭짓점(apex)이 된다.
    FVector EyePosition;
    // 포탈 개구부(사각형 입구)의 4개 모서리 월드 좌표. 눈→각 모서리를 잇는
    // 선들이 절두체의 옆면을 이룬다 (피라미드 모양을 상상하면 됨).
    FVector Corners[4];
    // 포탈 전체를 감싸는 AABB(축 정렬 바운딩 박스). 현재 컬링 계산에는 직접
    // 쓰이지 않고, 필요 시 참고용으로 함께 전달되는 정보.
    FBox    PortalBounds;
    // 이 데이터가 유효한지 여부. 포탈이 화면에 없거나 아직 갱신 전이면 false →
    // 렌더 스레드는 false일 때 컬링을 끈다(전역변수 0으로 리셋).
    bool    bIsValid = false;
};

// SceneViewExtension: 엔진 소스를 직접 고치지 않고도 렌더링 단계 곳곳에
// 콜백(훅)을 끼워 넣을 수 있는 공식 메커니즘. FSceneViewExtensionBase를 상속하고
// FSceneViewExtensions::NewExtension<T>() 로 생성하면 엔진이 매 프레임 자동 호출한다.
// (UObject가 아니라 일반 C++ 클래스라서 GC 대상이 아님 — TSharedPtr로 수명 관리)
class SEAMLESS_5_5_API FPortalViewExtension : public FSceneViewExtensionBase
{
public:
    // 생성자. FAutoRegister는 "엔진의 ViewExtension 목록에 자동 등록해 달라"는
    // 토큰으로, NewExtension<T>() 헬퍼를 통해서만 만들 수 있다 (직접 new 방지 장치).
    FPortalViewExtension(const FAutoRegister& AutoRegister);
    // 소멸자. 엔진 전역변수에 남은 절두체 평면을 0으로 리셋해서,
    // 확장이 사라진 뒤에도 컬링이 계속 걸려 있는 사고를 막는다.
    virtual ~FPortalViewExtension();

    // ── 아래 4개는 ISceneViewExtension 인터페이스의 순수가상 함수라 반드시
    //    override 해야 하지만, 이 프로젝트에서는 쓸 일이 없어 빈 구현으로 둔 것 ──
    // SetupViewFamily: 게임 스레드. 뷰 패밀리(한 프레임에 같이 렌더되는 뷰 묶음,
    // VR은 좌/우 눈 2개) 설정 직후 호출. 여기선 할 일 없음(.cpp에 빈 본문).
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    // SetupView: 게임 스레드. 개별 뷰(눈 하나)마다 호출. 사용 안 함.
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    // BeginRenderViewFamily: 게임 스레드. 렌더 커맨드 전송 직전 호출. 사용 안 함.
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
    // PreRenderView_RenderThread: 렌더 스레드. 개별 뷰 렌더 직전 호출. 사용 안 함.
    virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override {}

    // ★ 핵심 훅. [렌더 스레드] 뷰 패밀리 렌더링이 시작되기 직전에 엔진이 호출.
    // 여기서 포탈 절두체를 조립해 엔진 전역변수(GPortalFrustumPlanes 등)에 기록한다.
    // FRDGBuilder = RDG(Render Dependency Graph) 빌더. GPU 작업 그래프를 쌓는
    // 객체인데, 우리는 GPU 패스를 추가하지 않으므로 받기만 하고 쓰지 않는다.
    virtual void PreRenderViewFamily_RenderThread(
        FRDGBuilder& GraphBuilder,
        FSceneViewFamily& InViewFamily) override;

    // [렌더 스레드] 디퍼드 렌더링의 BasePass(GBuffer 채우기) 직후 호출되는 훅.
    // 현재는 SceneCapture 뷰면 바로 return 하는 빈 껍데기 — 과거 실험의 흔적이거나
    // 추후 포스트 처리 삽입을 위한 자리로 보면 된다.
    virtual void PostRenderBasePassDeferred_RenderThread(
        FRDGBuilder& GraphBuilder,
        FSceneView& InView,
        const FRenderTargetBindingSlots& RenderTargets,
        TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

    // ── 게임 스레드 → 렌더 스레드 데이터 전달용 공개 함수 3종 ──
    // 모두 [게임 스레드]에서 PortalActor 등이 매 프레임 호출하며, 내부에서
    // DataLock(뮤텍스)을 잡고 멤버에 복사만 한다. 렌더 스레드가 같은 락을 잡고
    // 읽으므로 두 스레드가 동시에 같은 데이터를 만지는 경합(race)을 막는다.
    void UpdatePortalData(const TArray<FBox>& InPortalBounds);          // 포탈 AABB 목록 (현재 미사용)
    void UpdatePortalFrustum(const FPortalFrustumData& InFrustumData);  // ★ 절두체 재료 (핵심)
    void UpdateSceneActorBounds(const TArray<FBoxSphereBounds>& InBounds); // 씬 액터 바운드 (거리 제한 계산용)

private:
    // [렌더 스레드] 눈 위치 + 개구부 4꼭짓점으로 FConvexVolume(절두체) 조립.
    // 옆면 4장 + 근평면 1장 = 총 5장의 평면으로 이루어진 피라미드 모양 공간.
    void BuildPortalConvexVolume(
        const FVector& EyePos,
        const FVector Corners[4],
        FConvexVolume& OutVolume);

    // [렌더 스레드] 절두체와 교차하는 씬 액터들의 크기를 보고
    // "컬링을 적용할 최대 거리"를 적응적으로 계산 (300~1500cm 범위로 클램프).
    float ComputeOptimalLumenDistance(
        const FConvexVolume& PortalVolume,
        const FVector& EyePos);

    // ── 아래 멤버들은 전부 DataLock으로 보호되는 공유 데이터 ──
    TArray<FBox>             PortalVisibleBounds; // 포탈 AABB 목록. 받기만 하고 현재 아무도 안 읽음(사실상 미사용)
    TArray<FBoxSphereBounds> SceneActorBounds;    // 씬 액터들의 (중심+박스+구 반경) 바운드. 거리 제한 계산에 사용
    FPortalFrustumData       PortalFrustum;       // 게임 스레드가 넣어준 최신 절두체 재료
    FCriticalSection         DataLock;            // 게임/렌더 스레드 간 동기화용 뮤텍스(임계 영역)
};