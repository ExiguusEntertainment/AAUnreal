#pragma once

#include "CoreMinimal.h"

class UTexture2D;

// Named map surfaces use the original PictureLock DRK42 fallback when absent.
// Empty/no-texture markers and absent sky markers do not become opaque surfaces.
AAORIGINAL_API UTexture2D* LoadWadSurfaceTexture(const FString& TextureName);
// Reports authored data availability, without treating DRK42 substitution as a match.
AAORIGINAL_API bool HasWadSurfaceTexture(const FString& TextureName);
bool LoadWadBackdrop(const FString& PictureName, FIntPoint& Size, TArray<FColor>& Pixels);
bool LoadWadSprite(const FString& PicturePath, FIntPoint& Size, TArray<FColor>& Pixels,
    TConstArrayView<uint8> ColorTable = {});
bool LoadWadInterfacePalette(TArray<FColor>& Palette);
bool LoadWadInterfacePicture(const FString& PicturePath, FIntPoint& Size, TArray<FColor>& Pixels);
