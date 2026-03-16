#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "InputActionValue.h"
#include "VRPawn_2.generated.h"

UCLASS()
class SEAMLESS_5_5_API AVRPawn_2 : public APawn
{
    GENERATED_BODY()

public:
    AVRPawn_2();

    virtual void Tick(float DeltaTime) override;
    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:
    virtual void BeginPlay() override;

    // 이동 처리
    void Move(const FInputActionValue& Value);

    // 회전 처리 (스냅 턴)
    void Turn(const FInputActionValue& Value);

public:
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VR")
    UCapsuleComponent* CapsuleComp;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VR")
    USceneComponent* VROrigin;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VR")
    UCameraComponent* VRCamera;

    // Input Actions (에디터에서 할당)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
    class UInputMappingContext* IMC_VR;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
    class UInputAction* IA_Move;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input")
    class UInputAction* IA_Turn;

    // 이동 속도
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VR")
    float MoveSpeed = 300.0f;

    // 스냅 턴 각도
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VR")
    float SnapTurnAngle = 45.0f;

private:
    bool bCanSnapTurn = true;
};