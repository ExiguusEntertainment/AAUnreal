#include "Misc/AutomationTest.h"
#include "../../AAOriginal/Private/WadSurfaceTextures.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginal_WadSurfaceFallback,
    "AAOriginal.WAD.Textures.SourceMissingPictureFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginal_WadSurfaceFallback::RunTest(const FString& Parameters)
{
    const FString Art = FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
    TArray<uint8> Resource, Palette;
    if (!TestTrue(TEXT("Original surface archive is available"), FFileHelper::LoadFileToArray(Resource, *(Art / TEXT("PICS.RES"))))
        || !TestTrue(TEXT("Original surface palette is available"), FFileHelper::LoadFileToArray(Palette, *(Art / TEXT("GAME.PAL"))))
        || !TestTrue(TEXT("Archive and palette headers are complete"), Resource.Num() >= 14 && Palette.Num() >= 768))
    {
        return false;
    }
    TArray<uint8> View;
    if(!FFileHelper::LoadFileToArray(View,*(Art/TEXT("VIEW00.PAL")))||View.Num()!=768){return false;}
    // Original ViewSetPalette updates only the lower224 entries.
    FMemory::Memcpy(Palette.GetData(),View.GetData(),224*3);
    const auto Read = [&Resource](int32 Offset, int32 Width)
    {
        uint32 Value = 0;
        for (int32 Byte = 0; Byte < Width; ++Byte) { Value |= uint32(Resource[Offset + Byte]) << (8 * Byte); }
        return Value;
    };
    const uint32 Index = Read(4, 4), IndexSize = Read(8, 4), Entries = Read(12, 2);
    if (!TestTrue(TEXT("Source resource index is valid"), Read(0, 4) == 0x21736552u
        && IndexSize == Entries * 39 && uint64(Index) + IndexSize <= uint64(Resource.Num())))
    {
        return false;
    }
    // Read the source bytes independently of the loader, including column order,
    // palette conversion and transparency, so sharing a fallback pointer is not
    // sufficient to pass this test.
    const auto Picture = [&](const ANSICHAR* Name, FIntPoint& Size, TArray<FColor>& Expected, FSHAHash& Hash)
    {
        for (uint32 Item = 0; Item < Entries; ++Item)
        {
            const int32 Entry = int32(Index + Item * 39);
            ANSICHAR EntryName[15] = {};
            FMemory::Memcpy(EntryName, Resource.GetData() + Entry + 4, 14);
            if (FCStringAnsi::Strcmp(EntryName, Name) != 0) { continue; }
            const uint32 Offset = Read(Entry + 18, 4), Length = Read(Entry + 22, 4);
            if (Length < 4 || uint64(Offset) + Length > uint64(Resource.Num())) { return false; }
            Size = FIntPoint(Read(Offset + 2, 2), Read(Offset, 2));
            const int64 Count = int64(Size.X) * Size.Y;
            if (Size.X <= 0 || Size.Y <= 0 || Count > int64(Length) - 4) { return false; }
            FSHA1::HashBuffer(Resource.GetData() + Offset, Length, Hash.Hash);
            Expected.SetNumUninitialized(int32(Count));
            for (int32 Y = 0; Y < Size.Y; ++Y)
            {
                for (int32 X = 0; X < Size.X; ++X)
                {
                    const uint8 Color = Resource[Offset + 4 + X * Size.Y + Y];
                    Expected[Y * Size.X + X] = FColor(
                        (int32(Palette[Color * 3]) & 63) * 255 / 63,
                        (int32(Palette[Color * 3 + 1]) & 63) * 255 / 63,
                        (int32(Palette[Color * 3 + 2]) & 63) * 255 / 63,
                        Color == 0 ? 0 : 255);
                }
            }
            return true;
        }
        return false;
    };
    const auto Matches = [&](const TCHAR* Name, UTexture2D* Texture, FIntPoint Size, const TArray<FColor>& Expected)
    {
        if (!TestNotNull(Name, Texture)
            || !TestEqual(FString(Name) + TEXT(" dimensions"), FIntPoint(Texture->GetSizeX(), Texture->GetSizeY()), Size)
            || !TestTrue(FString(Name) + TEXT(" decoded mip exists"), Texture->GetPlatformData() && !Texture->GetPlatformData()->Mips.IsEmpty()))
        {
            return false;
        }
        FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
        const void* Pixels = Mip.BulkData.LockReadOnly();
        const bool bMatches = Pixels && Mip.BulkData.GetBulkDataSize() == Expected.Num() * sizeof(FColor)
            && FMemory::Memcmp(Pixels, Expected.GetData(), Expected.Num() * sizeof(FColor)) == 0;
        Mip.BulkData.Unlock();
        return TestTrue(FString(Name) + TEXT(" matches every source pixel"), bMatches);
    };

    FIntPoint FallbackSize;
    TArray<FColor> FallbackPixels;
    FSHAHash FallbackHash;
    if (!TestTrue(TEXT("DRK42 exists in the source archive"), Picture("DRK42", FallbackSize, FallbackPixels, FallbackHash))) { return false; }
    TestEqual(TEXT("Known original DRK42 dimensions"), FallbackSize, FIntPoint(64, 64));
    TestEqual(TEXT("Known original DRK42 payload checksum"), FallbackHash.ToString(), FString(TEXT("27211F7885363BADF55B1D989E2FC8657BC8ECB7")));
    TestTrue(TEXT("Fallback source art is genuinely available"), HasWadSurfaceTexture(TEXT("DRK42")));
    Matches(TEXT("DRK42"), LoadWadSurfaceTexture(TEXT("DRK42")), FallbackSize, FallbackPixels);
    const TCHAR* Missing[] = {TEXT("MARBGRAY"), TEXT("AASHITTY"), TEXT("MARBLE1"), TEXT("ASHWALL2"),
        TEXT("STARTAN3"), TEXT("BLOOD3"), TEXT("CEIL3_2"), TEXT("FLOOR4_8")};
    for (const TCHAR* Name : Missing)
    {
        TestFalse(FString(Name) + TEXT(" remains diagnosed as absent authored art"), HasWadSurfaceTexture(Name));
        Matches(Name, LoadWadSurfaceTexture(Name), FallbackSize, FallbackPixels);
        TestFalse(FString(Name) + TEXT(" fallback does not disguise source availability"), HasWadSurfaceTexture(Name));
    }
    Matches(TEXT("Normalized MARBGRAY"), LoadWadSurfaceTexture(TEXT(" marbgray ")), FallbackSize, FallbackPixels);

    FIntPoint ValidSize;
    TArray<FColor> ValidPixels;
    FSHAHash ValidHash;
    if (TestTrue(TEXT("Valid rectangular T00100 source exists"), Picture("T00100", ValidSize, ValidPixels, ValidHash)))
    {
        TestTrue(TEXT("Valid texture bypasses missing-material refresh"), HasWadSurfaceTexture(TEXT("T00100")));
        TestEqual(TEXT("Valid rectangular dimensions are unchanged"), ValidSize, FIntPoint(64, 128));
        Matches(TEXT("T00100"), LoadWadSurfaceTexture(TEXT("T00100")), ValidSize, ValidPixels);
    }
    TestNull(TEXT("Empty texture remains absent"), LoadWadSurfaceTexture(TEXT("")));
    TestNull(TEXT("Blank texture remains absent"), LoadWadSurfaceTexture(TEXT("  ")));
    TestNull(TEXT("No-wall marker remains absent"), LoadWadSurfaceTexture(TEXT("-")));
    TestNull(TEXT("Missing sky marker does not acquire an opaque fallback"), LoadWadSurfaceTexture(TEXT("F_SKY999")));
    return true;
}
