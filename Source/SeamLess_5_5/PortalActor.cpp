#include "PortalActor.h"
#include "PortalLevelManager.h"
#include "PortalScheduler.h"
#include "PortalRTPool.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
// IXRTrackingSystem.h: 사용처 없음(주석에만 'HMD' 단어 1번). HeadMountedDisplay 모듈
// 의존성을 안 더해도 되도록 include만 제거. 추후 실제 XR API 필요 시 Build.cs에
// "HeadMountedDisplay" 모듈을 추가하고 이 헤더를 되살릴 것.
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/LightComponent.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"

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

APortalActor::APortalActor()
{
    PrimaryActorTick.bCanEverTick = true;

    PortalRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PortalRoot"));
    RootComponent = PortalRoot;

    PortalMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PortalMesh"));
    PortalMesh->SetupAttachment(PortalRoot);
    PortalMesh->SetMobility(EComponentMobility::Movable);
    PortalMesh->SetRelativeRotation(FRotator(0.0f, 0.0f, 90.0f));
    PortalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
    SceneCapture->SetupAttachment(PortalRoot);
    // Lumen GI 누적을 위해 bCaptureEveryFrame은 Tick에서 동적으로 관리
    // (근거리: true → Lumen Surface Cache 축적, 원거리: false → 성능)
    SceneCapture->bCaptureEveryFrame = false;
    SceneCapture->bCaptureOnMovement = false;
    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
    SceneCapture->bAlwaysPersistRenderingState = true;

    TriggerVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerVolume"));
    TriggerVolume->SetupAttachment(PortalRoot);
    TriggerVolume->SetBoxExtent(FVector(50.0f, 200.0f, 200.0f));
    TriggerVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    TriggerVolume->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
    TriggerVolume->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
    TriggerVolume->SetGenerateOverlapEvents(true);

    TargetCaptureLocation = FVector::ZeroVector;
}

void APortalActor::BeginPlay()
{
    Super::BeginPlay();

    // 디테일 창 체크박스 상태를 CVar에 반영 (Play 시작 시 1회)
    ApplyFeatureToggles();

    ViewExtension = FSceneViewExtensions::NewExtension<FPortalViewExtension>();

    // 포탈 메시: Custom Depth + Stencil 활성화
    PortalMesh->SetRenderCustomDepth(true);
    PortalMesh->SetCustomDepthStencilValue(PortalStencilValue);

    // 렌더 타겟 생성 (Phase 3 OFF일 때 또는 fallback용)
    if (!RenderTarget)
    {
        RenderTarget = NewObject<UTextureRenderTarget2D>(this);
        RenderTarget->InitAutoFormat(1920, 1080);
        RenderTarget->bAutoGenerateMips = false;
        RenderTarget->RenderTargetFormat = RTF_RGBA16f;
    }

    // Phase 3: ColdRT 생성 — 작은 해상도, 포탈별 영구 보유
    if (!ColdRenderTarget)
    {
        ColdRenderTarget = NewObject<UTextureRenderTarget2D>(this);
        ColdRenderTarget->InitAutoFormat(ColdRTWidth, ColdRTHeight);
        ColdRenderTarget->bAutoGenerateMips = false;
        ColdRenderTarget->RenderTargetFormat = RTF_RGBA16f;
        UE_LOG(LogTemp, Warning, TEXT("[Portal-Phase3] ColdRT created %dx%d (%.2f KB)"),
            ColdRTWidth, ColdRTHeight,
            (ColdRTWidth * ColdRTHeight * 8.0f) / 1024.0f);
    }

    SceneCapture->TextureTarget = RenderTarget;
    SceneCapture->FOVAngle = 104.0f;  // Quest 3 HMD FOV
    SceneCapture->ShowFlags.SetAtmosphere(true);
    SceneCapture->ShowFlags.SetSkyLighting(true);
    SceneCapture->ShowFlags.SetFog(true);
    SceneCapture->ShowFlags.SetVolumetricFog(true);
    SceneCapture->ShowFlags.SetDynamicShadows(true);
    SceneCapture->ShowFlags.SetGlobalIllumination(true);
    SceneCapture->ShowFlags.SetLumenGlobalIllumination(true);  // ← 핵심: 이게 없으면 ShouldRenderLumenDiffuseGI가 false 반환
    SceneCapture->ShowFlags.SetLighting(true);
    SceneCapture->ShowFlags.SetPostProcessing(true);
    SceneCapture->ShowFlags.SetAmbientOcclusion(true);
    SceneCapture->ShowFlags.SetLumenReflections(true);
    SceneCapture->ShowFlags.SetIndirectLightingCache(true);
    SceneCapture->ShowFlags.SetSkyLighting(true);

    // Lumen GI 방식 명시적 설정 — FinalPostProcessSettings.DynamicGlobalIlluminationMethod == Lumen 조건 충족
    SceneCapture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
    SceneCapture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;

    // 노출 설정 — SceneCapture는 기본적으로 auto exposure가 꺼져 있어서 어둡게 나옴
    // Min/Max를 같은 값으로 잠그면 씬 밝기와 어긋남 → 적절한 범위로 열어줌
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMethod = true;
    SceneCapture->PostProcessSettings.AutoExposureMethod = AEM_Histogram;
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
    SceneCapture->PostProcessSettings.AutoExposureMinBrightness = 0.18f;  // 실내 씬 기준
    SceneCapture->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
    SceneCapture->PostProcessSettings.AutoExposureMaxBrightness = 8.0f;
    // SpeedUp/Down: SceneCapture는 매 프레임 재설정되므로 빠른 적응이 필요
    SceneCapture->PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
    SceneCapture->PostProcessSettings.AutoExposureSpeedUp = 10.0f;
    SceneCapture->PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
    SceneCapture->PostProcessSettings.AutoExposureSpeedDown = 10.0f;

    // 클립 플레인: Tick에서 모드에 따라 동적으로 설정
    SceneCapture->bEnableClipPlane = false;

    // 포탈 메시 머티리얼 설정
    if (PortalMaterial)
    {
        DynamicMaterial = UMaterialInstanceDynamic::Create(PortalMaterial, this);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), RenderTarget);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), RenderTarget);
        PortalMesh->SetMaterial(0, DynamicMaterial);
    }

    // 스텐실 PP는 일단 비활성화 - 메시 머티리얼로 포탈 표시 확인 후 활성화
    // BindStencilMaterialToVolume();

    TriggerVolume->OnComponentBeginOverlap.AddDynamic(this, &APortalActor::OnOverlapBegin);

    // Phase 1: ShowFlags / LOD / PostProcess 적용 (ShowOnlyActors는 레벨 로드 후 OnTargetLevelLoaded에서)
    const bool bPhase1 = (CVarPortalPhase1.GetValueOnGameThread() != 0);
    ApplyPhase1ShowFlags(bPhase1);
    LastPhase1State = bPhase1 ? 1 : 0;

    // Phase 2: 스케줄러에 자신을 등록
    if (UPortalScheduler* Sched = GetWorld()->GetSubsystem<UPortalScheduler>())
    {
        Sched->RegisterPortal(this);
    }

    // 레벨은 BeginPlay에서 전부 로드하지 않는다.
    // 거리 기반으로 Tick의 UpdateLevelResidency()가 가까운 포탈만 동적 로드.
    // (포탈마다 다른 레벨일 때 N개를 동시에 띄우면 프레임이 무너지므로)
}

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
    StreamingLevel = nullptr;
    StreamingLevelActors.Reset();

    Super::EndPlay(EndPlayReason);
}

void APortalActor::BindStencilMaterialToVolume()
{
    if (!StencilPostProcessMaterial || !RenderTarget)
    {
        UE_LOG(LogTemp, Warning, TEXT("PortalActor: StencilPostProcessMaterial or RenderTarget not set"));
        return;
    }

    // PP 머티리얼 다이내믹 인스턴스 생성
    StencilPPInstance = UMaterialInstanceDynamic::Create(StencilPostProcessMaterial, this);
    StencilPPInstance->SetTextureParameterValue(FName("PortalRT"), RenderTarget);

    // 레벨에 있는 PostProcessVolume 찾기
    for (TActorIterator<APostProcessVolume> It(GetWorld()); It; ++It)
    {
        APostProcessVolume* PPVolume = *It;
        if (PPVolume && PPVolume->bUnbound)
        {
            PPVolume->Settings.WeightedBlendables.Array.Add(
                FWeightedBlendable(1.0f, StencilPPInstance));

            UE_LOG(LogTemp, Log, TEXT("PortalActor: Stencil PP bound to existing PostProcessVolume"));
            return;
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("PortalActor: No unbound PostProcessVolume found! Place one in the level with Infinite Extent enabled."));
}

void APortalActor::LoadTargetLevel()
{
    // PortalLevelManager 서브시스템 통해 공유 로드.
    // 같은 TargetLevel을 가진 다른 PortalActor가 이미 로드했다면
    // 그 인스턴스를 재사용 (메모리/렌더 비용 N배 절감).
    UPortalLevelManager* Mgr = GetWorld()->GetSubsystem<UPortalLevelManager>();
    if (!Mgr)
    {
        UE_LOG(LogTemp, Error, TEXT("[Portal] PortalLevelManager subsystem not found"));
        return;
    }

    // TargetViewTransform.Rotation + TargetLevelRotation을 합성해
    // 스트리밍 레벨 전체를 회전시킴.
    // 예) TargetLevelRotation.Yaw = 90 → Downtown_Alley가 Z축 기준 90도 돌아서 스폰됨
    // 주의: 같은 레벨을 공유하므로 두 번째 이후 포탈의 위치/회전은 무시되고
    //       첫 번째 포탈이 정한 값이 적용됨.
    const FQuat ComposedQ =
        TargetLevelRotation.Quaternion() * TargetViewTransform.GetRotation();
    const FRotator SpawnRot = ComposedQ.Rotator();

    // ── Phase 0 (refcount): AcquireLevel — 마지막 포탈이 사라질 때 자동 unload ──
    StreamingLevel = Mgr->AcquireLevel(
        TargetLevel,
        TargetViewTransform.GetLocation(),
        SpawnRot);

    if (!StreamingLevel)
    {
        UE_LOG(LogTemp, Warning, TEXT("PortalActor: TargetLevel load failed"));
        return;
    }

    bLevelAcquired = true;  // 참조 보유 표시 (중복 Acquire/Release 방지)

    // 다른 포탈이 이미 로드를 끝낸 상태면 콜백을 즉시 한 번 실행,
    // 아직 로드 중이면 델리게이트 바인딩해서 완료될 때 호출되도록.
    if (StreamingLevel->GetLoadedLevel())
    {
        UE_LOG(LogTemp, Warning, TEXT("[Portal] TargetLevel already loaded — invoking callback immediately"));
        OnTargetLevelLoaded();
    }
    else
    {
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
    1500.0f,
    TEXT("이 거리(cm) 이내로 들어오면 포탈 레벨을 로드 (기본 1500=15m)"),
    ECVF_Default
);
static TAutoConsoleVariable<float> CVarPortalLevelUnloadDist(
    TEXT("r.Portal.LevelUnloadDist"),
    2500.0f,
    TEXT("이 거리(cm) 밖으로 나가면 포탈 레벨을 언로드 (기본 2500=25m). LoadDist보다 커야 함"),
    ECVF_Default
);

void APortalActor::UpdateLevelResidency()
{
    // 링크드 포탈 모드(TargetLevel 없음)는 레벨 스트리밍 안 함
    if (TargetLevel.IsNull()) return;

    UWorld* World = GetWorld();
    if (!World) return;

    APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0);
    if (!Pawn) return;

    const float Dist = FVector::Dist(Pawn->GetActorLocation(), GetActorLocation());
    const float LoadDist = CVarPortalLevelLoadDist.GetValueOnGameThread();
    float UnloadDist = CVarPortalLevelUnloadDist.GetValueOnGameThread();
    // 안전: 언로드 거리는 항상 로드 거리보다 크게(히스테리시스 보장)
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

void APortalActor::ReleaseTargetLevel()
{
    if (!bLevelAcquired) return;

    if (UWorld* World = GetWorld())
    {
        if (UPortalLevelManager* Mgr = World->GetSubsystem<UPortalLevelManager>())
        {
            if (!TargetLevel.IsNull()) Mgr->ReleaseLevel(TargetLevel);
        }
    }

    // 콜백 바인딩 해제 + 캐시 정리 (StreamingLevelActors는 더 이상 유효하지 않음)
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

void APortalActor::SetLevelActive(bool bActive)
{
    if (bActive == bLevelActive) return;   // 상태 변화 시에만 토글 (thrash/깜빡임 방지)
    bLevelActive = bActive;

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
        if (!IsValid(Actor)) continue;
        Actor->SetActorTickEnabled(bActive);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[Portal] SetLevelActive(%d): %s"), bActive ? 1 : 0, *GetName());
}

void APortalActor::OnTargetLevelLoaded()
{
    ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();
    if (!LoadedLevel) return;

    // SceneCapture가 스트리밍 레벨을 볼 수 있도록 명시적으로 Visible 설정
    StreamingLevel->SetShouldBeVisible(true);

    StreamingLevelActors.Reset();
    for (AActor* Actor : LoadedLevel->Actors)
    {
        if (!IsValid(Actor)) continue;
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

void APortalActor::EnsureSingleDirectionalLight()
{
    // 기본 OFF. 서로 다른 레벨을 쓰는 경우 켜면 레벨 조명이 사라지므로 opt-in.
    if (CVarPortalSingleDirLight.GetValueOnGameThread() == 0) return;

    UWorld* World = GetWorld();
    if (!World) return;

    TArray<AActor*> Lights;
    UGameplayStatics::GetAllActorsOfClass(World, ADirectionalLight::StaticClass(), Lights);
    if (Lights.Num() <= 1) return;  // 이미 1개 이하면 할 일 없음

    ULevel* StreamedLevel = StreamingLevel ? StreamingLevel->GetLoadedLevel() : nullptr;

    // 남길 라이트(Keep) 선정: 스트리밍 레벨이 아닌(=메인) 라이트를 우선.
    ADirectionalLight* Keep = nullptr;
    for (AActor* A : Lights)
    {
        ADirectionalLight* DL = Cast<ADirectionalLight>(A);
        if (!IsValid(DL)) continue;
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
    int32 Disabled = 0;
    for (AActor* A : Lights)
    {
        ADirectionalLight* DL = Cast<ADirectionalLight>(A);
        if (!IsValid(DL) || DL == Keep) continue;
        if (ULightComponent* LC = DL->GetLightComponent())
        {
            LC->SetVisibility(false);
            Disabled++;
        }
    }

    UE_LOG(LogTemp, Warning,
        TEXT("[Portal] EnsureSingleDirectionalLight: 1개 유지, %d개 끔 (total=%d)"),
        Disabled, Lights.Num());
}

void APortalActor::UpdateStreamingLevelBounds()
{
    if (!ViewExtension.IsValid()) return;

    TArray<FBoxSphereBounds> StreamingBounds;
    for (AActor* Actor : StreamingLevelActors)
    {
        if (!IsValid(Actor) || !Actor->GetRootComponent()) continue;
        FBox Box = Actor->GetComponentsBoundingBox(true);
        if (Box.IsValid && Box.GetExtent().SizeSquared() > 0.0f)
            StreamingBounds.Add(FBoxSphereBounds(Box));
    }

    ViewExtension->UpdateSceneActorBounds(StreamingBounds);

    UE_LOG(LogTemp, Log, TEXT("PortalActor: StreamingLevel bounds updated: %d actors"),
        StreamingBounds.Num());
}

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
    static bool bLastPortalEnabled = true;
    if (bPortalEnabled != bLastPortalEnabled)
    {
        if (PortalMesh) PortalMesh->SetVisibility(bPortalEnabled, true);
        if (SceneCapture)
        {
            SceneCapture->SetVisibility(bPortalEnabled, true);
            if (!bPortalEnabled) SceneCapture->bCaptureEveryFrame = false;
        }
        bLastPortalEnabled = bPortalEnabled;

        UE_LOG(LogTemp, Warning, TEXT("[Portal] r.Portal.Enable = %d"), bPortalEnabled ? 1 : 0);
    }

    if (!bPortalEnabled)
    {
        // 포탈 OFF 상태 — 디버그 라인만 정리하고 나머지 로직 전부 스킵
        if (bDebugLinesDrawn)
        {
            FlushPersistentDebugLines(GetWorld());
            bDebugLinesDrawn = false;
        }
        return;
    }

    // ── 거리 기반 동적 레벨 로드/언로드 ──────────────────────
    // 가까운 포탈의 레벨만 상주시켜, 포탈 N개여도 동시 로드 레벨 수를 제한.
    UpdateLevelResidency();

    // ── 활성 레벨 게이팅 ─────────────────────────────────────
    // 모여 있는 다른-레벨 다중 포탈: 우선순위 상위 N개 레벨만 렌더+Tick,
    // 나머지는 숨기고 Tick 꺼서 CPU·GPU 동시 절감. (포탈은 마지막 캡처 유지)
    if (UPortalScheduler* Sched = GetWorld()->GetSubsystem<UPortalScheduler>())
    {
        SetLevelActive(Sched->ShouldLevelBeActive(this));
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
    if (bDebugLumenRays && !bDebugLinesDrawn)
    {
        APawn* DebugPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
        UCameraComponent* DebugCamera = DebugPawn
            ? DebugPawn->FindComponentByClass<UCameraComponent>()
            : nullptr;
        DrawLumenDebug(DebugCamera);
        bDebugLinesDrawn = true;
    }
    else if (!bDebugLumenRays && bDebugLinesDrawn)
    {
        FlushPersistentDebugLines(GetWorld());
        bDebugLinesDrawn = false;
    }

    if (!ViewExtension.IsValid()) return;

    // StreamingLevel 바운드 수집
    if (StreamingLevel && StreamingLevel->GetLoadedLevel() && ViewExtension.IsValid())
    {
        ULevel* LoadedLevel = StreamingLevel->GetLoadedLevel();

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
                ViewExtension->UpdateSceneActorBounds(StreamingBounds);
            }
        }
    }

    UpdatePortalFrustumData();

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    UCameraComponent* Camera = PlayerPawn
        ? PlayerPawn->FindComponentByClass<UCameraComponent>()
        : nullptr;

    if (!PlayerPawn || !Camera) return;

    // VR에서는 Overlap이 안 잡히는 경우가 많아서 Tick에서 직접 감지
    CheckPortalCrossing(Camera);

    float DistToPortal = FVector::Dist(
        PlayerPawn->GetActorLocation(),
        GetActorLocation());

    // Lumen GI는 bCaptureEveryFrame=true 상태에서 연속 프레임을 통해 누적됨
    // 수동 CaptureScene()은 Lumen 입장에서 매번 새 뷰 → GI 누적 안 됨
    // 1000cm 이내: 매 프레임 캡처 (Lumen 누적), 이외: 캡처 정지 (성능)
    const float CaptureRange = 1000.0f;
    bool bShouldCapture = (DistToPortal < CaptureRange);

    // 레벨 스트리밍 모드인데 아직 로드 안 끝났으면 캡처 금지.
    // (안 그러면 SceneCapture가 제자리에서 메인 씬을 찍어 포탈에 숲이 잠깐 뜸)
    // 이 동안 포탈은 RenderTarget의 마지막 캡처를 그대로 유지.
    if (!TargetLevel.IsNull() && (!StreamingLevel || !StreamingLevel->GetLoadedLevel()))
    {
        bShouldCapture = false;
    }

    // 비활성(숨김) 레벨은 캡처해도 내용이 없으므로 캡처 금지 → 마지막 캡처 유지.
    if (!bLevelActive)
    {
        bShouldCapture = false;
    }

    // ── Phase 2: Frame Budget Allocator ───────────────────────────
    // 스케줄러가 "내 차례 아님"이라고 하면 캡처 스킵 → 이전 RT 그대로 사용
    if (bShouldCapture && CVarPortalPhase2.GetValueOnGameThread() != 0)
    {
        if (UPortalScheduler* Sched = GetWorld()->GetSubsystem<UPortalScheduler>())
        {
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
            UTextureRenderTarget2D* HotRT = bShouldCapture
                ? RTPool->RequestHotRT(this, ComputePortalPriority())
                : nullptr;

            if (HotRT)
            {
                // Hot 슬롯 받음 → 풀 RT에 캡처하고 그 RT를 표시
                TargetRT = HotRT;
                bIsHot = true;
                LastHotRT = HotRT;
            }
            else
            {
                // Cold → 캡처 안 함, 자기 ColdRT(자기 내용) 표시 → 포탈 간 뒤섞임 없음
                bIsHot = false;
                bShouldCapture = false;

                // Hot→Cold 전환 시 마지막으로 자기 ColdRT에 백업 (자기 내용 보존)
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
    if (SceneCapture->TextureTarget != TargetRT)
    {
        SceneCapture->TextureTarget = TargetRT;
    }
    if (DynamicMaterial)
    {
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetLeft"), TargetRT);
        DynamicMaterial->SetTextureParameterValue(FName("RenderTargetRight"), TargetRT);
    }

    SceneCapture->bCaptureEveryFrame = bShouldCapture;

    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        SceneCapture->FOVAngle = 104.0f;
        SceneCapture->bEnableClipPlane = false;

        // TargetCaptureLocation = 레벨 안 카메라 위치 (에디터에서 직접 설정)
        // TargetCaptureRotation = 카메라 회전 오프셋. 플레이어 머리 회전과 합성됨.
        //   - ZeroRotator: 순수 플레이어 머리 회전만 사용 (기본 동작)
        //   - 값 있음: TargetCaptureRotation * PlayerRot 순서로 quaternion 합성
        // TargetViewTransform = 레벨 스폰 위치 (BeginPlay에서만 사용)
        FRotator PlayerRot = Camera->GetComponentRotation();
        FRotator FinalCaptureRot;
        if (TargetCaptureRotation.IsNearlyZero())
        {
            FinalCaptureRot = PlayerRot;
        }
        else
        {
            const FQuat CombinedQ = TargetCaptureRotation.Quaternion() * PlayerRot.Quaternion();
            FinalCaptureRot = CombinedQ.Rotator();
        }
        SceneCapture->SetWorldLocationAndRotation(TargetCaptureLocation, FinalCaptureRot);

        // 디버그: 매 60프레임마다 상태 출력 (bDebugLumenRays 켰을 때만 — 평소 로그 도배 방지)
        if (bDebugLumenRays)
        {
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
    else if (LinkedPortal)
    {
        // LinkedPortal 모드: 클립 플레인 활성화 (포탈 너머만 캡처)
        SceneCapture->bEnableClipPlane = true;
        UpdateSceneCapture();
    }
}

void APortalActor::UpdatePortalFrustumData()
{
    if (!ViewExtension.IsValid()) return;
    if (!IsValid(PortalMesh) || !GetWorld()) return;

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

    FPortalFrustumData FrustumData;
    FrustumData.EyePosition = Camera->GetComponentLocation();

    FVector PortalCenter = PortalMesh->GetComponentLocation();
    FVector Right = PortalMesh->GetRightVector();
    FVector Up = PortalMesh->GetUpVector();
    FVector Scale = PortalMesh->GetComponentScale();
    float HalfWidth = 50.0f * Scale.Y;
    float HalfHeight = 50.0f * Scale.Z;

    FrustumData.Corners[0] = PortalCenter + (Right * HalfWidth) + (Up * HalfHeight);
    FrustumData.Corners[1] = PortalCenter - (Right * HalfWidth) + (Up * HalfHeight);
    FrustumData.Corners[2] = PortalCenter - (Right * HalfWidth) - (Up * HalfHeight);
    FrustumData.Corners[3] = PortalCenter + (Right * HalfWidth) - (Up * HalfHeight);

    FrustumData.PortalBounds = PortalMesh->Bounds.GetBox();
    FrustumData.bIsValid = true;

    ViewExtension->UpdatePortalFrustum(FrustumData);
}

void APortalActor::UpdateSceneCapture()
{
    if (!LinkedPortal) return;

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return;

    UCameraComponent* Camera = PlayerPawn->FindComponentByClass<UCameraComponent>();
    if (!Camera) return;

    FVector  CameraLocation = Camera->GetComponentLocation();
    FRotator CameraRotation = Camera->GetComponentRotation();

    FTransform ThisTransform = GetActorTransform();
    FVector LocalPos = ThisTransform.InverseTransformPosition(CameraLocation);
    FQuat   LocalRot = ThisTransform.InverseTransformRotation(CameraRotation.Quaternion());

    LocalPos.X = -LocalPos.X;
    LocalRot = FQuat(FVector::UpVector, PI) * LocalRot;

    FTransform LinkedTransform = LinkedPortal->GetActorTransform();
    FVector TargetPos = LinkedTransform.TransformPosition(LocalPos);
    FQuat   TargetRot = LinkedTransform.TransformRotation(LocalRot);

    SceneCapture->SetWorldLocationAndRotation(TargetPos, TargetRot.Rotator());

    SceneCapture->FOVAngle = 104.0f;

    // 클립 플레인: LinkedPortal 면 기준으로 너머만 캡처
    SceneCapture->ClipPlaneBase = LinkedPortal->GetActorLocation();
    SceneCapture->ClipPlaneNormal = -LinkedPortal->GetActorForwardVector();
}

void APortalActor::CheckPortalCrossing(UCameraComponent* Camera)
{
    if (!Camera) return;

    FVector CameraPos = Camera->GetComponentLocation();
    FVector PortalPos = GetActorLocation();
    FVector PortalNormal = GetActorForwardVector();

    // 포탈까지 거리가 너무 멀면 체크 안 함
    float DistToPortal = FVector::Dist(CameraPos, PortalPos);
    if (DistToPortal > 200.0f)
    {
        LastDotSign = 0;
        return;
    }

    // 포탈 평면 기준 카메라 부호 (양수=앞, 음수=뒤)
    FVector ToCamera = CameraPos - PortalPos;
    float Dot = FVector::DotProduct(ToCamera, PortalNormal);
    int32 CurrentSign = (Dot >= 0.0f) ? 1 : -1;

    // 앞(+)에서 뒤(-)로 넘어간 순간 = 포탈 통과
    if (LastDotSign == 1 && CurrentSign == -1)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Portal] Camera crossed portal plane → Teleporting"));
        ExecuteTeleport(UGameplayStatics::GetPlayerPawn(GetWorld(), 0));
    }

    LastDotSign = CurrentSign;
}

void APortalActor::ExecuteTeleport(APawn* Pawn)
{
    if (!Pawn) return;

    // 모드 1: TargetLevel
    if (!TargetLevel.IsNull() && StreamingLevel && StreamingLevel->GetLoadedLevel())
    {
        FVector TeleportLocation = TargetCaptureLocation;
        FRotator TeleportRotation = TargetViewTransform.GetRotation().Rotator();
        Pawn->SetActorLocationAndRotation(TeleportLocation, TeleportRotation);
        LastDotSign = 0;

        UE_LOG(LogTemp, Warning, TEXT("[Portal] Teleported to TargetLevel at %s"),
            *TeleportLocation.ToString());
        return;
    }

    // 모드 2: LinkedPortal
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
    LastDotSign = 0;

    UE_LOG(LogTemp, Warning, TEXT("[Portal] Teleported to LinkedPortal"));
}

void APortalActor::DrawLumenDebug(UCameraComponent* Camera)
{
    UWorld* World = GetWorld();
    if (!World || !IsValid(SceneCapture)) return;

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

    for (int32 xi = 0; xi < GridSize; xi++)
    {
        for (int32 yi = 0; yi < GridSize; yi++)
        {
            float OffsetX = (xi - (GridSize - 1) * 0.5f) * Spread / (GridSize - 1);
            float OffsetY = (yi - (GridSize - 1) * 0.5f) * Spread / (GridSize - 1);

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

    for (int32 i = 0; i < 4; i++)
        DrawDebugLine(World, Corners[i], Corners[(i + 1) % 4], FColor::Green, true, -1.f, 0, 2.f);

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

void APortalActor::OnOverlapBegin(UPrimitiveComponent* OverlappedComp,
    AActor* OtherActor,
    UPrimitiveComponent* OtherComp,
    int32 OtherBodyIndex,
    bool bFromSweep,
    const FHitResult& SweepResult)
{
    if (!OtherActor) return;
    UE_LOG(LogTemp, Warning, TEXT("[Teleport] OnOverlapBegin: OtherActor=%s"), *OtherActor->GetName());

    APawn* Pawn = Cast<APawn>(OtherActor);
    if (!Pawn)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Teleport] FAIL: OtherActor is not a Pawn"));
        return;
    }

    // 포탈 앞면에서 진입했는지 확인 (뒤에서 진입 방지)
    FVector ToPlayer = Pawn->GetActorLocation() - GetActorLocation();
    float Dot = FVector::DotProduct(ToPlayer, GetActorForwardVector());
    UE_LOG(LogTemp, Warning, TEXT("[Teleport] Dot product: %.2f (negative = back side, teleport blocked)"), Dot);
    if (Dot < 0.0f) return;

    UE_LOG(LogTemp, Warning, TEXT("[Teleport] TargetLevel.IsNull=%d, StreamingLevel=%d, LoadedLevel=%d"),
        (int32)TargetLevel.IsNull(),
        (int32)(StreamingLevel != nullptr),
        (int32)(StreamingLevel && StreamingLevel->GetLoadedLevel() != nullptr));

    // 모드 1: TargetLevel (스트리밍 레벨로 순간이동)
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

void APortalActor::ApplyPhase1ShowFlags(bool bEnable)
{
    if (!SceneCapture) return;

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
    SceneCapture->PostProcessSettings.bOverride_MotionBlurAmount = bEnable;
    SceneCapture->PostProcessSettings.bOverride_BloomIntensity = bEnable;
    SceneCapture->PostProcessSettings.bOverride_LensFlareIntensity = bEnable;
    SceneCapture->PostProcessSettings.bOverride_VignetteIntensity = bEnable;
    SceneCapture->PostProcessSettings.bOverride_SceneFringeIntensity = bEnable;
    if (bEnable)
    {
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

void APortalActor::ApplyPhase1ShowOnlyActors(bool bEnable)
{
    if (!SceneCapture) return;

    if (bEnable && StreamingLevelActors.Num() > 0)
    {
        // ── ShowOnlyList 모드: StreamingLevelActors만 렌더 후보로 ──
        // 메인 씬(Showcase) 액터 전부 컷 → Basepass / Shadow / Lumen 모두 감소
        SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        SceneCapture->ShowOnlyActors.Reset();
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

float APortalActor::GetCapturePriority() const
{
    return ComputePortalPriority();
}

float APortalActor::ComputePortalPriority() const
{
    // 우선순위 점수 = 거리 × 화면면적 × 시선 (가까울수록·클수록·정면일수록 높음)
    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!PlayerPawn) return 0.f;

    const FVector PlayerLoc = PlayerPawn->GetActorLocation();
    const FVector PortalLoc = GetActorLocation();
    const float Dist = FVector::Dist(PlayerLoc, PortalLoc);

    // 거리 기반 (가까울수록 높음): 1000cm 기준 정규화
    const float DistScore = 1000.0f / FMath::Max(Dist, 1.0f);

    // 화면 크기 기반 (포탈 메시 스케일): Y * Z 면적
    const FVector Scale = PortalMesh ? PortalMesh->GetComponentScale() : FVector::OneVector;
    const float AreaScore = Scale.Y * Scale.Z;

    // 시선 기반: 카메라가 포탈을 향할수록 높고, 등 뒤(시야 밖)면 거의 0.
    //   Facing = 1(정면) / 0(옆) / -1(등 뒤)  →  [0.05, 1.2] 로 매핑
    float GazeScore = 1.0f;
    if (const UCameraComponent* Cam = PlayerPawn->FindComponentByClass<UCameraComponent>())
    {
        const FVector ToPortal = (PortalLoc - Cam->GetComponentLocation()).GetSafeNormal();
        const float Facing = static_cast<float>(FVector::DotProduct(Cam->GetForwardVector(), ToPortal));
        // Facing[-1,1] → t[0,1] → GazeScore[0.05, 1.2] 선형 매핑
        const float t = FMath::Clamp((Facing + 1.0f) * 0.5f, 0.0f, 1.0f);
        GazeScore = 0.05f + t * (1.2f - 0.05f);
    }

    return DistScore * AreaScore * GazeScore;
}

void APortalActor::DoFinalColdCapture()
{
    if (!SceneCapture || !ColdRenderTarget) return;

    // 마지막 1회 ColdRT 캡처 — Hot에서 Cold로 전환되기 전 fallback 갱신
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
    IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(CVarName);
    if (!CVar)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Portal-Toggle] CVar '%s' 를 찾을 수 없습니다."), CVarName);
        return;
    }

    CVar->Set(bOn ? TEXT("1") : TEXT("0"), ECVF_SetByConsole);

    const FString StateText = bOn ? TEXT("ON") : TEXT("OFF");
    UE_LOG(LogTemp, Display, TEXT("[Portal-Toggle] %s → %s (%s)"),
        *DisplayLabel, *StateText, CVarName);

    if (GEngine)
    {
        // CVarName 해시로 고정 키를 만들어, 같은 항목은 화면에서 갱신되도록 함
        const uint64 Key = (uint64)GetTypeHash(FString(CVarName));
        const FColor Color = bOn ? FColor::Green : FColor::Red;
        GEngine->AddOnScreenDebugMessage(
            Key, 4.0f, Color,
            FString::Printf(TEXT("%s : %s"), *DisplayLabel, *StateText));
    }
}

void APortalActor::ApplyFeatureToggles()
{
    ApplyCVar(TEXT("r.Portal.Enable"),        bEnablePortal,         TEXT("포탈 전체"));
    ApplyCVar(TEXT("r.Portal.Phase1"),        bEnablePhase1,         TEXT("Phase 1 (SceneCapture 최적화)"));
    ApplyCVar(TEXT("r.Portal.Phase2"),        bEnablePhase2,         TEXT("Phase 2 (Frame Budget)"));
    ApplyCVar(TEXT("r.Portal.Phase3"),        bEnablePhase3,         TEXT("Phase 3 (RT Memory Pool)"));
    ApplyCVar(TEXT("r.Portal.FrustumCulling"), bEnableFrustumCulling, TEXT("Frustum Culling"));
}

#if WITH_EDITOR
void APortalActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

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
