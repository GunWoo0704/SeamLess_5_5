//#pragma once
//
//#include "CoreMinimal.h"
//#include "Components/SceneCaptureComponent2D.h"
//#include "Engine/TextureRenderTarget2D.h"
//#include "SceneView.h"
//#include "VRPortalSceneCapture.generated.h"
//
//UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
//class SEAMLESS_5_5_API UVRPortalSceneCapture : public USceneCaptureComponent2D
//{
//    GENERATED_BODY()
//
//public:
//    UVRPortalSceneCapture();
//
//    // Stereo Render Targets
//    UPROPERTY(Transient)
//    UTextureRenderTarget2D* LeftEyeTarget;
//
//    UPROPERTY(Transient)
//    UTextureRenderTarget2D* RightEyeTarget;
//
//    // IPD (Inter-Pupillary Distance)
//    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VR Stereo")
//    float InterPupillaryDistance;
//
//    // Override capture function
//    virtual void UpdateSceneCaptureContents(FSceneInterface* Scene) override;
//
//protected:
//    void CaptureSceneStereo(FSceneInterface* Scene);
//    void SetupStereoViewFamily(FSceneViewFamilyContext& ViewFamily,
//        UTextureRenderTarget2D* RenderTarget,
//        bool bIsLeftEye);
//};