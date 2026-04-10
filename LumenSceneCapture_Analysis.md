# UE5.5 SceneCapture2D에서 Lumen GI가 동작하지 않는 문제 분석 및 해결

## 개요

UE5.5 환경에서 포탈 렌더링 구현 중 `SceneCaptureComponent2D`를 통해 타겟 레벨을 캡처할 때,  
발광(Emissive) 오브젝트는 보이지만 **벽·천장에 간접광(Indirect Lighting)이 전혀 나타나지 않는 문제**가 발생했다.

> 증상: 포탈 화면에 발광 구체는 렌더링되지만, 주변 벽·천장이 완전히 어둡게 보임.  
> Lumen GI(간접광)가 SceneCapture 뷰에 적용되지 않은 상태.

---

## 원인 분석

엔진 소스코드를 직접 추적하여 두 개의 독립적인 차단 지점을 발견했다.

---

### 원인 1 — `SceneCaptureRendering.cpp`: Lumen 데이터가 매 프레임 삭제됨

**파일 위치**  
`Engine/Source/Runtime/Renderer/Private/SceneCaptureRendering.cpp`

**문제 코드**

```cpp
if (ViewStateInterface &&
    (SceneRenderer->Views[0].FinalPostProcessSettings.DynamicGlobalIlluminationMethod
         == EDynamicGlobalIlluminationMethod::Lumen ||
     SceneRenderer->Views[0].FinalPostProcessSettings.ReflectionMethod
         == EReflectionMethod::Lumen))
{
    ViewStateInterface->AddLumenSceneData(this, ...);  // Lumen 데이터 생성
}
else if (ViewStateInterface)
{
    ViewStateInterface->RemoveLumenSceneData(this);    // Lumen 데이터 삭제 ← 문제
}
```

**설명**  
SceneCapture가 렌더링을 시작할 때마다 이 코드가 실행된다.  
`DynamicGlobalIlluminationMethod`가 명시적으로 `Lumen`으로 설정되어 있지 않으면  
`RemoveLumenSceneData()`가 호출되어 **Lumen Surface Cache 및 Radiance Cache 데이터가 삭제**된다.

즉, SceneCapture 컴포넌트의 PostProcess 설정에서 GI 방식을 Lumen으로 지정하지 않으면  
렌더링할 때마다 Lumen 데이터가 초기화되어 간접광 계산이 불가능한 상태가 된다.

---

### 원인 2 — `LumenDiffuseIndirect.cpp`: Lumen GI 패스 자체가 실행되지 않음

**파일 위치**  
`Engine/Source/Runtime/Renderer/Private/Lumen/LumenDiffuseIndirect.cpp`

**문제 코드**

```cpp
bool ShouldRenderLumenDiffuseGI(const FScene* Scene, const FSceneView& View, ...)
{
    return Lumen::IsLumenFeatureAllowedForView(Scene, View, ...)
        && View.FinalPostProcessSettings.DynamicGlobalIlluminationMethod
               == EDynamicGlobalIlluminationMethod::Lumen
        && CVarLumenGlobalIllumination.GetValueOnAnyThread()
        && View.Family->EngineShowFlags.GlobalIllumination
        && View.Family->EngineShowFlags.LumenGlobalIllumination   // ← 핵심
        && !HasRayTracedOverlay(*View.Family)
        && (...);
}
```

**설명**  
이 함수가 `false`를 반환하면 Lumen Diffuse GI 패스 전체가 스킵된다.  
조건 중 `EngineShowFlags.LumenGlobalIllumination`이 포함되어 있는데,  
이 플래그는 일반 `GlobalIllumination` 플래그와 **별개**로 존재한다.

SceneCapture의 ShowFlags에서 `SetGlobalIllumination(true)`만 설정하고  
`SetLumenGlobalIllumination(true)`를 누락하면, 이 함수는 항상 `false`를 반환하여  
**Lumen GI 렌더링 패스 자체가 실행되지 않는다**.

---

### 참고 — `IsLumenFeatureAllowedForView` 분석

**파일 위치**  
`Engine/Source/Runtime/Renderer/Private/Lumen/Lumen.cpp`

```cpp
bool Lumen::IsLumenFeatureAllowedForView(const FScene* Scene, const FSceneView& View, ...)
{
    return View.Family
        && DoesRuntimePlatformSupportLumen()
        && ShouldRenderLumenForViewFamily(...)
        && !View.bIsPlanarReflection
        && !View.bIsReflectionCapture
        && View.State          // ← ViewState가 없으면 Lumen 비활성화
        && (...);
}
```

주목할 점: `bIsSceneCapture`에 대한 명시적인 차단 조건이 없다.  
즉, **SceneCapture2D는 Lumen 사용 자체가 금지된 것이 아니며**, 조건만 올바르게 충족하면 동작한다.

단, `View.State`가 null이면 Lumen이 비활성화된다.  
SceneCapture 컴포넌트에서 `bAlwaysPersistRenderingState = true`를 설정해야  
persistent ViewState가 생성되어 이 조건을 통과할 수 있다.

---

## 해결 방법

`PortalActor.cpp`의 `BeginPlay()`에 다음 코드를 추가했다.

```cpp
// 기존 설정
SceneCapture->ShowFlags.SetGlobalIllumination(true);

// 추가 1: ShouldRenderLumenDiffuseGI()의 LumenGlobalIllumination 조건 충족
SceneCapture->ShowFlags.SetLumenGlobalIllumination(true);

// 추가 2: SceneCaptureRendering.cpp에서 AddLumenSceneData() 호출되도록
SceneCapture->PostProcessSettings.bOverride_DynamicGlobalIlluminationMethod = true;
SceneCapture->PostProcessSettings.DynamicGlobalIlluminationMethod =
    EDynamicGlobalIlluminationMethod::Lumen;

// 기존 설정 (Lumen View.State 조건 충족)
SceneCapture->bAlwaysPersistRenderingState = true;
```

---

## 결과

| 항목 | 수정 전 | 수정 후 |
|------|---------|---------|
| 발광 오브젝트 직접광 | ✅ 표시됨 | ✅ 표시됨 |
| 벽·천장 Lumen 간접광 | ❌ 완전히 어두움 | ✅ 간접광 정상 표시 |
| Lumen Surface Cache | ❌ 매 프레임 삭제 | ✅ 유지 및 누적 |
| Lumen GI 패스 실행 | ❌ 스킵됨 | ✅ 정상 실행 |

---

## 의의

UE5.5 공식 문서에는 SceneCapture2D에서 Lumen GI를 활성화하는 방법이 명시되어 있지 않다.  
`SetGlobalIllumination(true)`만으로는 충분하지 않으며, 엔진 소스를 직접 분석하지 않으면  
발견하기 어려운 두 가지 조건이 숨겨져 있다.

이번 분석을 통해 확인한 **SceneCapture2D에서 Lumen GI 활성화를 위한 필수 조건**:

1. `bAlwaysPersistRenderingState = true` — ViewState 유지 (Lumen 시간적 누적 가능)
2. `ShowFlags.SetLumenGlobalIllumination(true)` — GI 패스 실행 조건 충족
3. `PostProcessSettings.DynamicGlobalIlluminationMethod = Lumen` — Lumen 데이터 삭제 방지

세 조건 중 하나라도 누락되면 SceneCapture에서 Lumen GI가 동작하지 않는다.
