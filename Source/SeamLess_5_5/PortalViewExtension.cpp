// ============================================================================
// PortalViewExtension.cpp — 포탈 절두체 컬링 구현부
// ----------------------------------------------------------------------------
// [큰 그림] VR에서 포탈 N개를 SceneCapture2D로 렌더하면 캡처마다 씬 전체를
//   다시 그려서 비용이 N배가 된다. 이 파일은 "포탈 개구부를 통해 실제로 보이는
//   영역"만 그리도록, 눈 위치+개구부 4꼭짓점으로 만든 절두체 평면 5장을
//   수정된 엔진 Renderer 모듈의 전역변수(extern RENDERER_API ...)에 매 프레임
//   써 넣는다. 엔진 쪽 컬링 루프가 그 평면 밖 오브젝트를 건너뛴다.
// [스레드 주의] Update*() 3종은 게임 스레드에서, PreRenderViewFamily_RenderThread
//   는 렌더 스레드에서 돈다. 두 스레드가 공유하는 멤버는 전부 DataLock으로 보호.
// [관련 파일] PortalActor.cpp 의 UpdatePortalFrustumData()가 매 Tick 데이터를
//   생산해 UpdatePortalFrustum()으로 넘긴다. 엔진 전역변수 정의는 엔진 수정본
//   (Renderer 모듈) 쪽에 있다 — 여기서는 extern 선언으로 가져다 쓰기만 함.
// ============================================================================

#include "PortalViewExtension.h"
#include "RenderGraphBuilder.h"     // FRDGBuilder (RDG 패스 빌더) 타입 정의
#include "HAL/IConsoleManager.h"    // TAutoConsoleVariable (콘솔 변수 CVar) 정의
#include "RendererInterface.h"      // RENDERER_API 매크로 등 렌더러 모듈 인터페이스
#include "SceneView.h"              // FSceneView / FSceneViewFamily 정의

// 벤치마크용 토글 — 콘솔에서 r.Portal.FrustumCulling 0/1 로 켜고 끄기
// TAutoConsoleVariable = 엔진 콘솔(~키)이나 ini에서 바꿀 수 있는 전역 설정값.
// ECVF_RenderThreadSafe 플래그 덕분에 렌더 스레드에서도 안전하게 읽을 수 있다
// (엔진이 게임/렌더 스레드용 값 복사본을 따로 관리해 줌).
static TAutoConsoleVariable<int32> CVarPortalFrustumCulling(
    TEXT("r.Portal.FrustumCulling"),
    1,
    TEXT("0: Portal Frustum Culling 비활성화 (Baseline)\n1: Portal Frustum Culling 활성화 (기본값)"),
    ECVF_RenderThreadSafe
);

// 생성자. [게임 스레드] FSceneViewExtensions::NewExtension<FPortalViewExtension>()
// 가 호출될 때(보통 GameInstance/PortalActor 초기화 시점) 한 번 실행된다.
// 부모 생성자에 AutoRegister를 넘기면 엔진의 ViewExtension 목록에 자동 등록되어
// 이후 매 프레임 훅 함수들이 불리기 시작한다.
FPortalViewExtension::FPortalViewExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}

// 소멸자. [게임 스레드] 마지막 TSharedPtr 참조가 사라질 때(보통 게임 종료/레벨 전환).
// 엔진 전역변수에 절두체 평면이 남아 있으면, 이 확장이 죽은 뒤에도 엔진 컬링이
// 계속 작동해 화면이 통째로 잘려 나가는 사고가 난다 → 반드시 0으로 리셋.
FPortalViewExtension::~FPortalViewExtension()
{
    // extern 선언: "이 변수의 실체는 다른 모듈(수정된 엔진 Renderer)에 있다"는 뜻.
    // RENDERER_API = DLL 경계를 넘어 접근할 수 있게 하는 내보내기(export) 매크로.
    extern RENDERER_API float GPortalFrustumMaxDistance;
    extern RENDERER_API int32 GPortalFrustumPlaneCount;
    // PlaneCount=0 이면 엔진 쪽 컬링 코드가 "평면 없음 → 컬링 안 함"으로 동작.
    GPortalFrustumMaxDistance = 0.0f;
    GPortalFrustumPlaneCount = 0;
}

// [게임 스레드] 순수가상 함수라 본문은 필수지만 이 프로젝트에서는 할 일이 없어
// 의도적으로 비워 둔 함수. (뷰 패밀리 설정을 바꿀 일이 없음)
void FPortalViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

// ★ 절두체 조립 함수. [렌더 스레드] PreRenderViewFamily_RenderThread()에서만 호출.
// 눈(EyePos)을 꼭짓점으로, 포탈 개구부 사각형을 밑면으로 하는 "무한 피라미드"를
// 평면 5장(옆면 4 + 근평면 1)으로 표현한다. 이 피라미드 밖의 물체는
// 포탈 너머를 캡처할 때 어차피 개구부에 가려 안 보이므로 그릴 필요가 없다.
// FConvexVolume = 평면들로 둘러싸인 볼록 공간. UE가 절두체 컬링 판정
// (IntersectSphere/IntersectBox 등)에 쓰는 표준 자료구조.
void FPortalViewExtension::BuildPortalConvexVolume(
    const FVector& EyePos,
    const FVector Corners[4],
    FConvexVolume& OutVolume)
{
    // 이전에 들어 있던 평면을 모두 비우고 새로 시작
    OutVolume.Planes.Reset();

    // 개구부 4꼭짓점의 평균 = 포탈 중심점. 평면 법선 방향을 판별할 기준점으로 사용
    FVector Center = (Corners[0] + Corners[1] + Corners[2] + Corners[3]) / 4.0f;

    // ── 옆면 4장 생성: 인접한 두 꼭짓점(A,B)과 눈을 지나는 평면 ──
    for (int32 i = 0; i < 4; i++)
    {
        const FVector& A = Corners[i];
        const FVector& B = Corners[(i + 1) % 4]; // (i+1)%4 → 마지막 꼭짓점은 첫 꼭짓점과 연결 (사각형 한 바퀴)

        // 눈→A, 눈→B 방향 단위벡터. GetSafeNormal() = 길이 0이어도 안전하게 정규화
        FVector EdgeA = (A - EyePos).GetSafeNormal();
        FVector EdgeB = (B - EyePos).GetSafeNormal();
        // 두 방향벡터의 외적(CrossProduct) = 두 벡터가 만드는 면에 수직인 벡터
        // → 눈과 변 A-B를 동시에 지나는 평면의 법선이 된다
        FVector Normal = FVector::CrossProduct(EdgeA, EdgeB).GetSafeNormal();

        // 외적 결과의 방향은 꼭짓점 순서(시계/반시계)에 따라 뒤집힐 수 있으므로,
        // "법선이 포탈 중심 쪽을 향하도록" 부호를 통일한다.
        // 내적(DotProduct) < 0 이면 법선이 중심 반대편을 보고 있다는 뜻 → 뒤집기
        FVector ToCenter = (Center - EyePos).GetSafeNormal();
        if (FVector::DotProduct(Normal, ToCenter) < 0.0f)
            Normal = -Normal;

        // FPlane(점, 법선) = 그 점을 지나고 그 법선을 갖는 평면 생성
        OutVolume.Planes.Add(FPlane(A, Normal));
    }

    // ── 5번째 평면: 근평면(near plane) ──
    // 눈에서 포탈 방향으로 10cm 떨어진 지점에, 눈 쪽을 향하는 법선(-PortalForward)으로
    // 평면을 세워 "눈 뒤쪽 공간"을 절두체에서 잘라낸다. (UE 기본 단위는 cm)
    FVector PortalForward = (Center - EyePos).GetSafeNormal();
    OutVolume.Planes.Add(FPlane(EyePos + PortalForward * 10.0f, -PortalForward));
    // Init(): 평면 배열로부터 내부 가속 데이터(PermutedPlanes — SIMD 판정용으로
    // 재배열한 평면들)를 구축. 이걸 안 하면 Intersect* 판정이 제대로 안 됨.
    OutVolume.Init();
}

// 컬링 최대 거리 적응 계산. [렌더 스레드] PreRenderViewFamily_RenderThread()에서
// DataLock을 잡은 상태로 호출됨 (그래서 SceneActorBounds를 락 없이 읽어도 안전).
// 아이디어: 절두체 안에 큰 물체가 있으면 거리 제한을 늘려서 잘리지 않게 하고,
// 작은 물체뿐이면 제한을 짧게 유지해 컬링 효과(부하 절감)를 키운다.
// 이름의 "Lumen"은 원래 Lumen GI 거리 제한 실험에서 출발한 흔적이며, 결과값은
// GPortalFrustumMaxDistance(엔진 쪽 거리 컬링 한계)로 쓰인다.
float FPortalViewExtension::ComputeOptimalLumenDistance(
    const FConvexVolume& PortalVolume,
    const FVector& EyePos)
{
    // 기본 최소 후보값 5m (UE 단위 cm → 500 = 5m)
    float MaxDist = 500.0f;

    // 게임 스레드가 넘겨준 씬 액터 바운드를 전부 순회
    for (const FBoxSphereBounds& Bounds : SceneActorBounds)
    {
        // IntersectSphere: 액터를 감싼 구(중심+반경)가 절두체와 겹치는지 검사
        // → 겹친다 = 포탈 개구부를 통해 보일 가능성이 있는 액터
        if (PortalVolume.IntersectSphere(Bounds.Origin, Bounds.SphereRadius))
        {
            // 액터 크기(지름 = 반경*2)를 거리 후보로 사용 — 큰 액터일수록
            // 멀리까지 그려야 잘림이 안 보인다는 단순 휴리스틱.
            // (주의: 눈에서 액터까지의 실제 거리는 고려하지 않음)
            float Dist = Bounds.SphereRadius * 2.0f;
            MaxDist = FMath::Max(MaxDist, Dist);
        }
    }

    // 최종값을 3m~15m 사이로 강제 제한 (너무 짧으면 눈에 띄게 잘리고,
    // 너무 길면 컬링 효과가 사라지므로)
    return FMath::Clamp(MaxDist, 300.0f, 1500.0f);
}

// ★★ 이 파일의 심장. [렌더 스레드] 매 프레임, 각 뷰 패밀리(메인 카메라 1번 +
// SceneCapture마다 1번씩)가 렌더링되기 직전에 엔진이 자동 호출한다.
// 역할: 게임 스레드가 넣어 둔 절두체 재료(PortalFrustum)를 꺼내 FConvexVolume을
// 조립하고, 평면들을 엔진 전역변수에 복사 → 엔진 컬링 루프가 이를 소비.
void FPortalViewExtension::PreRenderViewFamily_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneViewFamily& InViewFamily)
{
    // 수정된 엔진 Renderer 모듈에 정의된 전역변수 3종을 extern으로 가져옴.
    //   GPortalFrustumPlanes[5]  : 절두체 평면 최대 5장 (옆면4 + 근평면1)
    //   GPortalFrustumPlaneCount : 유효한 평면 개수 (0이면 컬링 꺼짐)
    //   GPortalFrustumMaxDistance: 거리 기반 추가 컬링 한계 (0이면 거리 제한 없음)
    extern RENDERER_API float GPortalFrustumMaxDistance;
    extern RENDERER_API FPlane GPortalFrustumPlanes[5];
    extern RENDERER_API int32 GPortalFrustumPlaneCount;

    // bIsSceneCapture: 이 뷰가 일반 카메라가 아니라 SceneCapture2D(포탈 캡처)의
    // 렌더링인지 표시하는 플래그. Views[0]만 검사해도 되는 이유는 한 뷰 패밀리
    // 안의 뷰들은 같은 출처(같은 캡처 or 같은 카메라)이기 때문.
    if (InViewFamily.Views.Num() > 0 && InViewFamily.Views[0]->bIsSceneCapture)
    {
        // SceneCapture 렌더링 중에는 프러스텀 컬링 반드시 비활성화
        // 메인 카메라가 설정한 플레인이 SceneCapture 렌더링에도 영향을 주기 때문
        // return만 하면 이전 프레임 플레인이 그대로 남아 타겟 레벨 오브젝트 전부 컬링됨
        GPortalFrustumPlaneCount = 0;
        GPortalFrustumMaxDistance = 0.0f;
        return;
    }

    // FScopeLock: 생성 시 뮤텍스를 잠그고 스코프(함수)를 벗어나면 자동으로 풂.
    // 여기서부터는 게임 스레드의 Update*() 함수들과 동시에 실행되지 않음이 보장됨.
    FScopeLock Lock(&DataLock);

    // 벤치마크 토글: r.Portal.FrustumCulling 0 이면 Culling 비활성화
    // GetValueOnRenderThread(): 렌더 스레드 전용 복사본을 읽는 안전한 접근법
    // (게임 스레드에서 CVar를 바꿔도 프레임 경계에서만 반영됨)
    if (CVarPortalFrustumCulling.GetValueOnRenderThread() == 0)
    {
        // 끈 상태(Baseline 측정용) → 엔진 전역을 0으로 리셋해 컬링 완전 해제
        GPortalFrustumMaxDistance = 0.0f;
        GPortalFrustumPlaneCount = 0;
        return;
    }

    // 게임 스레드(PortalActor::UpdatePortalFrustumData)가 이번 프레임에
    // 유효한 절두체 재료를 넣어 줬을 때만 컬링 데이터 생산
    if (PortalFrustum.bIsValid)
    {
        // 눈 위치 + 개구부 4꼭짓점 → 평면 5장짜리 볼록 절두체 조립 (위 함수 참고)
        FConvexVolume PortalVolume;
        BuildPortalConvexVolume(PortalFrustum.EyePosition, PortalFrustum.Corners, PortalVolume);

        // ��� �����͸� ���� ������ ����
        // (위 깨진 한글 주석 원문: "평면 데이터를 엔진 전역에 복사")
        // 조립된 평면들을 엔진 전역 배열로 복사. 배열 크기가 5로 고정이므로
        // 혹시 평면이 더 많아도 5장까지만 잘라서 넘긴다(버퍼 오버런 방지).
        int32 PlaneCount = FMath::Min(PortalVolume.Planes.Num(), 5);
        for (int32 i = 0; i < PlaneCount; i++)
        {
            GPortalFrustumPlanes[i] = PortalVolume.Planes[i];
        }
        // 개수를 마지막에 써 주면 엔진 쪽에서 이 값만큼만 평면을 읽는다
        GPortalFrustumPlaneCount = PlaneCount;

        // �Ÿ� ���ѵ� �Բ� ����
        // (위 깨진 한글 주석 원문: "거리 제한도 함께 설정")
        // 평면 컬링에 더해 "최대 거리" 컬링도 설정. 씬 액터 바운드 정보가 있으면
        // 절두체와 교차하는 액터 크기 기준으로 적응 계산, 없으면 최대치 15m 고정.
        if (SceneActorBounds.Num() > 0)
        {
            float OptimalDistance = ComputeOptimalLumenDistance(PortalVolume, PortalFrustum.EyePosition);
            GPortalFrustumMaxDistance = OptimalDistance;
        }
        else
        {
            GPortalFrustumMaxDistance = 1500.0f;
        }

        // (디버그 로그 제거: 매 렌더 프레임 호출돼 로그가 도배되고 렌더 스레드 부담)
    }
    else
    {
        // 유효한 포탈 절두체가 없는 프레임 → 컬링 해제 (전역 0 리셋)
        GPortalFrustumMaxDistance = 0.0f;
        GPortalFrustumPlaneCount = 0;
    }
}

// [렌더 스레드] 디퍼드 렌더링의 BasePass(씬을 GBuffer 텍스처들에 그리는 1차 패스)
// 직후에 뷰마다 호출되는 훅. 현재는 실질적인 일을 전혀 하지 않는다 —
// SceneCapture 뷰면 return, 아니어도 그냥 함수 끝 (사실상 빈 함수).
// 과거 실험 코드의 흔적이거나, 추후 GBuffer 후처리를 넣기 위한 자리.
void FPortalViewExtension::PostRenderBasePassDeferred_RenderThread(
    FRDGBuilder& GraphBuilder,
    FSceneView& InView,
    const FRenderTargetBindingSlots& RenderTargets,
    TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
    if (InView.bIsSceneCapture) return;
}

// ── 이하 3개: [게임 스레드] 데이터 주입 함수들. 락 잡고 복사만 하는 단순 구조 ──

// 포탈 AABB 목록 주입. [게임 스레드] 호출되긴 하지만 PortalVisibleBounds를
// 읽는 코드가 현재 없어 사실상 미사용 데이터 (확장 여지로 남겨둔 것).
void FPortalViewExtension::UpdatePortalData(const TArray<FBox>& InPortalBounds)
{
    FScopeLock Lock(&DataLock); // 렌더 스레드가 읽는 중이면 끝날 때까지 대기
    PortalVisibleBounds = InPortalBounds;
}

// ★ 절두체 재료 주입. [게임 스레드] PortalActor::UpdatePortalFrustumData()가
// 매 Tick 호출. 여기 들어온 데이터를 다음 렌더 프레임의
// PreRenderViewFamily_RenderThread()가 소비한다 (생산자-소비자 패턴).
void FPortalViewExtension::UpdatePortalFrustum(const FPortalFrustumData& InFrustumData)
{
    FScopeLock Lock(&DataLock);
    PortalFrustum = InFrustumData; // 구조체 통째로 복사 (포인터 공유 없음 → 안전)
}

// 씬 액터 바운드 목록 주입. [게임 스레드] ComputeOptimalLumenDistance()의
// 적응형 거리 계산 재료가 된다. 안 불러주면 거리 제한이 1500 고정으로 동작.
void FPortalViewExtension::UpdateSceneActorBounds(const TArray<FBoxSphereBounds>& InBounds)
{
    FScopeLock Lock(&DataLock);
    SceneActorBounds = InBounds;
}