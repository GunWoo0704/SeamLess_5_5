// ═══════════════════════════════════════════════════════════════════════════
// PortalActor.cpp — VR 다중 포탈 액터의 구현부
//
// 전체 흐름 한눈에 보기:
//   BeginPlay()  : RT 생성 → SceneCapture 화질/노출 설정 → 머티리얼 연결
//                  → 스케줄러(Phase2) 등록. (레벨 로드는 여기서 안 함!)
//   Tick()       : ① r.Portal.Enable 토글 확인 → ② 거리 기반 레벨 로드/언로드
//                  → ③ 활성 레벨 게이팅 → ④ Phase1 토글 감지 → ⑤ 절두체 데이터 갱신
//                  → ⑥ 캡처 여부 결정(거리/로드상태/Phase2 예산/Phase3 풀)
//                  → ⑦ SceneCapture 위치를 플레이어 머리 회전에 맞춰 배치
//   EndPlay()    : 스케줄러/RT풀/레벨 참조 정리
//
// 협력 클래스: UPortalScheduler(Phase2 예산), UPortalRTPool(Phase3 풀),
//              UPortalLevelManager(레벨 공유), FPortalViewExtension(절두체 컬링)
// 콘솔 변수(CVar)로 각 최적화를 켜고 끄며 벤치마크 A/B 비교가 가능하다.
// ═══════════════════════════════════════════════════════════════════════════

#include "PortalActor.h"
#include "PortalLevelManager.h"       // 레벨 공유 서브시스템 (Acquire/Release)
#include "PortalScheduler.h"          // Phase2 프레임 예산 스케줄러
#include "PortalRTPool.h"             // Phase3 RT 공유 풀
#include "Kismet/GameplayStatics.h"   // GetPlayerPawn 등 전역 헬퍼 함수 모음
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"   // 플레이어 머리(HMD) 위치/회전을 읽을 카메라
#include "GameFramework/Pawn.h"       // 플레이어 폰 (위치/텔레포트 대상)
// IXRTrackingSystem.h: 사용처 없음(주석에만 'HMD' 단어 1번). HeadMountedDisplay 모듈
// 의존성을 안 더해도 되도록 include만 제거. 추후 실제 XR API 필요 시 Build.cs에
// "HeadMountedDisplay" 모듈을 추가하고 이 헤더를 되살릴 것.
#include "Engine/Engine.h"            // GEngine (화면 디버그 메시지 출력)
#include "Engine/World.h"             // UWorld (월드/서브시스템 접근)
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PostProcessVolume.h" // 스텐실 PP 바인딩 시 검색 대상
#include "Engine/DirectionalLight.h"  // 태양광 정리(EnsureSingleDirectionalLight)용
#include "Components/LightComponent.h"
#include "EngineUtils.h"              // TActorIterator (월드 액터 순회)
#include "DrawDebugHelpers.h"         // DrawDebugLine/Sphere 등 디버그 드로잉
#include "HAL/IConsoleManager.h"      // CVar(콘솔 변수) 정의/조회 API

// ───────────────────────────────────────────────────────────────
// 벤치마크용 토글 — 콘솔에서 r.Portal.Enable 0/1 로 켜고 끄기
//   0: 포탈 전체 비활성 (SceneCapture 중단 + 메시 숨김 + Frustum 갱신 중단)
//      → Baseline 측정용
//   1: 포탈 정상 동작 (기본값)
//
// FrustumCulling 토글은 PortalViewExtension.cpp 에 별도 존재:
//   r.Portal.FrustumCulling 0/1
//
// 3가지 벤치마크 시나리오:
//   ① r.Portal.Enable 0                           → Baseline
//   ② r.Portal.Enable 1, r.Portal.FrustumCulling 0 → 최적화 미적용
//   ③ r.Portal.Enable 1, r.Portal.FrustumCulling 1 → 최적화 적용 (저희 기여)
// ───────────────────────────────────────────────────────────────
// TAutoConsoleVariable = 게임 콘솔(~키)에서 조작 가능한 전역 변수를 등록하는 UE 템플릿.
//   인자: (콘솔 이름, 기본값, 도움말 텍스트, 플래그). static이라 이 cpp 파일 전용.
//   코드에서는 .GetValueOnGameThread()로 현재 값을 읽는다.
static TAutoConsoleVariable<int32> CVarPortalEnable(
    TEXT("r.Portal.Enable"),
    1,
    TEXT("0: Portal 전체 비활성화 (벤치마크 Baseline)\n")
    TEXT("1: Portal 정상 동작 (기본값)"),
    ECVF_Default
);

// ───────────────────────────────────────────────────────────────
// Phase 1: SceneCapture2D 공식 최적화 토글
//   0: Phase 1 OFF — vanilla SceneCapture2D 동작
//   1: Phase 1 ON  — ShowOnlyActors / 비싼 ShowFlags OFF / LODDistanceFactor 상향 등
//
// Phase 1은 UE5가 SceneCapture2D에 기본 제공하는 공식 최적화만 사용 (엔진 수정 X).
// 메인 씬(Showcase) 액터를 포탈 캡처에서 제외하고, 포탈 뷰에서 안 보이는 PP 효과를 끔.
// ───────────────────────────────────────────────────────────────
static TAutoConsoleVariable<int32> CVarPortalPhase1(
    TEXT("r.Portal.Phase1"),
    1,
    TEXT("0: Phase 1 (SceneCapture2D 공식 최적화) OFF\n")
    TEXT("1: Phase 1 ON (ShowOnlyActors + ShowFlags off + LOD 강제, 기본값)"),
    ECVF_Default
);

// ───────────────────────────────────────────────────────────────
// Phase 2: Frame Budget Allocator (라운드로빈 캡처)
//   0: 모든 포탈이 매 프레임 캡처 (Phase 1까지의 동작)
//   1: 등록된 포탈 중 1개만 프레임당 캡처 (캡처 비용 ≒ 1포탈 비용)
//
// 다른 포탈들은 이전 프레임의 RT를 그대로 사용. N포탈이면 각 포탈은
// 1/N 확률로 갱신 → 시각적으로 약간 stale하지만 정적 씬에서는 거의 안 보임.
// ───────────────────────────────────────────────────────────────
static TAutoConsoleVariable<int32> CVarPortalPhase2(
    TEXT("r.Portal.Phase2"),
    1,
    TEXT("0: Phase 2 Frame Budget OFF (모든 포탈 매 프레임 캡처)\n")
    TEXT("1: Phase 2 ON (라운드로빈으로 프레임당 1개만 캡처, 기본값)"),
    ECVF_Default
);

// ───────────────────────────────────────────────────────────────
// Phase 3: RT Memory Pool with LRU + Adaptive Resolution
//   0: 각 포탈 자기 HotRT(1920×1080) 보유 — VRAM N에 비례
//   1: K(=4)개 공유 풀 + 포탈별 ColdRT(저해상도) — VRAM 상수
//
// Hot 포탈: 풀 RT에 캡처, 머티리얼이 풀 RT 읽음
// Cold 포탈: 캡처 안 함, 머티리얼이 자기 ColdRT 읽음
// Hot→Cold 전환 시: 마지막으로 ColdRT 한 번 캡처해서 fallback 갱신
// ───────────────────────────────────────────────────────────────
static TAutoConsoleVariable<int32> CVarPortalPhase3(
    TEXT("r.Portal.Phase3"),
    0,  // 기본 OFF: 포탈마다 다른 레벨을 보는 경우 공유 풀이 화면을 뒤섞으므로.
        // 여러 포탈이 "같은 레벨 하나"를 공유하는 벤치마크에서만 켤 것.
    TEXT("0: Phase 3 RT Pool OFF (포탈마다 자기 RT, 기본값)\n")
    TEXT("1: Phase 3 ON (K개 공유 풀 — 단일 레벨 공유 시에만 권장)"),
    ECVF_Default
);

// 포탈 캡처(갱신) 거리 컷오프(cm). 이 안의 포탈만 캡처 대상이 됨.
//   멀거나 시야 밖 포탈이 안 움직이면 이 값을 늘리면 됨 (기본 2500 = 25m).
static TAutoConsoleVariable<float> CVarPortalCaptureRange(
    TEXT("r.Portal.CaptureRange"),
    2500.0f,
    TEXT("포탈 캡처 거리 컷오프(cm). 이 거리 이내 포탈만 갱신. 기본 2500(25m)."),
    ECVF_Default
);

// 생성자 — 컴포넌트 트리 조립과 기본값 설정.
// UE에서 컴포넌트는 반드시 생성자에서 CreateDefaultSubobject로 만들어야
// 에디터 디테일 패널에 계층 구조로 표시되고 인스턴스마다 자동 복제된다.
APortalActor::APortalActor()
{
    // 이 액터가 Tick(매 프레임 갱신)을 받도록 허용 — 포탈 로직 전부가 Tick에 있음
    PrimaryActorTick.bCanEverTick = true;

    // 루트 컴포넌트: 모든 자식 컴포넌트의 기준 좌표계가 됨
    PortalRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PortalRoot"));
    RootComponent = PortalRoot;

    // 포탈 "스크린" 메시 생성 후 루트에 부착
    PortalMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PortalMesh"));
    PortalMesh->SetupAttachment(PortalRoot);
    // Movable = 런타임에 움직일 수 있는 모빌리티 (Static이면 이동 시 경고/렌더 문제)
    PortalMesh->SetMobility(EComponentMobility::Movable);
    // 평면(Plane) 메시는 기본이 바닥에 눕는 방향이라 Roll 90도로 세워서 "문"처럼 만듦
    PortalMesh->SetRelativeRotation(FRotator(0.0f, 0.0f, 90.0f));
    // 메시 자체는 충돌 없음 — 플레이어가 화면을 뚫고 지나갈 수 있어야 함 (감지는 TriggerVolume이 담당)
    PortalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // 포탈 너머를 찍는 가상 카메라 생성
    SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
    SceneCapture->SetupAttachment(PortalRoot);
    // Lumen GI 누적을 위해 bCaptureEveryFrame은 Tick에서 동적으로 관리
    // (근거리: true → Lumen Surface Cache 축적, 원거리: false → 성능)
    SceneCapture->bCaptureEveryFrame = false;
    // 카메라가 움직일 때 자동 캡처하는 기능도 끔 (캡처 시점을 우리가 완전히 통제)
    SceneCapture->bCaptureOnMovement = false;
    // SCS_SceneColorHDR = 톤매핑 전의 HDR 씬 컬러를 캡처 (밝기 정보 보존)
    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
    // true면 캡처 안 하는 프레임에도 렌더 상태(TAA/Lumen 히스토리)를 유지 → 재개 시 깜빡임 방지
    SceneCapture->bAlwaysPersistRenderingState = true;

    // 텔레포트 감지용 박스 트리거 생성
    TriggerVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerVolume"));
    TriggerVolume->SetupAttachment(PortalRoot);
    // 박스 절반 크기(cm): 앞뒤 50, 좌우 200, 상하 200 — 포탈 면을 감싸는 얇은 판
    TriggerVolume->SetBoxExtent(FVector(50.0f, 200.0f, 200.0f));
    // QueryOnly = 물리 충돌(밀어내기) 없이 겹침 감지만 수행
    TriggerVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    TriggerVolume->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
    // 모든 채널에 대해 "Overlap" 응답 → 무엇이 닿아도 막지 않고 이벤트만 발생
    TriggerVolume->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
    // 겹침 이벤트(OnComponentBeginOverlap) 발생 허용
    TriggerVolume->SetGenerateOverlapEvents(true);

    // 캡처 위치 기본값 0 — 에디터에서 레벨별로 지정함
    TargetCaptureLocation = FVector::ZeroVector;
}

// 게임 시작 시 1회 호출되는 초기화 함수 (엔진이 자동 호출).
// RT 생성 → SceneCapture 화질/노출 설정 → 머티리얼 연결 → 스케줄러 등록 순서로 진행.
void APortalActor::BeginPlay()
{
    // Super:: = 부모(AActor)의 BeginPlay 먼저 실행 — 오버라이드 시 관례적으로 필수
    Super::BeginPlay();

    // 디테일 창 체크박스 상태를 CVar에 반영 (Play 시작 시 1회)
    ApplyFeatureToggles();

    // 렌더 파이프라인 확장 생성·등록 — 이후 절두체 컬링 데이터를 렌더 스레드로 전달하는 통로
    ViewExtension = FSceneViewExtensions::NewExtension<FPortalViewExtension>();

    // 포탈 메시: Custom Depth + Stencil 활성화
    // (Custom Depth 버퍼에 메시를 따로 그려서, PP 머티리얼이 "여기가 포탈"임을 식별 가능하게)
    PortalMesh->SetRenderCustomDepth(true);
    PortalMesh->SetCustomDepthStencilValue(PortalStencilValue);

    // 렌더 타겟 생성 (Phase 3 OFF일 때 또는 fallback용)
    // 에디터에서 RT 에셋을 직접 할당했으면 그걸 쓰고, 없으면 여기서 코드로 생성
    if (!RenderTarget)
    {
        // NewObject = 런타임에 UObject 생성 (this를 Outer로 → 이 액터와 수명 연동)
        RenderTarget = NewObject<UTextureRenderTarget2D>(this);
        // 1920x1080 해상도로 텍스처 메모리 초기화
        RenderTarget->InitAutoFormat(1920, 1080);
        RenderTarget->bAutoGenerateMips = false;          // 밉맵 자동 생성 끔 (포탈은 화면 크기 그대로 표시라 불필요)
        RenderTarget->RenderTargetFormat = RTF_RGBA16f;   // 16bit float RGBA — HDR 밝기 저장 가능
    }

    // Phase 3: ColdRT 생성 — 작은 해상도, 포탈별 영구 보유
    // (Hot 슬롯을 못 받은 포탈이 마지막 모습이라도 보여주기 위한 저해상도 백업)
    if (!ColdRenderTarget)
    {
        ColdRenderTarget = NewObject<UTextureRenderTarget2D>(this);
        ColdRenderTarget->InitAutoFormat(ColdRTWidth, ColdRTHeight);
        ColdRenderTarget->bAutoGenerateMips = false;
        ColdRenderTarget->RenderTargetFormat = RTF_RGBA16f;
        // 픽셀당 8바이트(RGBA16f) 기준 메모리 사용량을 KB로 로그 출력
        UE_LOG(LogTemp, Warning, TEXT("[Portal-Phase3] ColdRT created %dx%d (%.2f KB)"),
            ColdRTWidth, ColdRTHeight,
            (ColdRTWidth * ColdRTHeight * 8.0f) / 1024.0f);
    }

    // 리프로젝션 UV 오프셋이 텍스처 가장자리를 넘어가도 타일링(짜깁기)되지 않도록 Clamp.
    // (Wrap이면 UV가 0~1 밖으로 나가는 순간 반대편이 말려 들어와 화면이 조각남)
    if (RenderTarget)     { RenderTarget->AddressX = TA_Clamp;     RenderTarget->AddressY = TA_Clamp; }
    if (ColdRenderTarget) { ColdRenderTarget->AddressX = TA_Clamp; ColdRenderTarget->AddressY = TA_Clamp; }

    // 캡처 결과를 어디에 기록할지 연결 (카메라 → RT)
    SceneCapture->TextureTarget = RenderTarget;
    SceneCapture->FOVAngle = 104.0f;  // Quest 3 HMD FOV
    // ── ShowFlags: 포탈 캡처 뷰에서 어떤 렌더 기능을 켤지 하나씩 명시 ──
    // (SceneCapture는 메인 뷰와 별개 ShowFlags를 가지므로 직접 켜야 함)
    SceneCapture->ShowFlags.SetAtmosphere(true);          // 대기(하늘) 렌더
    SceneCapture->ShowFlags.SetSkyLighting(true);         // 스카이라이트(하늘빛 간접광)
    SceneCapture->ShowFlags.SetFog(true);                 // 일반 안개
    SceneCapture->ShowFlags.SetVolumetricFog(true);       // 볼류메트릭 안개 (※ Phase1 ON 시 다시 꺼짐)
    SceneCapture->ShowFlags.SetDynamicShadows(true);      // 동적 그림자
    SceneCapture->ShowFlags.SetGlobalIllumination(true);  // 전역 조명(GI) 전체 스위치
    SceneCapture->ShowFlags.SetLumenGlobalIllumination(true);  // ← 핵심: 이게 없으면 ShouldRenderLumenDiffuseGI가 false 반환
    SceneCapture->ShowFlags.SetLighting(true);            // 라이팅 자체 (끄면 Unlit)
    SceneCapture->ShowFlags.SetPostProcessing(true);      // 포스트 프로세스 (노출 등)
    SceneCapture->ShowFlags.SetAmbientOcclusion(true);    // 주변광 차폐(AO)
    SceneCapture->ShowFlags.SetLumenReflections(true);    // Lumen 반사
    SceneCapture->ShowFlags.SetIndirectLightingCache(true); // 간접광 캐시
    SceneCapture->ShowFlags.SetSkyLighting(true);         // (위 SkyLighting과 중복 호출 — 무해함)

    // Lumen GI 방식 명시적 설정 — FinalPostProcessSettings.DynamicGlobalIlluminationMethod == Lumen 조건 충족
    // (bOverride_X = true 는 "이 PP 항목을 볼륨 기본값 대신 내 값으로 덮어쓴다"는 스위치)
    SceneCapture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
    SceneCapture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;

    // 노출 설정 — SceneCapture는 기본적으로 auto exposure가 꺼져 있어서 어둡게 나옴
    // Min/Max를 같은 값으로 잠그면 씬 밝기와 어긋남 → 적절한 범위로 열어줌
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    SceneCapture->PostProcessSettings.AutoExposureMethod = AEM_Histogram;   // 히스토그램 기반 자동 노출 (메인 뷰와 같은 방식)
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
    SceneCapture->PostProcessSettings.AutoExposureMinBrightness = 0.18f;  // 실내 씬 기준
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
    SceneCapture->PostProcessSettings.AutoExposureMaxBrightness = 8.0f;
    // SpeedUp/Down: SceneCapture는 매 프레임 재설정되므로 빠른 적응이 필요
    // (값이 클수록 밝기 변화에 빨리 적응 — 10이면 거의 즉시)
    SceneCapture->PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
    SceneCapture->PostProcessSettings.AutoExposureSpeedUp = 10.0f;
    SceneCapture->PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
    SceneCapture->PostProcessSettings.AutoExposureSpeedDown = 10.0f;

    // 클립 플레인: Tick에서 모드에 따라 동적으로 설정
    // (클립 플레인 = 카메라 앞 특정 평면 이전의 지오메트리를 잘라내는 기능. LinkedPortal 모드에서만 사용)
    SceneCapture->bEnableClipPlane = false;

    // 포탈 메시 머티리얼 설정 — 에셋(PortalMaterial)을 직접 쓰지 않고
    // 다이내믹 인스턴스(MID)를 만들어, 런타임에 RT 텍스처 파라미터를 바꿀 수 있게 함
    if (PortalMaterial)
    {
        DynamicMaterial = UMaterialInstanceDynamic::Create(PortalMaterial, this);
        // 머티리얼 그래프의 "RenderTargetLeft/Right" 텍스처 파라미터에 우리 RT를 연결
        // (VR 좌/우 눈 분리를 염두에 둔 파라미터명이지만 현재는 둘 다 같은 RT 사용)
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), RenderTarget);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), RenderTarget);
        // 포탈 메시의 0번 슬롯에 이 머티리얼 장착 → 화면에 캡처 영상이 표시됨
        PortalMesh->SetMaterial(0, DynamicMaterial);
    }

    // 스텐실 PP는 일단 비활성화 - 메시 머티리얼로 포탈 표시 확인 후 활성화
    // BindStencilMaterialToVolume();

    // 트리거 겹침 이벤트에 우리 콜백 연결.
    // AddDynamic = UFUNCTION으로 등록된 함수를 델리게이트(이벤트 목록)에 바인딩하는 매크로
    TriggerVolume->OnComponentBeginOverlap.AddDynamic(this, &APortalActor::OnOverlapBegin);

    // Phase 1: ShowFlags / LOD / PostProcess 적용 (ShowOnlyActors는 레벨 로드 후 OnTargetLevelLoaded에서)
    // GetValueOnGameThread() = 게임 스레드에서 CVar의 현재 값을 읽기
    const bool bPhase1 = (CVarPortalPhase1.GetValueOnGameThread() != 0);
    ApplyPhase1ShowFlags(bPhase1);
    LastPhase1State = bPhase1 ? 1 : 0;   // 다음 틱부터 토글 변화 감지를 위한 기준값 저장

    // Phase 2: 스케줄러에 자신을 등록
    // GetSubsystem<T>() = 월드에 1개씩 존재하는 싱글톤 서비스 객체를 가져오는 UE API.
    // if (X* p = ...) 형태 = 포인터를 얻으면서 동시에 null 체크하는 C++ 관용구
    if (UPortalScheduler* Sched = GetWorld()->GetSubsystem<UPortalScheduler>())
    {
        Sched->RegisterPortal(this);
    }

    // 레벨은 BeginPlay에서 전부 로드하지 않는다.
    // 거리 기반으로 Tick의 UpdateLevelResidency()가 가까운 포탈만 동적 로드.
    // (포탈마다 다른 레벨일 때 N개를 동시에 띄우면 프레임이 무너지므로)
}

// 게임 종료/액터 파괴 시 호출되는 정리 함수 (엔진이 자동 호출).
// BeginPlay에서 등록한 것들(스케줄러/RT풀/레벨 참조)을 반드시 해제해야
// 파괴된 포탈을 다른 시스템이 계속 참조하는 사고(댕글링)를 막는다.
void APortalActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 월드가 유효할 때만 서브시스템 정리 (teardown 중 GetWorld() null 방어)
    if (UWorld* World = GetWorld())
    {
        // Phase 2: 스케줄러에서 자신을 제거 (PIE 종료/액터 파괴 시)
        if (UPortalScheduler* Sched = World->GetSubsystem<UPortalScheduler>())
        {
            Sched->UnregisterPortal(this);
        }

        // Phase 3: RT Pool에서도 자신을 제거
        if (UPortalRTPool* RTPool = World->GetSubsystem<UPortalRTPool>())
        {
            RTPool->UnregisterPortal(this);
        }

        // ── Phase 0 (refcount): 레벨 참조 해제 ──
        // 이 포탈이 잡고 있던 TargetLevel의 refcount를 1 감소.
        // 같은 레벨을 공유하던 마지막 포탈이면 즉시 unload되어 메모리 회수.
        if (!TargetLevel.IsNull() && StreamingLevel)
        {
            if (UPortalLevelManager* Mgr = World->GetSubsystem<UPortalLevelManager>())
            {
                Mgr->ReleaseLevel(TargetLevel);
            }
        }
    }
    // 로컬 캐시 비우기 (포인터 null 처리 + 액터 목록 비움)
    StreamingLevel = nullptr;
    StreamingLevelActors.Reset();

    Super::EndPlay(EndPlayReason);
}

// 스텐실 PP 머티리얼을 레벨의 무한 PostProcessVolume에 끼워 넣는 함수.
// 스텐실 기반 포탈 표시(메시 머티리얼 대신 화면 PP로 합성) 실험용이며,
// 현재는 BeginPlay에서 호출이 주석 처리되어 있어 실행되지 않는다.
void APortalActor::BindStencilMaterialToVolume()
{
    // 필요한 에셋이 안 채워졌으면 경고만 남기고 포기
    if (!StencilPostProcessMaterial || !RenderTarget)
    {
        UE_LOG(LogTemp, Warning, TEXT("PortalActor: StencilPostProcessMaterial or RenderTarget not set"));
        return;
    }

    // PP 머티리얼 다이내믹 인스턴스 생성 + 포탈 RT를 텍스처 파라미터로 주입
    StencilPPInstance = UMaterialInstanceDynamic::Create(StencilPostProcessMaterial, this);
    StencilPPInstance->SetTextureParameterValue(FName("PortalRT"), RenderTarget);

    // 레벨에 있는 PostProcessVolume 찾기
    // TActorIterator<T> = 월드의 해당 타입 액터를 전부 순회하는 이터레이터
    for (TActorIterator<APostProcessVolume> It(GetWorld()); It; ++It)
    {
        APostProcessVolume* PPVolume = *It;
        // bUnbound = "Infinite Extent" 체크된 볼륨 (맵 전체에 적용되는 PP)만 대상
        if (PPVolume && PPVolume->bUnbound)
        {
            // WeightedBlendables = PP 볼륨에 머티리얼을 가중치와 함께 끼워 넣는 배열 (1.0 = 100% 적용)
            PPVolume->Settings.WeightedBlendables.Array.Add(
                FWeightedBlendable(1.0f, StencilPPInstance));

            UE_LOG(LogTemp, Log, TEXT("PortalActor: Stencil PP bound to existing PostProcessVolume"));
            return;   // 첫 번째 무한 볼륨에만 붙이고 종료
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("PortalActor: No unbound PostProcessVolume found! Place one in the level with Infinite Extent enabled."));
}

// TargetLevel을 실제로 메모리에 올리는 함수.
// 직접 LoadLevelInstance를 부르지 않고 PortalLevelManager(공유 관리자)를 거친다.
// 호출 시점: UpdateLevelResidency()가 "플레이어가 가까워졌다"고 판단했을 때.
void APortalActor::LoadTargetLevel()
{
    // PortalLevelManager 서브시스템 통해 공유 로드.
    // 같은 TargetLevel을 가진 다른 PortalActor가 이미 로드했다면
    // 그 인스턴스를 재사용 (메모리/렌더 비용 N배 절감).
    UPortalLevelManager* Mgr = GetWorld()->GetSubsystem<UPortalLevelManager>();
    if (!Mgr)
    {
        // 매니저가 없으면 로드 자체가 불가능 — 에러 로그 후 중단
        UE_LOG(LogTemp, Error, TEXT("[Portal] PortalLevelManager subsystem not found"));
        return;
    }

    // TargetViewTransform.Rotation + TargetLevelRotation을 합성해
    // 스트리밍 레벨 전체를 회전시킴.
    // 예) TargetLevelRotation.Yaw = 90 → Downtown_Alley가 Z축 기준 90도 돌아서 스폰됨
    // 주의: 같은 레벨을 공유하므로 두 번째 이후 포탈의 위치/회전은 무시되고
    //       첫 번째 포탈이 정한 값이 적용됨.
    // FQuat(쿼터니언) 곱 = 회전 합성. A * B는 "B 먼저, 그 다음 A" 순서로 적용됨
    const FQuat ComposedQ =
        TargetLevelRotation.Quaternion() * TargetViewTransform.GetRotation();
    const FRotator SpawnRot = ComposedQ.Rotator();   // 쿼터니언 → 오일러 각(FRotator)로 변환

    // ── Phase 0 (refcount): AcquireLevel — 마지막 포탈이 사라질 때 자동 unload ──
    StreamingLevel = Mgr->AcquireLevel(
        TargetLevel,
        TargetViewTransform.GetLocation(),
        SpawnRot);

    // AcquireLevel이 nullptr를 반환했다 = 레벨 에셋 경로가 잘못됐거나 스폰 실패
    if (!StreamingLevel)
    {
        UE_LOG(LogTemp, Warning, TEXT("PortalActor: TargetLevel load failed"));
        return;
    }

    bLevelAcquired = true;  // 참조 보유 표시 (중복 Acquire/Release 방지)

    // 다른 포탈이 이미 로드를 끝낸 상태면 콜백을 즉시 한 번 실행,
    // 아직 로드 중이면 델리게이트 바인딩해서 완료될 때 호출되도록.
    // GetLoadedLevel() = 로드 완료된 ULevel 포인터. 아직 로드 중이면 nullptr
    if (StreamingLevel->GetLoadedLevel())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Portal] TargetLevel already loaded — invoking callback immediately"));
        OnTargetLevelLoaded();
    }
    else
    {
        // AddUniqueDynamic = 같은 함수가 이미 바인딩돼 있으면 중복 추가하지 않는 안전한 바인딩
        StreamingLevel->OnLevelLoaded.AddUniqueDynamic(this, &APortalActor::OnTargetLevelLoaded);
        UE_LOG(LogTemp, Warning, TEXT("[Portal] Bound OnLevelLoaded delegate (SpawnRot=%s)"), *SpawnRot.ToString());
    }
}

// ───────────────────────────────────────────────────────────────
// 거리 기반 동적 레벨 상주 (히스테리시스)
//   포탈마다 다른 레벨일 때 N개를 동시에 띄우면 프레임 붕괴 → 가까운 것만 로드.
//   LoadDist 이내로 들어오면 Acquire, UnloadDist 밖으로 나가면 Release.
//   (UnloadDist > LoadDist 로 히스테리시스 → 경계에서 로드/언로드 깜빡임 방지)
// ───────────────────────────────────────────────────────────────
static TAutoConsoleVariable<float> CVarPortalLevelLoadDist(
    TEXT("r.Portal.LevelLoadDist"),
    2800.0f,
    TEXT("이 거리(cm) 이내로 들어오면 포탈 레벨을 로드 (기본 2800=28m). CaptureRange보다 약간 크게"),
    ECVF_Default
);
static TAutoConsoleVariable<float> CVarPortalLevelUnloadDist(
    TEXT("r.Portal.LevelUnloadDist"),
    3800.0f,
    TEXT("이 거리(cm) 밖으로 나가면 포탈 레벨을 언로드 (기본 3800=38m). LoadDist보다 커야 함"),
    ECVF_Default
);

// 매 틱 호출되어 "이 포탈의 레벨을 메모리에 둘지 말지"를 거리로 결정.
// 가까우면 LoadTargetLevel(), 멀면 ReleaseTargetLevel() — 호출자는 Tick().
void APortalActor::UpdateLevelResidency()
{
    // 링크드 포탈 모드(TargetLevel 없음)는 레벨 스트리밍 안 함
    // IsNull() = TSoftObjectPtr에 에셋 경로 자체가 비어 있는지 검사
    if (TargetLevel.IsNull()) return;

    UWorld* World = GetWorld();
    if (!World) return;   // 월드가 없으면(파괴 중 등) 아무것도 못 함

    // 플레이어 폰(0번 = 로컬 플레이어) 가져오기
    APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0);
    if (!Pawn)
    {
        // 플레이어 폰이 없으면(예: Simulate 모드) 거리 판정 불가 →
        // 그냥 레벨을 로드해 두고 언로드는 하지 않음 (검은 포탈 방지).
        if (!bLevelAcquired) LoadTargetLevel();
        return;
    }

    // 플레이어 ↔ 포탈 직선거리(cm)와 로드/언로드 임계값을 CVar에서 읽기
    const float Dist = FVector::Dist(Pawn->GetActorLocation(), GetActorLocation());
    const float LoadDist = CVarPortalLevelLoadDist.GetValueOnGameThread();
    float UnloadDist = CVarPortalLevelUnloadDist.GetValueOnGameThread();
    // 안전: 언로드 거리는 항상 로드 거리보다 크게(히스테리시스 보장)
    // 둘이 같거나 역전되면 경계에서 로드↔언로드가 매 틱 반복(thrash)되므로 강제 보정
    UnloadDist = FMath::Max(UnloadDist, LoadDist + 100.0f);

    if (!bLevelAcquired && Dist <= LoadDist)
    {
        // 가까워짐 → 로드
        LoadTargetLevel();
    }
    else if (bLevelAcquired && Dist > UnloadDist)
    {
        // 멀어짐 → 언로드 (RenderTarget은 그대로 둬 마지막 캡처 유지)
        ReleaseTargetLevel();
    }
}

// 레벨 참조를 반납하는 함수 — 멀어졌을 때 UpdateLevelResidency()가 호출.
// refcount가 0이 되면 PortalLevelManager가 실제 언로드를 수행한다.
void APortalActor::ReleaseTargetLevel()
{
    // 애초에 참조를 잡은 적 없으면 반납할 것도 없음 (중복 Release 방지)
    if (!bLevelAcquired) return;

    if (UWorld* World = GetWorld())
    {
        if (UPortalLevelManager* Mgr = World->GetSubsystem<UPortalLevelManager>())
        {
            // 매니저에게 "나는 이 레벨 그만 쓴다"고 통보 → refcount 1 감소
            if (!TargetLevel.IsNull()) Mgr->ReleaseLevel(TargetLevel);
        }
    }

    // 콜백 바인딩 해제 + 캐시 정리 (StreamingLevelActors는 더 이상 유효하지 않음)
    // RemoveDynamic = AddDynamic으로 걸었던 바인딩 제거 — 언로드 후 콜백 오발사 방지
    if (StreamingLevel)
    {
        StreamingLevel->OnLevelLoaded.RemoveDynamic(this, &APortalActor::OnTargetLevelLoaded);
    }
    StreamingLevel = nullptr;
    StreamingLevelActors.Reset();
    bLevelAcquired = false;
    bLevelActive = true;  // 다음에 다시 로드되면 기본 visible 상태와 일치시킴

    // RenderTarget/ColdRenderTarget은 일부러 비우지 않음 → 마지막 캡처가 그대로 보임

    UE_LOG(LogTemp, Warning, TEXT("[Portal] 레벨 언로드 (거리 멀어짐): %s"), *GetName());
}

// "활성 레벨 게이팅"의 실행부 — 레벨을 통째로 보이거나 숨기고 Tick도 끈다.
// 매 틱 Tick()이 스케줄러의 ShouldLevelBeActive() 결과를 들고 호출한다.
void APortalActor::SetLevelActive(bool bActive)
{
    if (bActive == bLevelActive) return;   // 상태 변화 시에만 토글 (thrash/깜빡임 방지)
    bLevelActive = bActive;                // 새 상태 기억 (다음 틱 비교 기준)

    // 레벨 전체 가시성: 렌더/그림자/Lumen/DistanceField 씬에 포함·제외.
    // 토글이 드물게(시선 이동 시에만) 일어나므로 비동기 가시화여도 무방.
    if (StreamingLevel)
    {
        StreamingLevel->SetShouldBeVisible(bActive);
    }

    // 액터 Tick on/off — 비활성 레벨의 게임 스레드 비용 제거.
    // (틱 불가 액터는 영향 없음, 틱 가능 액터만 토글됨)
    for (AActor* Actor : StreamingLevelActors)
    {
        // IsValid() = null이거나 파괴 예약(PendingKill)된 액터를 걸러내는 표준 검사
        if (!IsValid(Actor)) continue;
        Actor->SetActorTickEnabled(bActive);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[Portal] SetLevelActive(%d): %s"), bActive ? 1 : 0, *GetName());
}

// M2: 회전 리프로젝션 토글 (벤치마크 A/B용). 0이면 오프셋 0 → 순수 stale 표시.
static TAutoConsoleVariable<int32> CVarPortalReproject(
    TEXT("r.Portal.Reproject"),
    1,
    TEXT("0: 리프로젝션 OFF (stale RT 그대로)\n")
    TEXT("1: 회전 리프로젝션 ON (머리 회전 델타로 UV 워프, 기본값)"),
    ECVF_Default
);

// 리프로젝션 UV 오프셋 상한. 낮을수록 늘어남↓(대신 머리 추종↓), 높을수록 추종↑(늘어남↑).
static TAutoConsoleVariable<float> CVarPortalReprojMaxUV(
    TEXT("r.Portal.ReprojMaxUV"),
    0.12f,
    TEXT("회전 리프로젝션 UV 오프셋 최대 크기(0~0.5). 기본 0.12. 화면 늘어남이 심하면 낮추세요."),
    ECVF_Default
);

void APortalActor::UpdateReprojection(UCameraComponent* Camera, bool bCapturedThisFrame)
{
    if (!DynamicMaterial) return;

    const bool bEnabled = (CVarPortalReproject.GetValueOnGameThread() != 0);
    const FRotator HeadRot = Camera ? Camera->GetComponentRotation() : FRotator::ZeroRotator;

    // 방금 캡처했거나(=최신) 리프로젝션 꺼짐 → 오프셋 0, 기준 회전 갱신
    if (bCapturedThisFrame || !bEnabled)
    {
        CaptureViewRotation = HeadRot;
        DynamicMaterial->SetVectorParameterValue(FName("ReprojUVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
        return;
    }

    // 캡처 시점 대비 머리 회전 델타 → UV 오프셋 (rotation-only 근사).
    // 포탈 캡처 FOV(104도) 기준으로 각도를 화면 비율로 정규화. 부호/축은 머티리얼에서 맞춤.
    const float DeltaYaw   = FMath::FindDeltaAngleDegrees(CaptureViewRotation.Yaw,   HeadRot.Yaw);
    const float DeltaPitch = FMath::FindDeltaAngleDegrees(CaptureViewRotation.Pitch, HeadRot.Pitch);

    // FOV로 정규화 + 과도한 워프 방지 상한(±0.25). 너무 크면 Clamp 가장자리가 늘어나 보임.
    const float FOV = 104.0f;
    const float MaxUV = FMath::Clamp(CVarPortalReprojMaxUV.GetValueOnGameThread(), 0.0f, 0.5f);
    const float UVx = FMath::Clamp(DeltaYaw   / FOV, -MaxUV, MaxUV);
    const float UVy = FMath::Clamp(DeltaPitch / FOV, -MaxUV, MaxUV);

    DynamicMaterial->SetVectorParameterValue(FName("ReprojUVOffset"), FLinearColor(UVx, UVy, 0.f, 0.f));
}

// 스트리밍 레벨 로드 완료 시 1회 호출되는 콜백 (OnLevelLoaded 델리게이트, 또는
// 이미 로드된 레벨을 재사용할 때 LoadTargetLevel이 직접 호출).
// 레벨 안 액터 목록을 캐싱하고, Phase1 ShowOnlyActors와 라이트 정리를 적용한다.
void APortalActor::OnTargetLevelLoaded()
{
    // 콜백이 왔어도 만약을 위해 로드 결과 재확인 (null이면 아직 준비 안 됨)
    ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
    if (!LoadedLevel) return;

    // SceneCapture가 스트리밍 레벨을 볼 수 있도록 명시적으로 Visible 설정
    StreamingLevel->SetShouldBeVisible(true);

    // 레벨 안 모든 액터를 우리 캐시 배열에 복사 (이후 ShowOnlyActors/바운드 수집에 사용)
    StreamingLevelActors.Reset();
    for (AActor* Actor : LoadedLevel->Actors)
    {
        if (!IsValid(Actor)) continue;   // 파괴됐거나 null인 항목은 제외
        StreamingLevelActors.Add(Actor);
    }

    UE_LOG(LogTemp, Warning, TEXT("[Portal] TargetLevel loaded! Actors: %d | Level: %s"),
        StreamingLevelActors.Num(),
        *LoadedLevel->GetPathName());

    // Phase 1: 이제 액터 리스트가 채워졌으니 ShowOnlyActors 적용 가능
    const bool bPhase1 = (CVarPortalPhase1.GetValueOnGameThread() != 0);
    ApplyPhase1ShowOnlyActors(bPhase1);

    // 스트리밍 레벨이 자체 디렉셔널 라이트를 들고 들어오므로, 월드에 1개만 남기고 정리
    EnsureSingleDirectionalLight();
}

// 월드에 디렉셔널 라이트를 1개만 남기는 최적화 토글.
//   기본 0(OFF): 각 포탈이 서로 다른 레벨을 띄울 때 레벨별 태양을 끄면 어두워지므로 끔.
//   1(ON): 모든 포탈이 "같은 레벨 하나"를 공유하는 벤치마크에서만 켜서 Lights 비용 절감.
static TAutoConsoleVariable<int32> CVarPortalSingleDirLight(
    TEXT("r.Portal.SingleDirLight"),
    0,
    TEXT("0: 끔(기본) — 레벨별 디렉셔널 라이트 유지\n")
    TEXT("1: 켬 — 월드에 디렉셔널 라이트 1개만 남기고 나머지 끔 (단일 레벨 공유 시)"),
    ECVF_Default
);

// 월드의 디렉셔널 라이트(태양)를 1개만 남기는 정리 함수.
// 스트리밍 레벨이 자기 태양을 들고 들어와 태양이 2개+가 되는 상황을 막는다.
// OnTargetLevelLoaded()에서 호출되지만, CVar 기본값이 0이라 평소엔 즉시 반환.
void APortalActor::EnsureSingleDirectionalLight()
{
    // 기본 OFF. 서로 다른 레벨을 쓰는 경우 켜면 레벨 조명이 사라지므로 opt-in.
    if (CVarPortalSingleDirLight.GetValueOnGameThread() == 0) return;

    UWorld* World = GetWorld();
    if (!World) return;

    // 월드의 모든 ADirectionalLight 액터 수집
    TArray<AActor*> Lights;
    UGameplayStatics::GetAllActorsOfClass(World, ADirectionalLight::StaticClass(), Lights);
    if (Lights.Num() <= 1) return;  // 이미 1개 이하면 할 일 없음

    // 내가 스폰한 스트리밍 레벨의 ULevel 포인터 (라이트 소속 비교용)
    ULevel* StreamedLevel = StreamingLevel ? StreamingLevel->GetLoadedLevel() : nullptr;

    // 남길 라이트(Keep) 선정: 스트리밍 레벨이 아닌(=메인) 라이트를 우선.
    ADirectionalLight* Keep = nullptr;
    for (AActor* A : Lights)
    {
        // Cast<T> = UE의 안전한 다운캐스트. 타입이 다르면 nullptr 반환
        ADirectionalLight* DL = Cast<ADirectionalLight>(A);
        if (!IsValid(DL)) continue;
        // GetLevel() = 이 액터가 소속된 ULevel. 스트리밍 레벨 소속이 "아니면" 메인 레벨 라이트
        if (DL->GetLevel() != StreamedLevel) { Keep = DL; break; }
    }
    // 메인에 없으면 첫 번째 유효 라이트를 남김
    if (!Keep)
    {
        for (AActor* A : Lights)
        {
            if (ADirectionalLight* DL = Cast<ADirectionalLight>(A))
            {
                if (IsValid(DL)) { Keep = DL; break; }
            }
        }
    }

    // Keep을 제외한 나머지 디렉셔널 라이트는 끈다 (라이트 컴포넌트 비가시화 → 라이팅 기여 제거)
    // 액터를 파괴하지 않고 끄기만 하므로 나중에 되살릴 수 있음
    int32 Disabled = 0;
    for (AActor* A : Lights)
    {
        ADirectionalLight* DL = Cast<ADirectionalLight>(A);
        if (!IsValid(DL) || DL == Keep) continue;   // Keep으로 뽑힌 라이트는 건너뜀
        if (ULightComponent* LC = DL->GetLightComponent())
        {
            LC->SetVisibility(false);   // 비가시화 = 라이팅 계산에서 제외
            Disabled++;
        }
    }

    UE_LOG(LogTemp, Warning,
        TEXT("[Portal] EnsureSingleDirectionalLight: 1개 유지, %d개 끔 (total=%d)"),
        Disabled, Lights.Num());
}

// 스트리밍 레벨 액터들의 바운딩 박스(공간 차지 범위)를 모아 ViewExtension에 전달.
// FrustumCulling이 "어떤 액터가 개구부 밖인지" 판단할 때 쓰는 데이터.
// ※ 주의: 현재 이 함수는 아무 데서도 호출되지 않음 — Tick() 안에 같은 로직이 인라인 중복.
void APortalActor::UpdateStreamingLevelBounds()
{
    // ViewExtension이 아직 안 만들어졌으면(BeginPlay 전) 보낼 곳이 없음
    if (!ViewExtension.IsValid()) return;

    TArray<FBoxSphereBounds> StreamingBounds;
    for (AActor* Actor : StreamingLevelActors)
    {
        // 파괴됐거나 루트 컴포넌트 없는(=공간을 차지하지 않는) 액터는 제외
        if (!IsValid(Actor) || !Actor->GetRootComponent()) continue;
        // GetComponentsBoundingBox(true) = 모든 컴포넌트를 포함한 월드 공간 AABB 계산
        FBox Box = Actor->GetComponentsBoundingBox(true);
        // 크기가 0인(빈) 박스는 컬링에 무의미하므로 제외
        if (Box.IsValid && Box.GetExtent().SizeSquared() > 0.0f)
            StreamingBounds.Add(FBoxSphereBounds(Box));
    }

    // 렌더 스레드 쪽으로 바운드 목록 전달
    ViewExtension->UpdateSceneActorBounds(StreamingBounds);

    UE_LOG(LogTemp, Log, TEXT("PortalActor: StreamingLevel bounds updated: %d actors"),
        StreamingBounds.Num());
}

// ═══════════════════════════════════════════════════════════════
// Tick — 매 프레임 호출되는 포탈의 메인 루프 (이 클래스의 심장부).
// 순서: 토글 확인 → 레벨 상주 → 게이팅 → Phase1 감지 → 절두체 갱신
//       → 캡처 여부 결정(거리/Phase2/Phase3) → 캡처 카메라 배치.
// ═══════════════════════════════════════════════════════════════
void APortalActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // ── 방어 코드 ──────────────────────────────────────────────
    // 핵심 컴포넌트/월드가 유효하지 않으면 이번 틱은 통째로 스킵.
    // (Live Coding 재인스턴싱, 액터 파괴 진행 중 등 비정상 상태에서의 크래시 방지)
    if (!IsValid(SceneCapture) || !IsValid(PortalMesh) || !GetWorld())
    {
        return;
    }

    // ── 벤치마크 토글 처리 ─────────────────────────────────────
    // r.Portal.Enable 0 일 때 포탈 렌더/캡처/프러스텀 갱신 전부 중단
    // 상태 변경 시에만 컴포넌트 가시성을 토글 (매 프레임 SetVisibility 방지)
    const bool bPortalEnabled = (CVarPortalEnable.GetValueOnGameThread() != 0);
    // ※ static 지역 변수 = 모든 APortalActor 인스턴스가 공유하는 변수 1개
    //   (포탈이 여러 개면 처음 Tick 도는 포탈만 변화를 감지하는 잠재적 문제 있음 — 멤버 변수가 더 안전)
    static bool bLastPortalEnabled = true;
    if (bPortalEnabled != bLastPortalEnabled)
    {
        // SetVisibility(b, true) — 두 번째 인자 true는 자식 컴포넌트까지 전파한다는 뜻
        if (PortalMesh) PortalMesh->SetVisibility(bPortalEnabled, true);
        if (SceneCapture)
        {
            SceneCapture->SetVisibility(bPortalEnabled, true);
            if (!bPortalEnabled) SceneCapture->bCaptureEveryFrame = false;   // OFF면 캡처도 즉시 중단
        }
        bLastPortalEnabled = bPortalEnabled;

        UE_LOG(LogTemp, Warning, TEXT("[Portal] r.Portal.Enable = %d"), bPortalEnabled ? 1 : 0);
    }

    if (!bPortalEnabled)
    {
        // 포탈 OFF 상태 — 디버그 라인만 정리하고 나머지 로직 전부 스킵
        if (bDebugLinesDrawn)
        {
            // FlushPersistentDebugLines = persistent로 그린 디버그 선을 전부 지우는 엔진 함수
            FlushPersistentDebugLines(GetWorld());
            bDebugLinesDrawn = false;
        }
        return;   // ← Baseline 측정 모드: 여기서 끝 (캡처/레벨/절두체 갱신 없음)
    }

    // ── 거리 기반 동적 레벨 로드/언로드 ──────────────────────
    // 가까운 포탈의 레벨만 상주시켜, 포탈 N개여도 동시 로드 레벨 수를 제한.
    UpdateLevelResidency();

    // ── 활성 레벨 게이팅 ─────────────────────────────────────
    // 모여 있는 다른-레벨 다중 포탈: 우선순위 상위 N개 레벨만 렌더+Tick,
    // 나머지는 숨기고 Tick 꺼서 CPU·GPU 동시 절감. (포탈은 마지막 캡처 유지)
    {
        UPortalScheduler* Sched = GetWorld()->GetSubsystem<UPortalScheduler>();
        // 아직 한 번도 캡처 못 한 포탈은 게이팅 무시하고 강제 활성 → 검은 포탈 방지.
        // (|| 단락 평가: 앞 조건이 true면 뒤의 ShouldLevelBeActive는 아예 호출 안 됨)
        const bool bActive = !bHasCapturedOnce || !Sched || Sched->ShouldLevelBeActive(this);
        SetLevelActive(bActive);
    }

    // ── Phase 1 런타임 토글 감지 ─────────────────────────────
    // r.Portal.Phase1 콘솔 변경 시 실시간 반영 (벤치마크 A/B 비교용)
    const int32 CurPhase1 = CVarPortalPhase1.GetValueOnGameThread();
    if (CurPhase1 != LastPhase1State)
    {
        const bool bPhase1 = (CurPhase1 != 0);
        ApplyPhase1ShowFlags(bPhase1);
        ApplyPhase1ShowOnlyActors(bPhase1);
        LastPhase1State = CurPhase1;
        UE_LOG(LogTemp, Warning, TEXT("[Portal-Phase1] Toggle = %d"), CurPhase1);
    }

    // 디버그 레이: 켜면 한 번 그리고 유지, 끄면 제거
    // (persistent 라인이라 매 프레임 다시 그릴 필요 없음 — bDebugLinesDrawn 플래그로 1회만)
    if (bDebugLumenRays && !bDebugLinesDrawn)
    {
        APawn* DebugPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
        // FindComponentByClass<T> = 액터에 붙은 컴포넌트 중 해당 타입을 검색 (없으면 nullptr)
        UCameraComponent* DebugCamera = DebugPawn
            ? DebugPawn->FindComponentByClass<UCameraComponent>()
            : nullptr;
        DrawLumenDebug(DebugCamera);
        bDebugLinesDrawn = true;
    }
    else if (!bDebugLumenRays && bDebugLinesDrawn)
    {
        // 체크박스를 껐으면 그렸던 선들을 지움
        FlushPersistentDebugLines(GetWorld());
        bDebugLinesDrawn = false;
    }

    // ViewExtension 없이는 절두체 컬링 데이터를 보낼 수 없으므로 이후 로직 스킵
    if (!ViewExtension.IsValid()) return;

    // StreamingLevel 바운드 수집
    // 매 틱 레벨 액터들의 바운딩 박스를 다시 계산해 ViewExtension에 전달
    // (UpdateStreamingLevelBounds()와 같은 내용이 인라인으로 들어와 있음)
    if (StreamingLevel && StreamingLevel->GetLoadedLevel() && ViewExtension.IsValid())
    {
        ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();

        // 캐시가 비어 있으면(콜백을 놓쳤거나 공유 레벨 재사용) 여기서 늦게라도 채움
        if (StreamingLevelActors.Num() == 0)
        {
            for (AActor* Actor : LoadedLevel->Actors)
            {
                if (!IsValid(Actor)) continue;
                StreamingLevelActors.Add(Actor);
            }
        }

        if (StreamingLevelActors.Num() > 0)
        {
            // 각 액터의 월드 공간 바운딩 박스를 모음 (빈 박스는 제외)
            TArray<FBoxSphereBounds> StreamingBounds;
            for (AActor* Actor : StreamingLevelActors)
            {
                if (!IsValid(Actor) || !Actor->GetRootComponent()) continue;
                FBox Box = Actor->GetComponentsBoundingBox(true);
                if (Box.IsValid && Box.GetExtent().SizeSquared() > 0.0f)
                    StreamingBounds.Add(FBoxSphereBounds(Box));
            }

            if (StreamingBounds.Num() > 0)
            {
                // 렌더 스레드에서 컬링 판정에 쓸 바운드 목록 갱신
                ViewExtension->UpdateSceneActorBounds(StreamingBounds);
            }
        }
    }

    // 포탈 개구부 4꼭짓점 + 눈 위치를 렌더 스레드로 전달 (FrustumCulling 데이터 생산)
    UpdatePortalFrustumData();

    // 카메라(HMD 머리) 확보 — 스케줄러 캐시 경유(프레임당 1회 조회). 뷰어 기준점.
    UCameraComponent* Camera = GetViewCamera();

    // 카메라가 있을 때만 포탈 통과 감지 + 거리 계산.
    // 없으면(예: Simulate 모드) DistToPortal=0(=가까움)으로 둬서 캡처는 계속 진행 → 검은 포탈 방지.
    const bool bHasViewer = (Camera != nullptr);
    if (bHasViewer)
    {
        // VR에서는 Overlap이 안 잡히는 경우가 많아서 Tick에서 직접 감지
        CheckPortalCrossing(Camera);
    }
    // 뷰어가 없으면 거리 0으로 간주 → 아래 거리 조건을 항상 통과 (캡처 유지)
    const float DistToPortal = bHasViewer
        ? FVector::Dist(Camera->GetComponentLocation(), GetActorLocation())
        : 0.0f;

    // 이 거리(cm) 이내 포탈만 캡처 대상. r.Portal.CaptureRange 로 조절(기본 2500=25m).
    // 멀거나 시야 밖 포탈도 갱신되게 하려면 이 값을 늘리면 됨.
    const float CaptureRange = CVarPortalCaptureRange.GetValueOnGameThread();
    // bShouldCapture = "이번 프레임에 캡처할까?" — 아래에서 여러 조건이 이 값을 깎아내림
    bool bShouldCapture = (DistToPortal < CaptureRange);

    // 레벨 스트리밍 모드인데 아직 로드 안 끝났으면 캡처 금지.
    // (안 그러면 SceneCapture가 제자리에서 메인 씬을 찍어 포탈에 숲이 잠깐 뜸)
    // 이 동안 포탈은 RenderTarget의 마지막 캡처를 그대로 유지.
    if (!TargetLevel.IsNull() && (!StreamingLevel || !StreamingLevel->GetLoadedLevel()))
    {
        bShouldCapture = false;   // 조건: 스트리밍 모드인데 레벨이 아직 안 떴음
    }

    // 비활성(숨김) 레벨은 캡처해도 내용이 없으므로 캡처 금지 → 마지막 캡처 유지.
    if (!bLevelActive)
    {
        bShouldCapture = false;   // 조건: 게이팅으로 레벨이 숨겨진 상태
    }

    // ── Phase 2: Frame Budget Allocator ───────────────────────────
    // 스케줄러가 "내 차례 아님"이라고 하면 캡처 스킵 → 이전 RT 그대로 사용
    if (bShouldCapture && CVarPortalPhase2.GetValueOnGameThread() != 0)
    {
        if (UPortalScheduler* Sched = GetWorld()->GetSubsystem<UPortalScheduler>())
        {
            // 스케줄러에 "이번 프레임 내가 캡처해도 돼?" 질의 (예산 BudgetCount개 안에 들었는지)
            const bool bMyTurn = Sched->ShouldCaptureThisFrame(this);
            if (!bMyTurn)
            {
                bShouldCapture = false;  // 이번 프레임은 RT 재사용
            }
        }
    }

    // ── Phase 3: RT Memory Pool ──────────────────────────────────
    // 우선순위 기반으로 K개 슬롯에 들어간 포탈만 풀 RT 받음.
    // Cold 포탈은 자기 ColdRT를 머티리얼로 표시.
    // TargetRT = 이번 프레임에 "캡처를 기록하고 화면에 표시할" 텍스처
    UTextureRenderTarget2D* TargetRT = RenderTarget;  // 기본값 (Phase 3 OFF)
    bool bIsHot = true;
    if (CVarPortalPhase3.GetValueOnGameThread() != 0)
    {
        if (UPortalRTPool* RTPool = GetWorld()->GetSubsystem<UPortalRTPool>())
        {
            // ★ 중요: Phase2 스케줄러가 이번 프레임 캡처를 승인한 포탈(bShouldCapture)만
            //   공유 풀 Hot 슬롯을 받을 수 있다. 승인 안 된 포탈이 풀 슬롯을 읽으면
            //   그 슬롯엔 다른 포탈이 캡처한 내용이 들어 있어 화면이 뒤섞인다(머리 흔들 때 증상).
            //   캡처 안 하는 포탈은 자기 ColdRT(자기 마지막 내용)만 표시한다.
            // 풀에 "내 우선순위가 이 정도인데 Hot 슬롯 줄래?" 요청 (거절되면 nullptr)
            UTextureRenderTarget2D* HotRT = bShouldCapture
                ? RTPool->RequestHotRT(this, ComputePortalPriority())
                : nullptr;

            if (HotRT)
            {
                // Hot 슬롯 받음 → 풀 RT에 캡처하고 그 RT를 표시
                TargetRT = HotRT;
                bIsHot = true;
                LastHotRT = HotRT;   // 다음 프레임 Hot→Cold 전환 감지용으로 기억
            }
            else
            {
                // Cold → 캡처 안 함, 자기 ColdRT(자기 내용) 표시 → 포탈 간 뒤섞임 없음
                bIsHot = false;
                bShouldCapture = false;

                // Hot→Cold 전환 시 마지막으로 자기 ColdRT에 백업 (자기 내용 보존)
                // WasHotLastFrame = "직전 프레임엔 Hot이었나?" → 전환되는 그 한 프레임에만 백업 실행
                if (LastHotRT && RTPool->WasHotLastFrame(this))
                {
                    DoFinalColdCapture();
                    LastHotRT = nullptr;
                }
                TargetRT = ColdRenderTarget;
            }
        }
    }

    // SceneCapture 타겟 및 머티리얼 동적 갱신
    // (값이 바뀌었을 때만 대입 — 불필요한 렌더 상태 변경 방지)
    if (SceneCapture->TextureTarget != TargetRT)
    {
        SceneCapture->TextureTarget = TargetRT;
    }
    // 포탈 표면 머티리얼도 같은 RT를 읽도록 텍스처 파라미터 갱신
    if (DynamicMaterial)
    {
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), TargetRT);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), TargetRT);
    }

    // 레벨이 준비됐는지 (스트리밍 모드면 로드 완료, LinkedPortal 모드면 항상 ok)
    const bool bLevelReady = TargetLevel.IsNull()
        || (StreamingLevel && StreamingLevel->GetLoadedLevel());

    // 아직 한 번도 캡처 못 한 포탈은 예산/게이팅 무시하고 강제 캡처 → 검은 포탈 방지.
    if (!bHasCapturedOnce && bLevelReady)
    {
        bShouldCapture = true;
    }

    // ★ 최종 결정 적용: 이 값이 true인 동안 엔진이 매 프레임 자동으로 씬을 캡처한다
    SceneCapture->bCaptureEveryFrame = bShouldCapture;

    // 실제로 캡처가 일어나면(레벨 준비 완료) "내용 있음"으로 표시 → 이후 게이팅 허용
    if (bShouldCapture && bLevelReady)
    {
        bHasCapturedOnce = true;
    }

    // M2: 회전 리프로젝션 — 캡처 안 한 프레임엔 머리 회전 델타로 UV 워프해 stale 완화
    UpdateReprojection(Camera, bShouldCapture);

    // ── 캡처 카메라 배치 (모드 1: TargetLevel 스트리밍) ──
    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        SceneCapture->FOVAngle = 104.0f;          // Quest 3 FOV 유지
        SceneCapture->bEnableClipPlane = false;   // 별도 레벨이라 가릴 게 없음 → 클립 불필요

        // TargetCaptureLocation = 레벨 안 카메라 위치 (에디터에서 직접 설정)
        // TargetCaptureRotation = 카메라 회전 오프셋. 플레이어 머리 회전과 합성됨.
        //   - ZeroRotator: 순수 플레이어 머리 회전만 사용 (기본 동작)
        //   - 값 있음: TargetCaptureRotation * PlayerRot 순서로 quaternion 합성
        // TargetViewTransform = 레벨 스폰 위치 (BeginPlay에서만 사용)
        // 카메라 없으면(Simulate) 회전 0으로 폴백 → 크래시 방지하고 캡처는 진행
        FRotator PlayerRot = Camera ? Camera->GetComponentRotation() : FRotator::ZeroRotator;
        FRotator FinalCaptureRot;
        if (TargetCaptureRotation.IsNearlyZero())
        {
            // 오프셋이 0이면 합성 생략 — 플레이어 머리 회전 그대로 사용
            FinalCaptureRot = PlayerRot;
        }
        else
        {
            // 쿼터니언 곱으로 회전 합성 (오일러 각 덧셈은 짐벌락 문제가 있어 쿼터니언 사용)
            const FQuat CombinedQ = TargetCaptureRotation.Quaternion() * PlayerRot.Quaternion();
            FinalCaptureRot = CombinedQ.Rotator();
        }
        // 캡처 카메라를 레벨 안 지정 위치에 두고, 플레이어가 고개 돌리는 대로 같이 회전
        // → 포탈이 "창문"처럼 너머 풍경이 시선을 따라 움직이는 효과
        SceneCapture->SetWorldLocationAndRotation(TargetCaptureLocation, FinalCaptureRot);

        // 디버그: 매 60프레임마다 상태 출력 (bDebugLumenRays 켰을 때만 — 평소 로그 도배 방지)
        if (bDebugLumenRays)
        {
            // static 카운터로 60프레임에 1번만 로그 (매 프레임 출력하면 로그 폭주)
            static int32 DebugFrame = 0;
            if (++DebugFrame % 60 == 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("[Portal] SceneCapture pos: %s | LoadedLevel actors: %d"),
                    *TargetCaptureLocation.ToString(),
                    StreamingLevel->GetLoadedLevel()->Actors.Num());
                UE_LOG(LogTemp, Warning, TEXT("[Portal] LevelSpawn(TargetViewTransform): %s"),
                    *TargetViewTransform.GetLocation().ToString());
            }
        }
    }
    // ── 캡처 카메라 배치 (모드 2: LinkedPortal) ──
    else if (LinkedPortal)
    {
        // LinkedPortal 모드: 클립 플레인 활성화 (포탈 너머만 캡처)
        // (같은 레벨이라 링크드 포탈 뒤의 벽 등이 캡처에 끼어드는 것을 평면으로 잘라냄)
        SceneCapture->bEnableClipPlane = true;
        UpdateSceneCapture();
    }
}

// 포탈 개구부 절두체(눈 위치 → 포탈 4꼭짓점으로 만들어지는 피라미드) 데이터를
// 계산해 ViewExtension(렌더 스레드)으로 전달. r.Portal.FrustumCulling이 이 데이터로
// 포탈 뷰에서 개구부 밖 지오메트리를 잘라낸다. 매 Tick 호출.
void APortalActor::UpdatePortalFrustumData()
{
    // 전달 통로(ViewExtension)나 기준 메시가 없으면 계산 무의미 → 스킵
    if (!ViewExtension.IsValid()) return;
    if (!IsValid(PortalMesh) || !GetWorld()) return;

    // 절두체의 꼭짓점(눈)이 될 플레이어 카메라 (스케줄러 캐시 경유)
    UCameraComponent* Camera = GetViewCamera();
    if (!Camera) return;

    // 렌더 스레드로 보낼 데이터 구조체 채우기 시작
    FPortalFrustumData FrustumData;
    FrustumData.EyePosition = Camera->GetComponentLocation();   // 절두체의 시작점(눈)

    // 포탈 평면의 4꼭짓점 계산: 중심 ± (오른쪽 벡터 × 반너비) ± (위 벡터 × 반높이)
    FVector PortalCenter = PortalMesh->GetComponentLocation();
    FVector Right = PortalMesh->GetRightVector();
    FVector Up = PortalMesh->GetUpVector();
    FVector Scale = PortalMesh->GetComponentScale();
    // 기본 Plane 메시는 100x100cm → 절반인 50에 스케일을 곱해 실제 반너비/반높이
    float HalfWidth = 50.0f * Scale.Y;
    float HalfHeight = 50.0f * Scale.Z;

    // 시계 방향으로 4개 코너: 우상 → 좌상 → 좌하 → 우하
    FrustumData.Corners[0] = PortalCenter + (Right * HalfWidth) + (Up * HalfHeight);
    FrustumData.Corners[1] = PortalCenter - (Right * HalfWidth) + (Up * HalfHeight);
    FrustumData.Corners[2] = PortalCenter - (Right * HalfWidth) - (Up * HalfHeight);
    FrustumData.Corners[3] = PortalCenter + (Right * HalfWidth) - (Up * HalfHeight);

    // 포탈 메시 전체 바운딩 박스 + 유효 플래그
    FrustumData.PortalBounds = PortalMesh->Bounds.GetBox();
    FrustumData.bIsValid = true;

    // 게임 스레드 → 렌더 스레드로 절두체 데이터 발행
    ViewExtension->UpdatePortalFrustum(FrustumData);
}

// [LinkedPortal 모드] 전통적 포탈 수학의 핵심 — "거울 반사" 카메라 배치.
// 플레이어가 포탈 A를 보면, A 기준 상대좌표를 구해 포탈 B 기준으로 펼쳐서
// SceneCapture를 B 뒤편 대응 위치에 놓는다. Tick에서 매 프레임 호출.
void APortalActor::UpdateSceneCapture()
{
    // 짝 포탈이 없으면 이 모드 자체가 성립 안 함
    if (!LinkedPortal) return;

    UCameraComponent* Camera = GetViewCamera();   // 스케줄러 캐시 경유
    if (!Camera) return;

    // 1) 플레이어 카메라의 월드 위치/회전 취득
    FVector  CameraLocation = Camera->GetComponentLocation();
    FRotator CameraRotation = Camera->GetComponentRotation();

    // 2) 월드 좌표 → "이 포탈 기준" 로컬 좌표로 변환
    //    InverseTransformPosition = 월드→로컬 변환 (TransformPosition의 역연산)
    FTransform ThisTransform = GetActorTransform();
    FVector LocalPos = ThisTransform.InverseTransformPosition(CameraLocation);
    FQuat   LocalRot = ThisTransform.InverseTransformRotation(CameraRotation.Quaternion());

    // 3) 포탈은 "뒤집힌 거울"이므로 앞뒤(X)를 반전하고 Z축 기준 180도(PI 라디안) 회전
    LocalPos.X = -LocalPos.X;
    LocalRot = FQuat(FVector::UpVector, PI) * LocalRot;

    // 4) 그 로컬 좌표를 "링크드 포탈 기준"으로 다시 월드 좌표로 펼침
    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector TargetPos = LinkedTransform.TransformPosition(LocalPos);
    FQuat   TargetRot = LinkedTransform.TransformRotation(LocalRot);

    // 5) 캡처 카메라를 계산된 위치/회전으로 이동 → 포탈 너머에서 보는 것 같은 그림이 찍힘
    SceneCapture->SetWorldLocationAndRotation(TargetPos, TargetRot.Rotator());

    SceneCapture->FOVAngle = 104.0f;

    // 클립 플레인: LinkedPortal 면 기준으로 너머만 캡처
    // (법선을 -Forward로 = 포탈 뒤쪽 공간만 남기고 앞쪽 지오메트리는 잘라냄)
    SceneCapture->ClipPlaneBase = LinkedPortal->GetActorLocation();
    SceneCapture->ClipPlaneNormal = -LinkedPortal->GetActorForwardVector();
}

// 카메라(머리)가 포탈 평면을 통과했는지 매 틱 검사.
// VR에서는 머리만 쑥 들어가는 경우 캡슐 Overlap이 안 잡혀서, 평면 부호 변화로 직접 감지한다.
// 호출자: Tick() (뷰어가 있을 때만).
void APortalActor::CheckPortalCrossing(UCameraComponent* Camera)
{
    if (!Camera) return;

    FVector CameraPos = Camera->GetComponentLocation();
    FVector PortalPos = GetActorLocation();
    FVector PortalNormal = GetActorForwardVector();   // 포탈 "앞" 방향 (평면의 법선)

    // 포탈까지 거리가 너무 멀면 체크 안 함
    // (멀리서 평면 뒤로 돌아가는 건 통과가 아님 — 부호도 0으로 리셋해 오판 방지)
    float DistToPortal = FVector::Dist(CameraPos, PortalPos);
    if (DistToPortal > 200.0f)
    {
        LastDotSign = 0;
        return;
    }

    // 포탈 평면 기준 카메라 부호 (양수=앞, 음수=뒤)
    // 내적(Dot) = 두 벡터 방향 일치도. (카메라 방향벡터)·(법선) > 0 이면 평면 앞쪽
    FVector ToCamera = CameraPos - PortalPos;
    float Dot = FVector::DotProduct(ToCamera, PortalNormal);
    int32 CurrentSign = (Dot >= 0.0f) ? 1 : -1;

    // 앞(+)에서 뒤(-)로 넘어간 순간 = 포탈 통과
    // (이전 프레임 부호와 비교 — "순간"을 잡기 위해 프레임 간 상태(LastDotSign)가 필요)
    if (LastDotSign == 1 && CurrentSign == -1)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Portal] Camera crossed portal plane → Teleporting"));
        ExecuteTeleport(UGameplayStatics::GetPlayerPawn(GetWorld(), 0));
    }

    // 이번 프레임 부호를 저장 → 다음 프레임 비교 기준
    LastDotSign = CurrentSign;
}

// 실제 순간이동 실행. CheckPortalCrossing(평면 통과)이 호출.
// 모드 1(TargetLevel): 레벨 안 캡처 위치로 점프 / 모드 2(LinkedPortal): 상대좌표 보존 점프.
void APortalActor::ExecuteTeleport(APawn* Pawn)
{
    if (!Pawn) return;   // 이동시킬 대상이 없으면 무의미

    // 모드 1: TargetLevel — 레벨이 로드 완료된 경우에만 (안 그러면 허공으로 떨어짐)
    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        // 도착 지점 = 캡처 카메라가 있던 위치 (보던 풍경 속으로 들어가는 연출)
        FVector TeleportLocation = TargetCaptureLocation;
        FRotator TeleportRotation = TargetViewTransform.GetRotation().Rotator();
        Pawn->SetActorLocationAndRotation(TeleportLocation, TeleportRotation);
        LastDotSign = 0;   // 통과 상태 리셋 — 도착 직후 재통과로 오판하는 것 방지

        UE_LOG(LogTemp, Warning, TEXT("[Portal] Teleported to TargetLevel at %s"),
            *TeleportLocation.ToString());
        return;
    }

    // 모드 2: LinkedPortal — 짝 포탈이 없으면 갈 곳이 없음
    if (!LinkedPortal) return;

    // UpdateSceneCapture와 같은 수학: 내 포탈 기준 상대 위치/회전을 구해서
    FTransform ThisTransform = GetActorTransform();
    FVector LocalPosition = ThisTransform.InverseTransformPosition(Pawn->GetActorLocation());
    FQuat   LocalRotation = ThisTransform.InverseTransformRotation(Pawn->GetActorRotation().Quaternion());

    // 링크드 포탈 기준으로 다시 펼침 → 들어간 각도/위치 그대로 반대편에서 나옴
    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector NewPosition = LinkedTransform.TransformPosition(LocalPosition);
    FQuat   NewRotation = LinkedTransform.TransformRotation(LocalRotation);

    // 포탈은 서로 마주보는 구조이므로 나올 때 180도 돌려서 등지고 나오게 함
    FRotator FinalRotation = NewRotation.Rotator();
    FinalRotation.Yaw += 180.0f;

    Pawn->SetActorLocationAndRotation(NewPosition, FinalRotation);
    LastDotSign = 0;   // 통과 상태 리셋

    UE_LOG(LogTemp, Warning, TEXT("[Portal] Teleported to LinkedPortal"));
}

// 디버그 시각화 — bDebugLumenRays 체크 시 Tick이 1회 호출.
// 캡처 카메라의 시야(빨강), 포탈 절두체(초록), 법선(빨강 화살표), 텔레포트 반경(주황)을 그림.
void APortalActor::DrawLumenDebug(UCameraComponent* Camera)
{
    UWorld* World = GetWorld();
    if (!World || !IsValid(SceneCapture)) return;   // 그릴 월드/카메라가 없으면 스킵

    // ───────────────────────────────────────────────
    // 1) SceneCapture 샘플 레이 그리드 (Red)
    //    Lumen이 타겟 레벨을 어떤 방향으로 샘플링하는지 시각화
    // ───────────────────────────────────────────────
    FVector CapturePos     = SceneCapture->GetComponentLocation();
    FVector CaptureForward = SceneCapture->GetForwardVector();
    FVector CaptureRight   = SceneCapture->GetRightVector();
    FVector CaptureUp      = SceneCapture->GetUpVector();

    const int32 GridSize  = 20;    // 20x20 = 400개 레이
    const float Spread    = 0.8f;  // FOV 104도에 맞춰 넓게
    const float RayLength = 2000.f;

    // 정면 방향을 중심으로 좌우/상하로 부채꼴처럼 퍼지는 격자 레이 생성
    for (int32 xi = 0; xi < GridSize; xi++)
    {
        for (int32 yi = 0; yi < GridSize; yi++)
        {
            // 격자 인덱스를 [-Spread/2, +Spread/2] 범위의 오프셋으로 변환 (중앙 = 0)
            float OffsetX = (xi - (GridSize - 1) * 0.5f) * Spread / (GridSize - 1);
            float OffsetY = (yi - (GridSize - 1) * 0.5f) * Spread / (GridSize - 1);

            // 정면 + 좌우/상하 성분 합성 후 정규화 = 레이 방향 벡터
            FVector Dir = (CaptureForward
                         + CaptureRight * OffsetX
                         + CaptureUp    * OffsetY).GetSafeNormal();

            DrawDebugLine(
                World,
                CapturePos,
                CapturePos + Dir * RayLength,
                FColor::Red,
                true,    // bPersistentLines = true → FlushPersistentDebugLines로 명시적 제거
                -1.f,
                0,
                0.5f
            );
        }
    }

    // SceneCapture 위치 표시 (노란 구)
    DrawDebugSphere(World, CapturePos, 8.f, 8, FColor::Yellow, true, -1.f, 0, 1.5f);

    // ───────────────────────────────────────────────
    // 2) 포탈 프러스텀 (Green) — 카메라 → 포탈 4꼭짓점
    // ───────────────────────────────────────────────
    FVector PortalCenter = PortalMesh->GetComponentLocation();
    FVector Right = PortalMesh->GetRightVector();
    FVector Up    = PortalMesh->GetUpVector();
    FVector Scale = PortalMesh->GetComponentScale();
    float HalfW = 50.f * Scale.Y;
    float HalfH = 50.f * Scale.Z;

    FVector Corners[4] = {
        PortalCenter + Right * HalfW + Up * HalfH,
        PortalCenter - Right * HalfW + Up * HalfH,
        PortalCenter - Right * HalfW - Up * HalfH,
        PortalCenter + Right * HalfW - Up * HalfH
    };

    // 포탈 테두리 사각형: (i+1)%4 로 마지막 꼭짓점과 첫 꼭짓점도 이어줌
    for (int32 i = 0; i < 4; i++)
        DrawDebugLine(World, Corners[i], Corners[(i + 1) % 4], FColor::Green, true, -1.f, 0, 2.f);

    // 카메라가 있으면 눈 → 각 꼭짓점 선도 그려 절두체 피라미드 모양 완성
    if (Camera)
    {
        FVector CameraPos = Camera->GetComponentLocation();
        for (int32 i = 0; i < 4; i++)
            DrawDebugLine(World, CameraPos, Corners[i], FColor(0, 200, 0), true, -1.f, 0, 0.5f);
    }

    // ───────────────────────────────────────────────
    // 3) 포탈 법선 벡터 (Red 화살표)
    // ───────────────────────────────────────────────
    FVector PortalNormal = GetActorForwardVector();
    DrawDebugDirectionalArrow(
        World,
        PortalCenter,
        PortalCenter + PortalNormal * 120.f,
        30.f,
        FColor::Red,
        true, -1.f, 0, 2.f
    );

    // ───────────────────────────────────────────────
    // 4) 텔레포트 감지 거리 (Orange 구, 200cm)
    // ───────────────────────────────────────────────
    DrawDebugSphere(World, GetActorLocation(), 200.f, 16, FColor::Orange, true, -1.f, 0, 0.5f);
}

// TriggerVolume에 무언가 겹쳤을 때 엔진이 호출하는 콜백 (BeginPlay에서 AddDynamic으로 바인딩).
// CheckPortalCrossing(평면 감지)과 별개로 동작하는 "몸 전체 진입" 경로의 텔레포트.
void APortalActor::OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult& SweepResult)
{
    if (!OtherActor) return;   // 겹친 대상이 없으면(이론상) 무시
    UE_LOG(LogTemp, Warning, TEXT("[Teleport] OnOverlapBegin: OtherActor=%s"), *OtherActor->GetName());

    // Pawn(플레이어/AI 캐릭터)만 텔레포트 대상 — 소품이 굴러 들어와도 무시
    APawn* Pawn = Cast<APawn>(OtherActor);
    if (!Pawn)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Teleport] FAIL: OtherActor is not a Pawn"));
        return;
    }

    // 포탈 앞면에서 진입했는지 확인 (뒤에서 진입 방지)
    // 내적 > 0 = 플레이어가 포탈 앞쪽 공간에 있음
    FVector ToPlayer = Pawn->GetActorLocation() - GetActorLocation();
    float Dot = FVector::DotProduct(ToPlayer, GetActorForwardVector());
    UE_LOG(LogTemp, Warning, TEXT("[Teleport] Dot product: %.2f (negative = back side, teleport blocked)"), Dot);
    if (Dot < 0.0f) return;   // 뒷면 진입은 차단

    UE_LOG(LogTemp, Warning, TEXT("[Teleport] TargetLevel.IsNull=%d, StreamingLevel=%d, LoadedLevel=%d"),
        (int32)TargetLevel.IsNull(),
        (int32)(StreamingLevel != nullptr),
        (int32)(StreamingLevel && StreamingLevel->GetLoadedLevel() != nullptr));

    // 모드 1: TargetLevel (스트리밍 레벨로 순간이동)
    // ※ 주의: 여기서는 도착지가 TargetViewTransform.GetLocation()(레벨 스폰 위치)인데,
    //   ExecuteTeleport()는 TargetCaptureLocation(캡처 카메라 위치)을 씀 — 두 경로의 도착지가 다름.
    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        FVector TeleportLocation = TargetViewTransform.GetLocation();
        FRotator TeleportRotation = TargetViewTransform.GetRotation().Rotator();

        Pawn->SetActorLocationAndRotation(TeleportLocation, TeleportRotation);

        UE_LOG(LogTemp, Log, TEXT("PortalActor: Teleported to TargetLevel at %s"),
            *TeleportLocation.ToString());
        return;
    }

    // 모드 2: LinkedPortal (같은 레벨 내 포탈 간 이동)
    // 이하 수학은 ExecuteTeleport()의 모드 2와 동일 (상대좌표 보존 점프)
    if (!LinkedPortal) return;

    FTransform ThisTransform = GetActorTransform();
    FVector LocalPosition = ThisTransform.InverseTransformPosition(Pawn->GetActorLocation());
    FQuat   LocalRotation = ThisTransform.InverseTransformRotation(Pawn->GetActorRotation().Quaternion());

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector NewPosition = LinkedTransform.TransformPosition(LocalPosition);
    FQuat   NewRotation = LinkedTransform.TransformRotation(LocalRotation);

    FRotator FinalRotation = NewRotation.Rotator();
    FinalRotation.Yaw += 180.0f;

    Pawn->SetActorLocationAndRotation(NewPosition, FinalRotation);

    UE_LOG(LogTemp, Log, TEXT("PortalActor: Teleported to LinkedPortal"));
}

// ───────────────────────────────────────────────────────────────
// Phase 1 — SceneCapture2D 공식 최적화 헬퍼
// ───────────────────────────────────────────────────────────────

// Phase 1의 절반: 캡처 뷰의 비싼 렌더 기능을 끄고 LOD를 낮추는 함수.
// BeginPlay에서 1회 + Tick에서 r.Portal.Phase1 토글 변화 감지 시 호출.
// bEnable=true → 최적화 적용, false → vanilla 상태로 원상복귀 (A/B 비교 가능).
void APortalActor::ApplyPhase1ShowFlags(bool bEnable)
{
    if (!SceneCapture) return;   // 캡처 컴포넌트 없으면 설정할 대상이 없음

    // ── 비싼 ShowFlags OFF (포탈 뷰에서는 시각적 차이 거의 없음) ──
    // 안개·모션블러·블룸·SSR·TAA·렌즈플레어·필름그레인·비네트
    // bEnable=true → 위 효과 OFF (false 인자), bEnable=false → 원상복귀 (true 인자)
    SceneCapture->ShowFlags.SetVolumetricFog(!bEnable);
    SceneCapture->ShowFlags.SetMotionBlur(!bEnable);
    SceneCapture->ShowFlags.SetBloom(!bEnable);
    SceneCapture->ShowFlags.SetScreenSpaceReflections(!bEnable);  // Lumen Reflections와 중복
    SceneCapture->ShowFlags.SetTemporalAA(!bEnable);
    SceneCapture->ShowFlags.SetLensFlares(!bEnable);
    SceneCapture->ShowFlags.SetGrain(!bEnable);
    SceneCapture->ShowFlags.SetVignette(!bEnable);

    // ── LOD 및 시야거리 강제 ──
    // 포탈 RT는 메인 뷰보다 픽셀당 디테일 차이 잘 안 보이므로 강제 LOD 다운
    if (bEnable)
    {
        SceneCapture->LODDistanceFactor = 2.5f;        // 값이 클수록 더 낮은 LOD 메시 사용
        SceneCapture->MaxViewDistanceOverride = 8000.0f; // 80m 넘는 액터는 컷
    }
    else
    {
        SceneCapture->LODDistanceFactor = 1.0f;
        SceneCapture->MaxViewDistanceOverride = -1.0f; // 비활성
    }

    // ── PostProcess 강제 오버라이드 (포탈 뷰 전용 경량 PP) ──
    // bOverride_X = bEnable: 최적화 ON일 때만 오버라이드 활성, OFF면 볼륨 기본값으로 복귀
    SceneCapture->PostProcessSettings.bOverride_MotionBlurAmount = bEnable;
    SceneCapture->PostProcessSettings.bOverride_BloomIntensity = bEnable;
    SceneCapture->PostProcessSettings.bOverride_LensFlareIntensity = bEnable;
    SceneCapture->PostProcessSettings.bOverride_VignetteIntensity = bEnable;
    SceneCapture->PostProcessSettings.bOverride_SceneFringeIntensity = bEnable;
    if (bEnable)
    {
        // 강도를 전부 0으로 → 해당 PP 패스가 사실상 비용 없이 통과
        SceneCapture->PostProcessSettings.MotionBlurAmount = 0.f;
        SceneCapture->PostProcessSettings.BloomIntensity = 0.f;
        SceneCapture->PostProcessSettings.LensFlareIntensity = 0.f;
        SceneCapture->PostProcessSettings.VignetteIntensity = 0.f;
        SceneCapture->PostProcessSettings.SceneFringeIntensity = 0.f;
    }

    UE_LOG(LogTemp, Warning,
        TEXT("[Portal-Phase1] ShowFlags=%s, LOD=%g, MaxView=%g"),
        bEnable ? TEXT("OPT") : TEXT("VANILLA"),
        SceneCapture->LODDistanceFactor,
        SceneCapture->MaxViewDistanceOverride);
}

// Phase 1의 나머지 절반: 캡처가 "스트리밍 레벨 액터만" 그리도록 화이트리스트 설정.
// 액터 목록이 채워진 뒤에만 의미가 있어서 OnTargetLevelLoaded()에서 호출됨
// (+ Tick의 Phase1 토글 변화 시에도).
void APortalActor::ApplyPhase1ShowOnlyActors(bool bEnable)
{
    if (!SceneCapture) return;

    // 켜라고 했어도 목록이 비어 있으면(레벨 미로드) 화이트리스트를 걸 수 없음 → else로 빠져 vanilla 유지
    if (bEnable && StreamingLevelActors.Num() > 0)
    {
        // ── ShowOnlyList 모드: StreamingLevelActors만 렌더 후보로 ──
        // 메인 씬(Showcase) 액터 전부 컷 → Basepass / Shadow / Lumen 모두 감소
        SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        SceneCapture->ShowOnlyActors.Reset();
        // Reserve = 미리 메모리 확보 (Add 반복 시 재할당 방지하는 관용적 최적화)
        SceneCapture->ShowOnlyActors.Reserve(StreamingLevelActors.Num());
        for (AActor* Actor : StreamingLevelActors)
        {
            if (IsValid(Actor)) SceneCapture->ShowOnlyActors.Add(Actor);
        }
        UE_LOG(LogTemp, Warning,
            TEXT("[Portal-Phase1] ShowOnlyActors ON: %d actors (Alley only)"),
            SceneCapture->ShowOnlyActors.Num());
    }
    else
    {
        // 원상복귀: 모든 액터 렌더 후보
        SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_LegacySceneCapture;
        SceneCapture->ShowOnlyActors.Reset();
        UE_LOG(LogTemp, Warning, TEXT("[Portal-Phase1] ShowOnlyActors OFF (vanilla)"));
    }
}

// ───────────────────────────────────────────────────────────────
// Phase 3 — RT Memory Pool 헬퍼
// ───────────────────────────────────────────────────────────────

// 외부(PortalScheduler)에 공개하는 우선순위 — 내부 계산 함수로 그대로 위임.
float APortalActor::GetCapturePriority() const
{
    return ComputePortalPriority();
}

// 우선순위 점수 계산의 본체. Phase2 스케줄러(예산 배분)와 Phase3 풀(Hot 슬롯 배정)이 사용.
// 점수가 클수록 "지금 갱신이 급한" 포탈로 취급된다.
float APortalActor::ComputePortalPriority() const
{
    // 우선순위 점수 = 거리 × 화면면적 × 시선 (가까울수록·클수록·정면일수록 높음)
    // 카메라는 스케줄러 캐시 경유 → 매 프레임 N번 호출돼도 실제 조회는 1회.
    const UCameraComponent* Cam = GetViewCamera();
    if (!Cam) return 0.f;   // 기준이 될 카메라가 없으면 최저 점수

    const FVector ViewerLoc = Cam->GetComponentLocation();  // 헤드 위치 = 뷰어 기준점
    const FVector PortalLoc = GetActorLocation();
    const float Dist = FVector::Dist(ViewerLoc, PortalLoc);

    // 거리 기반 (가까울수록 높음): 1000cm 기준 정규화
    const float DistScore = 1000.0f / FMath::Max(Dist, 1.0f);

    // 화면 크기 기반 (포탈 메시 스케일): Y * Z 면적
    const FVector Scale = PortalMesh ? PortalMesh->GetComponentScale() : FVector::OneVector;
    const float AreaScore = Scale.Y * Scale.Z;

    // 시선 기반: 카메라가 포탈을 향할수록 높고, 등 뒤(시야 밖)면 거의 0.
    //   Facing = 1(정면) / 0(옆) / -1(등 뒤)  →  [0.05, 1.2] 로 매핑
    const FVector ToPortal = (PortalLoc - ViewerLoc).GetSafeNormal();
    const float Facing = static_cast<float>(FVector::DotProduct(Cam->GetForwardVector(), ToPortal));
    const float t = FMath::Clamp((Facing + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float GazeScore = 0.05f + t * (1.2f - 0.05f);

    // 세 점수의 곱이 최종 우선순위
    return DistScore * AreaScore * GazeScore;
}

UCameraComponent* APortalActor::GetViewCamera() const
{
    if (UWorld* World = GetWorld())
    {
        if (UPortalScheduler* Sched = World->GetSubsystem<UPortalScheduler>())
        {
            return Sched->GetActiveCamera();  // 프레임당 1회만 실제 조회되는 캐시
        }
    }
    return nullptr;
}

// Hot→Cold로 강등되기 직전, 자기 ColdRT에 마지막 모습을 1회 백업하는 함수.
// Tick의 Phase3 분기에서 "직전엔 Hot이었는데 이번엔 슬롯을 못 받은" 프레임에만 호출.
void APortalActor::DoFinalColdCapture()
{
    if (!SceneCapture || !ColdRenderTarget) return;   // 캡처할 카메라/저장할 RT가 없으면 불가

    // 마지막 1회 ColdRT 캡처 — Hot에서 Cold로 전환되기 전 fallback 갱신
    // 타겟을 잠깐 ColdRT로 바꿔 수동 캡처(CaptureScene = 즉시 1회 캡처) 후 원복
    UTextureRenderTarget2D* PrevTarget = SceneCapture->TextureTarget;
    SceneCapture->TextureTarget = ColdRenderTarget;
    SceneCapture->CaptureScene();
    SceneCapture->TextureTarget = PrevTarget;

    UE_LOG(LogTemp, Verbose, TEXT("[Portal-Phase3] Final ColdRT capture for fallback"));
}

// ───────────────────────────────────────────────────────────────
// 디테일 창 기능 토글 (체크박스) 구현
//   디테일 패널의 bool 체크박스를 켜고/끄면 대응 r.Portal.* CVar가 적용된다.
// ───────────────────────────────────────────────────────────────
void APortalActor::ApplyCVar(const TCHAR* CVarName, bool bOn, const FString& DisplayLabel)
{
    // 이름 문자열로 콘솔 변수 객체를 검색 (다른 cpp에 정의된 CVar도 이름만 알면 접근 가능)
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(CVarName);
    if (!CVar)
    {
        // 오타이거나 해당 CVar를 정의한 모듈이 아직 안 로드된 경우
        UE_LOG(LogTemp, Warning, TEXT("[Portal-Toggle] CVar '%s' 를 찾을 수 없습니다."), CVarName);
        return;
    }

    // ECVF_SetByConsole = "콘솔에서 직접 입력"과 같은 우선순위로 값 설정 (기본값을 확실히 덮음)
    CVar->Set(bOn ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);

    const FString StateText = bOn ? TEXT("ON") : TEXT("OFF");
    UE_LOG(LogTemp, Display, TEXT("[Portal-Toggle] %s → %s (%s)"),
        *DisplayLabel, *StateText, CVarName);

    if (GEngine)
    {
        // CVarName 해시로 고정 키를 만들어, 같은 항목은 화면에서 갱신되도록 함
        // (AddOnScreenDebugMessage는 같은 Key로 다시 부르면 기존 줄을 교체 — 줄 도배 방지)
        const uint64 Key = (uint64)GetTypeHash(FString(CVarName));
        const FColor Color = bOn ? FColor::Green : FColor::Red;   // ON=초록, OFF=빨강
        GEngine->AddOnScreenDebugMessage(
            Key, 4.0f, Color,   // 4초간 화면 좌상단에 표시
            FString::Printf(TEXT("%s : %s"), *DisplayLabel, *StateText));
    }
}

// 디테일 패널 체크박스 5개를 대응 CVar에 일괄 반영.
// 호출 시점: BeginPlay(플레이 시작) + 블루프린트에서 수동 호출 가능(BlueprintCallable).
void APortalActor::ApplyFeatureToggles()
{
    ApplyCVar(TEXT("r.Portal.Enable"),        bEnablePortal,         TEXT("포탈 전체"));
    ApplyCVar(TEXT("r.Portal.Phase1"),        bEnablePhase1,         TEXT("Phase 1 (SceneCapture 최적화)"));
    ApplyCVar(TEXT("r.Portal.Phase2"),        bEnablePhase2,         TEXT("Phase 2 (Frame Budget)"));
    ApplyCVar(TEXT("r.Portal.Phase3"),        bEnablePhase3,         TEXT("Phase 3 (RT Memory Pool)"));
    ApplyCVar(TEXT("r.Portal.FrustumCulling"), bEnableFrustumCulling, TEXT("Frustum Culling"));
}

// 에디터 전용: 디테일 패널에서 체크박스를 바꾸는 "즉시" CVar에 반영하는 훅.
// (Play 중이 아니어도 동작 — 에디터가 프로퍼티 변경 때마다 자동 호출)
#if WITH_EDITOR
void APortalActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // 어떤 프로퍼티가 바뀌었는지 이름으로 식별
    // GET_MEMBER_NAME_CHECKED = 멤버 이름을 FName으로 얻되, 오타면 컴파일 에러 (문자열 하드코딩보다 안전)
    const FName Name = PropertyChangedEvent.GetPropertyName();

    if (Name == GET_MEMBER_NAME_CHECKED(APortalActor, bEnablePortal))
        ApplyCVar(TEXT("r.Portal.Enable"), bEnablePortal, TEXT("포탈 전체"));
    else if (Name == GET_MEMBER_NAME_CHECKED(APortalActor, bEnablePhase1))
        ApplyCVar(TEXT("r.Portal.Phase1"), bEnablePhase1, TEXT("Phase 1 (SceneCapture 최적화)"));
    else if (Name == GET_MEMBER_NAME_CHECKED(APortalActor, bEnablePhase2))
        ApplyCVar(TEXT("r.Portal.Phase2"), bEnablePhase2, TEXT("Phase 2 (Frame Budget)"));
    else if (Name == GET_MEMBER_NAME_CHECKED(APortalActor, bEnablePhase3))
        ApplyCVar(TEXT("r.Portal.Phase3"), bEnablePhase3, TEXT("Phase 3 (RT Memory Pool)"));
    else if (Name == GET_MEMBER_NAME_CHECKED(APortalActor, bEnableFrustumCulling))
        ApplyCVar(TEXT("r.Portal.FrustumCulling"), bEnableFrustumCulling, TEXT("Frustum Culling"));
}
#endif
