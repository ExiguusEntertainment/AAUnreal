#include "NativeWadScenerySource.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"

namespace
{
    FString SourceRoot()
    { return FPaths::ProjectDir() / TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor"); }
    bool Numbers(const FString& Path, TArray<int32>& Values, FString& Error)
    {
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *Path)) { Error = TEXT("Missing ") + Path; return false; }
        for (const FString& Line : Lines)
        {
            const int32 Comment = Line.Find(TEXT(";"));
            const FString Token = (Comment == INDEX_NONE ? Line : Line.Left(Comment)).TrimStartAndEnd();
            int32 Value = 0;
            if (Token.IsEmpty()) { continue; }
            if (!FDefaultValueHelper::ParseInt(Token, Value)) { Error = TEXT("Invalid number in ") + Path; return false; }
            Values.Add(Value);
        }
        return true;
    }
}

bool NativeWadScenerySource::LoadOrdinaryCatalog(TMap<int32, FString>& Resources, FString& Error)
{
    Resources.Reset(); Error.Reset();
    FString Creatures;
    TArray<FString> Lines;
    const FString Root = SourceRoot();
    if (!FFileHelper::LoadFileToString(Creatures, *(Root / TEXT("Include/CREDATA.H")))
        || !FFileHelper::LoadFileToStringArray(Lines, *(Root / TEXT("Resources/ArtFinal/OBJS/OBJS.FL"))))
    { Error = TEXT("Original CREDATA.H or OBJS.FL is unavailable."); return false; }
    TSet<int32> CreatureTypes;
    FRegexMatcher Matcher(FRegexPattern(TEXT("([0-9]+)\\s*,\\s*NAV_LOGIC_")), Creatures);
    while (Matcher.FindNext()) { CreatureTypes.Add(FCString::Atoi(*Matcher.GetCaptureGroup(1))); }
    if (CreatureTypes.IsEmpty()) { Error = TEXT("No exact original creature definitions found."); return false; }
    for (const FString& Line : Lines)
    {
        TArray<FString> Words;
        Line.ParseIntoArrayWS(Words);
        if (Words.Num() < 2 || !FPaths::GetExtension(Words[0]).Equals(TEXT("res"), ESearchCase::IgnoreCase)
            || !Words[1].IsNumeric()) { continue; }
        const int32 Type = FCString::Atoi(*Words[1]);
        if (Type <= 4 || Type == 924 || (Type >= 1500 && Type <= 1543) || CreatureTypes.Contains(Type)
            || FPaths::FileExists(Root / FString::Printf(TEXT("Resources/ArtFinal/INVDESC/DES%05d.TXT"), Type))) { continue; }
        FString Resource = Words[0].Replace(TEXT("\\"), TEXT("/"));
        Resource = FPaths::GetPath(Resource).ToUpper();
        if (Resource.IsEmpty() || Resource.Contains(TEXT("..")) || Resources.Contains(Type))
        { Error = FString::Printf(TEXT("Ambiguous ordinary object resource for exact type%d."), Type); return false; }
        Resources.Add(Type, Resource);
    }
    return !Resources.IsEmpty();
}

bool NativeWadScenerySource::Load(const FString& Resource, FWadScenerySource& Result, FString& Error)
{
    Result = FWadScenerySource(); Error.Reset();
    const FString Directory = SourceRoot() / TEXT("Resources/ArtFinal/OBJS") / Resource;
    const FString Name = Resource == TEXT("DEAD01") ? TEXT("DEAD") : Resource;
    TArray<int32> Values;
    if (!Numbers(Directory / (Name + TEXT(".INF")), Values, Error)) { return false; }
    if (Values.Num() < 12 || Values[0] < 1 || Values[0] > 16 || Values[1] < 0 || Values[1] > 32767
        || Values[2] < 0 || Values[2] > 65535 || Values[7] < 0 || Values[7] > 65535 || Values[5] != 0)
    { Error = TEXT("Unsupported object header/script: ") + Resource; return false; }
    // Native masked billboards do not yet implement original translucent/no-shade ELMARK.
    if (Values[2] & (0x0100 | 0x0400 | 0x4000))
    { Error = TEXT("Unsupported translucent/piecewise/body-part object: ") + Resource; return false; }
    Result.StanceCount = Values[0]; Result.Radius = Values[1]; Result.Attributes = uint16(Values[2]);
    Result.MoveAttributes = uint16(Values[7]);
    const int32 FrameStart = 8 + Result.StanceCount * 4;
    if (Values.Num() < FrameStart) { Error = TEXT("Truncated stance headers: ") + Resource; return false; }
    Result.FrameTicks = Values[9]; Result.AnimationType = Values[10]; Result.NextStance = Values[11];
    // Only the initial STAND stance is mounted; never pretend later authored stances are one animation.
    const int32 Frames = Values[8];
    if (Frames < 1 || Frames > 1024 || Frames > (Values.Num() - FrameStart) / 4
        || Result.FrameTicks < 0 || Result.AnimationType < 0 || Result.AnimationType > 2
        || (Result.NextStance != -1 && Result.NextStance != 0)
        || (Result.AnimationType == 0 && Result.NextStance == -1 && Frames > 1 && Result.FrameTicks > 0))
    { Error = TEXT("Unsupported initial stance transition: ") + Resource; return false; }
    int32 PictureIndex = FrameStart + Frames * 4;
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        const int32 At = FrameStart + Frame * 4, Angles = Values[At];
        if ((Angles != 1 && Angles != 4 && Angles != 8) || PictureIndex + Angles > Values.Num()
            || Values[At + 1] != 0 || Values[At + 2] != 0 || Values[At + 3] != 0)
        { Error = TEXT("Unsupported frame attributes/sounds: ") + Resource; return false; }
        auto& Entry = Result.Frames.AddDefaulted_GetRef();
        for (int32 Angle = 0; Angle < Angles; ++Angle)
        {
            const int32 Picture = Values[PictureIndex++];
            if (Picture == MIN_int32) { Error = TEXT("Invalid signed picture: ") + Resource; return false; }
            Entry.Pictures.Add(Picture);
        }
    }
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *(Directory / (Name + TEXT(".FL")))))
    { Error = TEXT("Missing frame list: ") + Resource; return false; }
    for (const FString& Line : Lines)
    {
        TArray<FString> Words; Line.ParseIntoArrayWS(Words);
        if (Words.Num() < 2 || !Words[1].IsNumeric()
            || !FPaths::GetExtension(Words[0]).Equals(TEXT("CPC"), ESearchCase::IgnoreCase)) { continue; }
        const int32 Id = FCString::Atoi(*Words[1]);
        if (Result.PictureFiles.Contains(Id)) { Error = TEXT("Duplicate frame identity: ") + Resource; return false; }
        Result.PictureFiles.Add(Id, Words[0].ToUpper());
    }
    for (const auto& Frame : Result.Frames)
    { for (const int32 Id : Frame.Pictures)
        { const FString* File = Result.PictureFiles.Find(FMath::Abs(Id));
          if (!File || !FPaths::FileExists(Directory / *File))
          { Error = TEXT("Missing original CPC: ") + Resource; return false; } } }
    return true;
}


bool NativeWadScenerySource::ResolveMountSector(const FString& SourceWadPath, FIntPoint Position,
    int32& Sector, bool& bUsedSectorZeroFallback, FString& Error)
{
    Sector = INDEX_NONE; bUsedSectorZeroFallback = false; Error.Reset();
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *SourceWadPath))
    { Error = TEXT("Original MAP unavailable for source mounting: ") + SourceWadPath; return false; }
    return ResolveMountSector(Bytes, Position, Sector, bUsedSectorZeroFallback, Error);
}

bool NativeWadScenerySource::ResolveMountSector(TConstArrayView<uint8> Bytes, FIntPoint Position,
    int32& Sector, bool& bUsedSectorZeroFallback, FString& Error)
{
    Sector = INDEX_NONE; bUsedSectorZeroFallback = false; Error.Reset();
    const auto Fail = [&]() { Error = TEXT("Malformed original MAP/BLOCKMAP; no mounting fallback applied."); return false; };
    if (Bytes.Num() < 12 || (FMemory::Memcmp(Bytes.GetData(), "PWAD", 4) && FMemory::Memcmp(Bytes.GetData(), "IWAD", 4))) { return Fail(); }
    const auto U16 = [](TConstArrayView<uint8> B, int32 At) { return uint16(B[At]) | uint16(B[At+1]) << 8; };
    const auto U32 = [](TConstArrayView<uint8> B, int32 At)
    { return uint32(B[At]) | uint32(B[At+1])<<8 | uint32(B[At+2])<<16 | uint32(B[At+3])<<24; };
    const uint32 Count = U32(Bytes,4), Directory = U32(Bytes,8);
    if (uint64(Directory)+uint64(Count)*16 > uint64(Bytes.Num())) { return Fail(); }
    TMap<FString,TConstArrayView<uint8>> Lumps;
    for (uint32 I=0; I<Count; ++I)
    {
        const int32 At=int32(Directory+I*16); const uint32 Offset=U32(Bytes,At), Size=U32(Bytes,At+4);
        if (uint64(Offset)+Size>uint64(Bytes.Num())) { return Fail(); }
        ANSICHAR Name[9]={}; FMemory::Memcpy(Name,Bytes.GetData()+At+8,8);
        const FString Key=ANSI_TO_TCHAR(Name);
        if (Lumps.Contains(Key)) { return Fail(); }
        Lumps.Add(Key,Bytes.Slice(int32(Offset),int32(Size)));
    }
    for (const TCHAR* Key:{TEXT("BLOCKMAP"),TEXT("VERTEXES"),TEXT("LINEDEFS"),TEXT("SIDEDEFS"),TEXT("SECTORS")})
    { if (!Lumps.Contains(Key)) { return Fail(); } }
    const auto Blocks=Lumps.FindChecked(TEXT("BLOCKMAP")), Vertices=Lumps.FindChecked(TEXT("VERTEXES")),
        Lines=Lumps.FindChecked(TEXT("LINEDEFS")), Sides=Lumps.FindChecked(TEXT("SIDEDEFS")), Sectors=Lumps.FindChecked(TEXT("SECTORS"));
    if (Blocks.Num()<8 || Blocks.Num()%2 || Vertices.Num()%4 || Lines.Num()%14 || Sides.Num()%30 || Sectors.Num()<26 || Sectors.Num()%26)
    { return Fail(); }
    const int32 Columns=U16(Blocks,4), Rows=U16(Blocks,6), Words=Blocks.Num()/2;
    if (Columns==0 || Rows==0 || int64(Columns)*Rows>Words-4) { return Fail(); }
    const int32 Column=(Position.X-int16(U16(Blocks,0)))>>7;
    int32 Row=(Position.Y-int16(U16(Blocks,2)))>>7;
    const auto Fallback=[&]() { Sector=0; bUsedSectorZeroFallback=true; return true; };
    if (Column<0 || Column>=Columns || Row<0 || Row>=Rows) { return Fallback(); }
    bool bInvalid=false;
    const auto OnSide=[&](FIntPoint Point,int32 Line)
    {
        if (Line<0 || Line>=Lines.Num()/14) { bInvalid=true; return 2; }
        const int32 From=U16(Lines,Line*14), To=U16(Lines,Line*14+2);
        if (From>=Vertices.Num()/4 || To>=Vertices.Num()/4) { bInvalid=true; return 2; }
        const int32 X1=int16(U16(Vertices,From*4)),Y1=int16(U16(Vertices,From*4+2)),
            X2=int16(U16(Vertices,To*4)),Y2=int16(U16(Vertices,To*4+2));
        const int64 Calc=int64(Y1-Y2)*(int64(Point.X)-X2)-int64(X1-X2)*(int64(Point.Y)-Y2);
        return Calc<0?0:Calc>0?1:2;
    };
    const auto Endpoint=[&](int32 Line,int32 End)
    {
        const int32 Index=U16(Lines,Line*14+End*2);
        if (Index>=Vertices.Num()/4) { bInvalid=true; return FIntPoint::ZeroValue; }
        return FIntPoint(int16(U16(Vertices,Index*4)),int16(U16(Vertices,Index*4+2)));
    };
    int32 Found=0xfffe,LastLine=0xffff,LastSide=0;
    TArray<FIntPoint> History;
    while (Found==0xfffe)
    {
        int32 At=1+U16(Blocks,(4+Row*Columns+Column)*2);
        if (At<4+Rows*Columns || At>=Words) { return Fail(); }
        bool bTerminated=false;
        for (;At<Words;++At)
        {
            const int32 Line=U16(Blocks,At*2);
            if (Line==0xffff) { bTerminated=true; break; }
            int32 Side=OnSide(Position,Line);
            if (bInvalid) { return Fail(); }
            if (Side==2) { Side=Found==0xfffe?0:1-LastSide; }
            const int32 Candidate=U16(Lines,Line*14+10+Side*2);
            if (Candidate!=0xffff && Candidate>=Sides.Num()/30) { return Fail(); }
            bool bNew=false;
            if (Candidate!=0xffff)
            {
                bNew=Found==0xfffe || OnSide(Endpoint(Line,0),LastLine)==LastSide || OnSide(Endpoint(Line,1),LastLine)==LastSide;
            }
            for (const FIntPoint& Old:History)
            { if (OnSide(Endpoint(Line,0),Old.X)!=Old.Y && OnSide(Endpoint(Line,1),Old.X)!=Old.Y) { bNew=false; break; } }
            if (bInvalid) { return Fail(); }
            if (bNew)
            {
                if (LastLine!=0xffff)
                { if (History.Num()>=20) { return Fail(); } History.Add(FIntPoint(LastLine,LastSide)); }
                Found=Candidate;LastLine=Line;LastSide=Side;
            }
        }
        if (!bTerminated) { return Fail(); }
        if (Found==0xfffe && ++Row>=Rows) { return Fallback(); }
    }
    if (Found==0xffff) { return Fallback(); }
    Sector=U16(Sides,Found*30+28);
    if (Sector>=Sectors.Num()/26) { return Fallback(); }
    return true;
}
