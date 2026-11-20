#pragma once

class UTexture2D;

UTexture2D* LoadWadSurfaceTexture(const FString& TextureName);
bool LoadWadBackdrop(const FString& PictureName, FIntPoint& Size, TArray<FColor>& Pixels);
bool LoadWadSprite(const FString& PicturePath, FIntPoint& Size, TArray<FColor>& Pixels);
bool LoadWadInterfacePalette(TArray<FColor>& Palette);
bool LoadWadInterfacePicture(const FString& PicturePath, FIntPoint& Size, TArray<FColor>& Pixels);
