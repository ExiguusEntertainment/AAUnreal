#pragma once

class UTexture2D;

UTexture2D* LoadWadSurfaceTexture(const FString& TextureName);
bool LoadWadBackdrop(const FString& PictureName, FIntPoint& Size, TArray<FColor>& Pixels);
