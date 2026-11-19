#pragma once

#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "NativeWadPlayer.generated.h"

class UCameraComponent;
class ANativeWadLevel;

UCLASS()
class AAORIGINAL_API ANativeWadPlayer : public ACharacter
{
    GENERATED_BODY()

public:
    ANativeWadPlayer();
    virtual void BeginPlay() override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FVector GetPawnViewLocation() const override;

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool Use();

    static constexpr float CapsuleRadius = 10.0f;
    static constexpr float CapsuleHalfHeight = 27.0f;

private:
    void MoveInViewDirection(float Forward, float Right);
    void UsePressed();
    void CheckLevelLink();

    TWeakObjectPtr<ANativeWadLevel> NativeLevel;
    int32 LastLevelLink = INDEX_NONE;
    bool bCheckedLevelLink = false;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UCameraComponent> ViewCamera;
};

UCLASS()
class AAORIGINAL_API ANativeWadGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ANativeWadGameMode();
};
