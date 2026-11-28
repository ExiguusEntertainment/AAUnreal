#pragma once

#include "CoreMinimal.h"
#include "NativeWadArt.h"

class UTexture2D;

// Named map surfaces use the original PictureLock DRK42 fallback when absent.
// Empty/no-texture markers and absent sky markers do not become opaque surfaces.
AAORIGINAL_API UTexture2D* LoadWadSurfaceTexture(const FString& TextureName);
// Reports authored data availability, without treating DRK42 substitution as a match.
AAORIGINAL_API bool HasWadSurfaceTexture(const FString& TextureName);
AAORIGINAL_API bool LoadWadBackdrop(const FString& PictureName, FIntPoint& Size, TArray<FColor>& Pixels,
    EWadArtPalette Palette=EWadArtPalette::InterfaceGame);
AAORIGINAL_API bool LoadWadSprite(const FString& PicturePath, FIntPoint& Size, TArray<FColor>& Pixels,
    TConstArrayView<uint8> ColorTable = {},EWadArtPalette Palette=EWadArtPalette::InterfaceGame);
bool LoadWadInterfacePalette(TArray<FColor>& Palette);
bool LoadWadInterfacePicture(const FString& PicturePath, FIntPoint& Size, TArray<FColor>& Pixels);

AAORIGINAL_API bool ReadWadSurfaceResource(const FString& Name,FString& ResolvedName,TArray<uint8>& Resource);
