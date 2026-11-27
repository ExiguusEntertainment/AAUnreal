#pragma once

#include "CoreMinimal.h"

class UWorld;
class ASceneCapture2D;
class USceneCaptureComponent2D;

ASceneCapture2D* SpawnWadReviewCamera(UWorld& World, int32 ImageSize, const FVector& Position, const FRotator& Rotation);
bool SaveWadReviewFrame(const USceneCaptureComponent2D& Capture, const FString& Output, const TCHAR* Name, int32 Size);

bool StartNativeL30Review(UWorld* World, const FString& OutputPath, int32 ImageSize);
bool StartNativeL30DoorReview(UWorld* World, const FString& OutputPath, int32 ImageSize);
bool StartNativeWadLightingReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bDayClock = false);
bool StartNativeL30PlayerReview(UWorld* World, const FString& OutputPath, int32 ImageSize);
bool StartNativeWadWalkReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bLevelLink, bool bSlice,
    bool bSecondMap, bool bCampaign, bool bCheckpoint);
bool StartNativeWadRiverReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bReload);
bool StartNativeWadExtensionReview(UWorld* World, const FString& OutputPath, int32 ImageSize, const FString& Mode);

bool StartNativeWadMagicMapReview(UWorld* World, const FString& OutputPath, bool bReload);
