#include "NativeWadPlayer.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "NativeWadLevel.h"

ANativeWadPlayer::ANativeWadPlayer()
{
    GetCapsuleComponent()->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
    BaseEyeHeight = 22.0f;
    bUseControllerRotationYaw = true;
    GetCharacterMovement()->MaxWalkSpeed = 190.0f;
    GetCharacterMovement()->MaxStepHeight = 24.0f;
    ViewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
    ViewCamera->SetupAttachment(GetCapsuleComponent());
    ViewCamera->SetRelativeLocation(FVector(0, 0, BaseEyeHeight));
    ViewCamera->bUsePawnControlRotation = true;
}

void ANativeWadPlayer::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    Input->BindKey(EKeys::E, IE_Pressed, this, &ANativeWadPlayer::UsePressed);
    Input->BindAxisKey(EKeys::MouseX, this, &ANativeWadPlayer::AddControllerYawInput);
    Input->BindAxisKey(EKeys::MouseY, this, &ANativeWadPlayer::AddControllerPitchInput);
    Input->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ANativeWadPlayer::Jump);
    Input->BindKey(EKeys::SpaceBar, IE_Released, this, &ANativeWadPlayer::StopJumping);
}

void ANativeWadPlayer::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (const APlayerController* Player = Cast<APlayerController>(GetController()); Player && Player->IsLocalController())
    {
        const float Forward = float(Player->IsInputKeyDown(EKeys::W)) - float(Player->IsInputKeyDown(EKeys::S));
        const float Right = float(Player->IsInputKeyDown(EKeys::D)) - float(Player->IsInputKeyDown(EKeys::A));
        MoveInViewDirection(Forward, Right);
    }
}

void ANativeWadPlayer::MoveInViewDirection(float Forward, float Right)
{
    const FRotator Yaw(0.0, GetControlRotation().Yaw, 0.0);
    const FRotationMatrix Basis(Yaw);
    AddMovementInput(Basis.GetUnitAxis(EAxis::X), Forward);
    AddMovementInput(Basis.GetUnitAxis(EAxis::Y), Right);
}

FVector ANativeWadPlayer::GetPawnViewLocation() const
{
    return ViewCamera->GetComponentLocation();
}

bool ANativeWadPlayer::Use()
{
    for (TActorIterator<ANativeWadLevel> Level(GetWorld()); Level; ++Level)
    {
        if (Level->UseDoorFromView(GetPawnViewLocation(), GetControlRotation().Vector(), 112.0, this))
        {
            return true;
        }
    }
    return false;
}

void ANativeWadPlayer::UsePressed()
{
    Use();
}

ANativeWadGameMode::ANativeWadGameMode()
{
    DefaultPawnClass = ANativeWadPlayer::StaticClass();
}
