#include "NativeWadArt.h"
#include "Engine/Texture2D.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
    bool HexHash(const FString& Text)
    {
        if(Text.Len()!=40){return false;}
        for(TCHAR C:Text){if(!((C>=TEXT('0')&&C<=TEXT('9'))||(C>=TEXT('A')&&C<=TEXT('F')))){return false;}}
        return true;
    }
    int32 Word(TConstArrayView<uint8> Bytes,int32 At)
    {return int32(Bytes[At])|(int32(Bytes[At+1])<<8);}
}
int32 FWadIndexedMipChain::UploadMipCount() const
{
    int32 Count=0;
    for(const auto& Mip:Mips){if(Mip.Size.X<=0||Mip.Size.Y<=0){break;}++Count;}
    return Count;
}
bool FWadArtProvenance::IsValid() const
{
    if(Version!=1||uint8(Palette)>uint8(EWadArtPalette::WorldStandard)||uint8(Encoding)>uint8(EWadArtEncoding::Backdrop)
        ||ResourcePath.IsEmpty()||ResourcePath.Len()>512||ResourcePath.Contains(TEXT(".."))||ResourcePath.Contains(TEXT("\\"))
        ||ResourcePath.Contains(TEXT(":"))||ResourcePath.StartsWith(TEXT("/"))||ResourcePath.EndsWith(TEXT("/"))
        ||OriginalSize.X<=0||OriginalSize.Y<=0||OriginalSize.X>4096||OriginalSize.Y>4096
        ||(!ColorTable.IsEmpty()&&ColorTable.Num()!=256)||!HexHash(SourceSHA1)||MipSHA1.IsEmpty()||MipSHA1.Num()>5)
    {return false;}
    for(TCHAR C:ResourcePath){if(C<=32||C>126){return false;}}
    for(TCHAR C:ArchiveEntry){if(!((C>=TEXT('A')&&C<=TEXT('Z'))||(C>=TEXT('0')&&C<=TEXT('9'))||C==TEXT('_'))){return false;}}
    if(Encoding==EWadArtEncoding::Surface)
    {
        if(!ResourcePath.Equals(TEXT("PICS.RES"),ESearchCase::IgnoreCase)||ArchiveEntry.IsEmpty()||ArchiveEntry.Len()>8
            ||ArchiveEntry.Contains(TEXT("/"))||ArchiveEntry.Contains(TEXT("\\"))||!bAuthoredMips){return false;}
        int32 Upload=0;
        for(int32 I=0;I<5;++I){if((OriginalSize.X>>I)<=0||(OriginalSize.Y>>I)<=0){break;}++Upload;}
        if(MipSHA1.Num()!=Upload){return false;}
    }
    else if(!ArchiveEntry.IsEmpty()||bAuthoredMips||MipSHA1.Num()!=1){return false;}
    for(const auto& Hash:MipSHA1){if(!HexHash(Hash)){return false;}}
    return true;
}
namespace NativeWadArt
{
    bool DecodePalette(TConstArrayView<uint8> Game,TConstArrayView<uint8> View,EWadArtPalette Domain,TArray<FColor>& Out)
    {
        if(Game.Num()!=PaletteBytes||uint8(Domain)>uint8(EWadArtPalette::WorldStandard)
            ||(Domain==EWadArtPalette::WorldStandard&&View.Num()!=PaletteBytes)){return false;}
        TArray<FColor> Parsed;Parsed.Reserve(256);
        for(int32 I=0;I<256;++I)
        {
            const auto Bytes=Domain==EWadArtPalette::WorldStandard&&I<ViewColors?View:Game;
            const auto Channel=[&](int32 Offset){return uint8((int32(Bytes[I*3+Offset])&63)*255/63);};
            Parsed.Add(FColor(Channel(0),Channel(1),Channel(2),255));
        }
        Out=MoveTemp(Parsed);return true;
    }
    bool LoadPalette(EWadArtPalette Domain,TArray<FColor>& Out)
    {
        const FString Root=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
        TArray<uint8> Game,View;
        if(!FFileHelper::LoadFileToArray(Game,*(Root/TEXT("GAME.PAL")))){return false;}
        if(Domain==EWadArtPalette::WorldStandard&&!FFileHelper::LoadFileToArray(View,*(Root/TEXT("VIEW00.PAL")))){return false;}
        return DecodePalette(Game,View,Domain,Out);
    }
    bool DecodeSurfaceMips(TConstArrayView<uint8> Resource,FWadIndexedMipChain& Out)
    {
        if(Resource.Num()<4){return false;}
        const FIntPoint Size(Word(Resource,2),Word(Resource,0));
        if(Size.X<=0||Size.Y<=0||Size.X>4096||Size.Y>4096){return false;}
        int64 Expected=0;
        for(int32 Level=0;Level<AuthoredMipLevels;++Level){Expected+=4+int64(Size.X>>Level)*(Size.Y>>Level);}
        if(Expected!=Resource.Num()){return false;}
        FWadIndexedMipChain Parsed;Parsed.OriginalSize=Size;int32 At=0;
        for(int32 Level=0;Level<AuthoredMipLevels;++Level)
        {
            // The shipped resource repeats ORIGINAL height,width at every level.
            // Reduced dimensions come from the level, not the repeated header.
            if(Word(Resource,At)!=Size.Y||Word(Resource,At+2)!=Size.X){return false;}At+=4;
            FWadIndexedMip& Mip=Parsed.Mips.AddDefaulted_GetRef();Mip.Size={Size.X>>Level,Size.Y>>Level};
            const int32 Count=Mip.Size.X*Mip.Size.Y;Mip.Indices.SetNumUninitialized(Count);
            for(int32 Y=0;Y<Mip.Size.Y;++Y)for(int32 X=0;X<Mip.Size.X;++X)
            {Mip.Indices[Y*Mip.Size.X+X]=Resource[At+X*Mip.Size.Y+Y];}
            At+=Count;
        }
        Out=MoveTemp(Parsed);return true;
    }
    bool ExpandPixels(TConstArrayView<uint8> Indices,TConstArrayView<FColor> Palette,bool bZeroTransparent,
        TConstArrayView<uint8> ColorTable,TArray<FColor>& Out)
    {
        if(Palette.Num()!=256||(!ColorTable.IsEmpty()&&ColorTable.Num()!=256)){return false;}
        TArray<FColor> Parsed;Parsed.Reserve(Indices.Num());
        for(uint8 Index:Indices)
        {
            if(Index==0&&bZeroTransparent){Parsed.Add(FColor(0,0,0,0));continue;}
            FColor Pixel=Palette[ColorTable.IsEmpty()?Index:ColorTable[Index]];Pixel.A=255;Parsed.Add(Pixel);
        }
        Out=MoveTemp(Parsed);return true;
    }
    int32 SelectSurfaceMip(FIntPoint Size,bool bPlane,int32 StepX,int32 StepY)
    {
        if(Size.X<=0||Size.Y<=0){return 0;}
        if(bPlane&&(Size.X<64||Size.Y<64||Size.X==256)){return 0;}
        if(!bPlane&&Size.Y==256){return 0;}
        int32 Level=0;FIntPoint Mask=Size-FIntPoint(1,1);
        const auto Large=[](int32 Step){return Step<=-MipStep||Step>=MipStep;};
        while(Level<4)
        {
            if(!bPlane&&(Mask.X<=2||Mask.Y<=2)){break;}
            if(!Large(StepX)&&(!bPlane||!Large(StepY))){break;}
            StepX>>=1;StepY>>=1;Mask.X>>=1;Mask.Y>>=1;++Level;
        }
        return Level;
    }
    FString SHA1(TConstArrayView<uint8> Bytes)
    {
        FSHAHash Hash;FSHA1::HashBuffer(Bytes.GetData(),Bytes.Num(),Hash.Hash);return Hash.ToString();
    }
    bool RecordSource(UTexture2D& Texture,const FWadArtProvenance& Source)
    {
        if(!Source.IsValid()){return false;}
        auto* Data=Cast<UNativeWadArtSource>(Texture.GetAssetUserDataOfClass(UNativeWadArtSource::StaticClass()));
        if(!Data){Data=NewObject<UNativeWadArtSource>(&Texture);Texture.AddAssetUserData(Data);}
        Data->Source=Source;return true;
    }
    const FWadArtProvenance* GetSource(const UTexture2D& Texture)
    {
        const auto* Data=Cast<UNativeWadArtSource>(const_cast<UTexture2D&>(Texture).GetAssetUserDataOfClass(UNativeWadArtSource::StaticClass()));
        return Data?&Data->Source:nullptr;
    }
}
