#pragma once
#include "CoreMinimal.h"

// A real importer request identity, not a guessed filename or RGB match.
// Multiple full CREDATA identities may intentionally share one identical material.
struct FWadArtRequest
{
    FString AssetName;
    FString Picture; // Exact OBJS-relative CPC path.
    TArray<int32> SourceTypes;
    TArray<uint8> ColorTable;
    bool bMirrored=false; // Actual imported static view binding; never inferred from a name.
    FString Role;
};


AAORIGINAL_API bool ReadWadCreatureArtRequests(TArray<FWadArtRequest>& Out,FOutputDevice& Output);
AAORIGINAL_API bool ReadWadFixedArtRequest(const FString& Key,FWadArtRequest& Out);
AAORIGINAL_API bool ResolveWadScriptMissileArt(int32 Type,FWadArtRequest& Out);

AAORIGINAL_API bool ReadWadSharedProjectileArtRequest(const FString& Key,FWadArtRequest& Out);
AAORIGINAL_API bool ReadWadSharedProjectileArtRequests(TArray<FWadArtRequest>& Out);

// A texture uses the original unmirrored CPC. Signed per-view mirroring remains
// in FWadScenerySourceFrame and the imported scenery definition, not baked here.
AAORIGINAL_API bool ReadWadSceneryArtRequest(const FString& Resource,int32 PictureId,FWadArtRequest& Out);
