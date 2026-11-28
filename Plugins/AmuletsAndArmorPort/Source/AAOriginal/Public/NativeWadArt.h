#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "NativeWadArt.generated.h"

class UTexture2D;

UENUM()
enum class EWadArtPalette : uint8
{
    // Deliberately retains approved native UI art. Original in-level VGA state
    // installs VIEW00 globally for0..223, including UI pixels using those indices.
    InterfaceGame,
    WorldStandard,
};
UENUM()
enum class EWadArtEncoding : uint8 { Surface, Sprite, InterfacePicture, Backdrop };

struct FWadIndexedMip
{
    FIntPoint Size=FIntPoint::ZeroValue;
    TArray<uint8> Indices; // Row-major, independent of texture compilation/platform data.
};
struct AAORIGINAL_API FWadIndexedMipChain
{
    FIntPoint OriginalSize=FIntPoint::ZeroValue;
    TArray<FWadIndexedMip> Mips;
    int32 UploadMipCount() const;
};

USTRUCT()
struct AAORIGINAL_API FWadArtProvenance
{
    GENERATED_BODY()
    UPROPERTY() int32 Version=1;
    UPROPERTY() EWadArtPalette Palette=EWadArtPalette::InterfaceGame;
    UPROPERTY() EWadArtEncoding Encoding=EWadArtEncoding::InterfacePicture;
    UPROPERTY() FString ResourcePath; // Relative to original ArtFinal, never an absolute host path.
    UPROPERTY() FString ArchiveEntry; // Full original PICS entry; empty for loose pictures.
    UPROPERTY() FIntPoint OriginalSize=FIntPoint::ZeroValue;
    UPROPERTY() bool bZeroTransparent=false;
    UPROPERTY() bool bAuthoredMips=false;
    UPROPERTY() TArray<int32> SourceTypes; // Full source identities sharing this exact frame binding.
    UPROPERTY() bool bMirrored=false; // Binding metadata; source pixels remain unflipped.
    UPROPERTY() TArray<uint8> ColorTable; // Source object-variant mapping before palette lookup.
    UPROPERTY() FString SourceSHA1; // Exact original resource-entry/loose-file bytes.
    UPROPERTY() TArray<FString> MipSHA1; // Exact generated row-major BGRA bytes, in mip order.
    bool IsValid() const;
};

UCLASS()
class AAORIGINAL_API UNativeWadArtSource : public UAssetUserData
{
    GENERATED_BODY()
public:
    UPROPERTY() FWadArtProvenance Source;
};

namespace NativeWadArt
{
    constexpr int32 AuthoredMipLevels=5;
    constexpr int32 PaletteBytes=256*3;
    constexpr int32 ViewColors=224;
    constexpr int32 MipStep=0x1C000;
    // Pure/transactional decoders: failure leaves the output unchanged.
    AAORIGINAL_API bool DecodePalette(TConstArrayView<uint8> Game,TConstArrayView<uint8> View,
        EWadArtPalette Domain,TArray<FColor>& Out);
    AAORIGINAL_API bool LoadPalette(EWadArtPalette Domain,TArray<FColor>& Out);
    AAORIGINAL_API bool DecodeSurfaceMips(TConstArrayView<uint8> Resource,FWadIndexedMipChain& Out);
    AAORIGINAL_API bool ExpandPixels(TConstArrayView<uint8> Indices,TConstArrayView<FColor> Palette,
        bool bZeroTransparent,TConstArrayView<uint8> ColorTable,TArray<FColor>& Out);
    // Exact original integer decision for reference tests. Native GPU derivatives
    // are a separate approximation and must preserve these per-surface exclusions.
    AAORIGINAL_API int32 SelectSurfaceMip(FIntPoint Size,bool bPlane,int32 StepX,int32 StepY=0);
    AAORIGINAL_API FString SHA1(TConstArrayView<uint8> Bytes);
    AAORIGINAL_API UTexture2D* CreateSurfaceTexture(TConstArrayView<uint8> Resource,const FString& Entry,EWadArtPalette Domain=EWadArtPalette::WorldStandard);
    // Metadata only: this never regenerates pixels or changes material properties.
    AAORIGINAL_API bool RecordSource(UTexture2D& Texture,const FWadArtProvenance& Source);
    AAORIGINAL_API const FWadArtProvenance* GetSource(const UTexture2D& Texture);
    AAORIGINAL_API bool RecordPicture(UTexture2D& Texture,EWadArtEncoding Encoding,EWadArtPalette Palette,
        const FString& RelativePath,FIntPoint Size,TConstArrayView<FColor> Pixels,
        bool bZeroTransparent,TConstArrayView<uint8> ColorTable={});
}
