#include "DoomWadLoader.h"
#include "DoomWadTypes.h"
#include "AAOriginalAPI.h"
#include "CoreMinimal.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDoomWad, Log, All);

namespace AA
{
#pragma pack(push,1)
    struct FDoomThing { int16 X; int16 Y; int16 Angle; int16 Type; int16 Flags; };
#pragma pack(pop)

    struct FLumpRef { int32 Offset = 0; int32 Size = 0; };

    static bool ReadWholeFile(const FString& AbsPath, TArray<uint8>& OutData)
    {
        if (!FFileHelper::LoadFileToArray(OutData, *AbsPath))
        {
            UE_LOG(LogDoomWad, Error, TEXT("Failed to read WAD file %s"), *AbsPath);
            return false;
        }
        return true;
    }

    static FString MakeUpper8(const char* Name)
    {
        TCHAR Temp[9] = {0};
        for (int i = 0; i < 8; i++)
        {
            char c = Name[i];
            if (c == 0) break;
            Temp[i] = TCHAR(FChar::ToUpper((TCHAR)c));
        }
        return FString(Temp).TrimEnd();
    }

    class FDoomWadParser
    {
    public:
        bool Load(const FString& WadPath, const FString& MapName)
        {
            Data.Reset();
            if (!ReadWholeFile(WadPath, Data)) return false;
            if (Data.Num() < (int32)sizeof(T_wadHeader)) { UE_LOG(LogDoomWad, Error, TEXT("File too small.")); return false; }

            const T_wadHeader* Header = reinterpret_cast<const T_wadHeader*>(Data.GetData());
            FString Sig = FString(ANSI_TO_TCHAR((const char*)Header->Signature)).Left(4);
            if (!(Sig == TEXT("IWAD") || Sig == TEXT("PWAD")))
            {
                UE_LOG(LogDoomWad, Error, TEXT("Bad WAD signature: %s"), *Sig);
                return false;
            }
            int32 NumLumps = Header->NumLumps;
            int32 DirOffset = Header->DirOffset;
            if (DirOffset + NumLumps * (int32)sizeof(T_directoryEntry) > Data.Num())
            {
                UE_LOG(LogDoomWad, Error, TEXT("Directory out of range."));
                return false;
            }
            const T_directoryEntry* Dir = reinterpret_cast<const T_directoryEntry*>(Data.GetData() + DirOffset);
            FString Wanted = MapName.ToUpper();
            int32 MapIndex = -1;
            for (int32 i = 0; i < NumLumps; i++)
            {
                FString Name = MakeUpper8((const char*)Dir[i].Name);
                if (Name == Wanted)
                {
                    MapIndex = i;
                    break;
                }
            }
            if (MapIndex == -1)
            {
                UE_LOG(LogDoomWad, Error, TEXT("Map marker %s not found."), *MapName);
                return false;
            }
            TMap<FString, FLumpRef> Lumps;
            static const TCHAR* Needed[] = { TEXT("VERTEXES"), TEXT("LINEDEFS"), TEXT("SIDEDEFS"), TEXT("SECTORS") };
            for (int32 i = MapIndex + 1; i < NumLumps; i++)
            {
                const T_directoryEntry& E = Dir[i];
                FString Name = MakeUpper8((const char*)E.Name);
                if (E.Size == 0) break; // next map marker
                FLumpRef Ref; Ref.Offset = E.FilePos; Ref.Size = E.Size; Lumps.Add(Name, Ref);
            }
            for (auto* N : Needed)
            {
                if (!Lumps.Contains(N))
                {
                    UE_LOG(LogDoomWad, Error, TEXT("Missing required lump %s"), N);
                    return false;
                }
            }
            if (Lumps[TEXT("VERTEXES")].Size % sizeof(T_3dVertex) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad VERTEXES size")); return false; }
            if (Lumps[TEXT("LINEDEFS")].Size % sizeof(T_3dLine) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad LINEDEFS size")); return false; }
            if (Lumps[TEXT("SIDEDEFS")].Size % sizeof(T_3dSide) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad SIDEDEFS size")); return false; }
            if (Lumps[TEXT("SECTORS")].Size % sizeof(T_3dSector) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad SECTORS size")); return false; }

            Vertices = MakeArray<T_3dVertex>(Lumps[TEXT("VERTEXES")]);
            Lines = MakeArray<T_3dLine>(Lumps[TEXT("LINEDEFS")]);
            Sides = MakeArray<T_3dSide>(Lumps[TEXT("SIDEDEFS")]);
            Sectors = MakeArray<T_3dSector>(Lumps[TEXT("SECTORS")]);


            UE_LOG(LogDoomWad, Display, TEXT("Loaded map %s: V=%d L=%d Sides=%d Sec=%d"), *MapName, Vertices.Num(), Lines.Num(), Sides.Num(), Sectors.Num());
            return true;
        }

        template<typename T>
        TArray<T> MakeArray(const FLumpRef& Ref)
        {
            TArray<T> Arr;
            const uint8* Src = Data.GetData() + Ref.Offset;
            int32 Count = Ref.Size / sizeof(T);
            Arr.AddUninitialized(Count);
            FMemory::Memcpy(Arr.GetData(), Src, Ref.Size);
            return Arr;
        }

        const TArray<T_3dVertex>& GetVertices() const { return Vertices; }
        const TArray<T_3dLine>& GetLines() const { return Lines; }
        const TArray<T_3dSide>& GetSides() const { return Sides; }
        const TArray<T_3dSector>& GetSectors() const { return Sectors; }

    private:
        TArray<uint8> Data;
        TArray<T_3dVertex> Vertices;
        TArray<T_3dLine> Lines;
        TArray<T_3dSide> Sides;
        TArray<T_3dSector> Sectors;
    };

    static FDoomWadLoadStats G_LastStats;
}

static void UpdateStats(const AA::FDoomWadParser& Parser, bool bSuccess)
{
    FDoomWadLoadStats Stats;
    if (bSuccess)
    {
        Stats.NumVertices = Parser.GetVertices().Num();
        Stats.NumLines = Parser.GetLines().Num();
        Stats.NumSides = Parser.GetSides().Num();
        Stats.NumSectors = Parser.GetSectors().Num();
        Stats.bValid = true;
    }
    AA::G_LastStats = Stats;
}

AAORIGINAL_API bool AA_LoadDoomMap(const FString& WadPath, const FString& MapName)
{
    AA::FDoomWadParser Parser;
    const bool bOk = Parser.Load(WadPath, MapName);
    UpdateStats(Parser, bOk);
    return bOk;
}

bool UDoomWadLoader::LoadDoomWadMap(const FString& WadAbsolutePath, const FString& MapName)
{
    return AA_LoadDoomMap(WadAbsolutePath, MapName);
}

FDoomWadLoadStats UDoomWadLoader::GetLastLoadStats()
{
    return AA::G_LastStats;
}

extern "C" AAORIGINAL_API void AA_GetLastDoomWadStats(int32& OutVerts, int32& OutLines, int32& OutSides, int32& OutSectors, bool& bValid)
{
    const FDoomWadLoadStats S = UDoomWadLoader::GetLastLoadStats();
    OutVerts = S.NumVertices;
    OutLines = S.NumLines;
    OutSides = S.NumSides;
    OutSectors = S.NumSectors;
    bValid = S.bValid;
}
