#include "NativeWadBagLayout.h"

namespace
{
    bool Number(const FString& Token,int32& Value)
    {
        if(Token.IsEmpty()||Token.Len()>11){return false;}
        int32 Start=Token[0]==TEXT('-')?1:0;if(Start==Token.Len()){return false;}
        for(int32 I=Start;I<Token.Len();++I){if(Token[I]<TEXT('0')||Token[I]>TEXT('9')){return false;}}
        return LexTryParseString(Value,*Token)&&Token==LexToString(Value);
    }
    bool Intersects(const FWadBagPlacement& P,FIntPoint Size,FIntPoint A,FIntPoint B)
    {return A.X<P.X+Size.X&&B.X>P.X&&A.Y<P.Y+Size.Y&&B.Y>P.Y;}
}
FIntPoint FWadBagLayout::Footprint(FIntPoint Bitmap)
{
    // INVENTOR.C1035: spacing is17x14, but bitmap footprint divisors are18x15.
    return {int32((int64(FMath::Max(1,Bitmap.X))+17)/18),int32((int64(FMath::Max(1,Bitmap.Y))+14)/15)};
}
FIntPoint FWadBagLayout::BitmapOffset(FIntPoint Bitmap)
{
    const FIntPoint F=Footprint(Bitmap);
    return {1+(F.X*CellWidth-Bitmap.X)/2,1+(F.Y*CellHeight-Bitmap.Y)/2};
}
FIntPoint FWadBagLayout::Anchor(FVector2D Point,FVector2D Origin,FIntPoint Bitmap)
{
    // Signed arithmetic preserves ordinary C integer division while refusing the
    // original unsigned-wrap/off-screen placement bug at negative GRID anchors.
    return {(int32(Point.X)-Bitmap.X/2-int32(Origin.X))/CellWidth,
        (int32(Point.Y)-Bitmap.Y/2-int32(Origin.Y))/CellHeight};
}
int32 FWadBagLayout::LeaderOf(int32 Slot) const {return Entries.IsValidIndex(Slot)?Entries[Slot].Leader:INDEX_NONE;}
int32 FWadBagLayout::StackCount(int32 Leader) const
{
    int32 N=0;if(!Entries.IsValidIndex(Leader)||Entries[Leader].Leader!=Leader){return 0;}
    for(const auto& P:Entries){N+=P.Leader==Leader;}return N;
}
int32 FWadBagLayout::At(int32 Page,int32 X,int32 Y,TConstArrayView<FWadBagItem> Items) const
{
    if(Page<0||Page>=Pages||X<0||X>=Columns||Y<0||Y>=Rows||Items.Num()!=Entries.Num()){return INDEX_NONE;}
    for(int32 I=0;I<Entries.Num();++I)
    {const auto& P=Entries[I];if(P.Leader==I&&P.Page==Page&&Intersects(P,Footprint(Items[I].Bitmap),{X,Y},{X+1,Y+1})){return I;}}
    return INDEX_NONE;
}
bool FWadBagLayout::Place(int32 Slot,int32 Page,FIntPoint A,TConstArrayView<FWadBagItem> Items)
{
    if(Items.Num()!=Entries.Num()||!Entries.IsValidIndex(Slot)||Items[Slot].bOffPage||Entries[Slot].Leader!=INDEX_NONE
        ||Page<0||Page>=Pages){return false;}
    const FIntPoint F=Footprint(Items[Slot].Bitmap),B=A+F;
    if(A.X<0||A.Y<0||B.X>Columns||B.Y>Rows){return false;}
    int32 Overlap=INDEX_NONE;
    for(int32 I=0;I<Entries.Num();++I)
    {
        const auto& P=Entries[I];
        if(P.Leader==I&&P.Page==Page&&Intersects(P,Footprint(Items[I].Bitmap),A,B)){if(Overlap==INDEX_NONE||P.Order<Entries[Overlap].Order){Overlap=I;}}
    }
    if(Overlap!=INDEX_NONE)
    {
        if(Items[Overlap].Type!=Items[Slot].Type||StackCount(Overlap)>=FMath::Max(1,Items[Overlap].StackLimit)){return false;}
        Entries[Slot]={INDEX_NONE,INDEX_NONE,INDEX_NONE,Overlap,INDEX_NONE};return true;
    }
    Entries[Slot]={Page,A.X,A.Y,Slot,Entries[Slot].Order};return true;
}
bool FWadBagLayout::AutoPlace(int32 Slot,TConstArrayView<FWadBagItem> Items,bool bAllowOverflow,bool bRowMajor)
{
    if(Items.Num()!=Entries.Num()||!Entries.IsValidIndex(Slot)||Items[Slot].bOffPage){return false;}
    if(Entries[Slot].Leader!=INDEX_NONE){return true;}
    // Deliberately bag-only: source automatic refill of readied consumable stacks
    // remains a separate Ready ownership/use milestone.
    int32 Match=INDEX_NONE;
    for(int32 I=0;I<Entries.Num();++I)
    {
        const auto& P=Entries[I];
        if(P.Leader==I&&Items[I].Type==Items[Slot].Type&&StackCount(I)<FMath::Max(1,Items[I].StackLimit)
            &&(Match==INDEX_NONE||P.Order<Entries[Match].Order)){Match=I;}
    }
    if(Match!=INDEX_NONE){Entries[Slot]={INDEX_NONE,INDEX_NONE,INDEX_NONE,Match,INDEX_NONE};return true;}
    const FIntPoint F=Footprint(Items[Slot].Bitmap);
    if(F.X>Columns||F.Y>Rows){return false;}
    const int32 Limit=bAllowOverflow?MaxPages:Pages;
    for(int32 Page=0;Page<Limit;++Page)
    {
        if(Page>=Pages){Pages=Page+1;bLegacyOverflow=true;}
        // Auto-store source order: page, descending X, descending Y. Creation
        // reorder separately scans row-major and keeps original linked order.
        for(int32 Major=0;Major<(bRowMajor?Rows-F.Y+1:Columns-F.X+1);++Major)
        for(int32 Minor=0;Minor<(bRowMajor?Columns-F.X+1:Rows-F.Y+1);++Minor)
        {
            const FIntPoint A=bRowMajor?FIntPoint(Minor,Major):FIntPoint(Columns-F.X-Major,Rows-F.Y-Minor);
            bool bFree=true;
            for(int32 I=0;I<Entries.Num();++I)
            {const auto& P=Entries[I];if(P.Leader==I&&P.Page==Page&&Intersects(P,Footprint(Items[I].Bitmap),A,A+F)){bFree=false;break;}}
            if(bFree){Entries[Slot]={Page,A.X,A.Y,Slot,Entries[Slot].Order};return true;}
        }
    }
    return false;
}
void FWadBagLayout::TrimOverflow()
{
    if(!bLegacyOverflow){return;}int32 Required=SourcePages;
    for(int32 I=0;I<Entries.Num();++I){if(Entries[I].Leader==I){Required=FMath::Max(Required,Entries[I].Page+1);}}
    Pages=Required;bLegacyOverflow=Pages>SourcePages;
}
void FWadBagLayout::Detach(int32 Slot)
{
    if(!Entries.IsValidIndex(Slot)){return;}const auto Old=Entries[Slot];
    if(Old.Leader==INDEX_NONE){return;}
    bool bSplit=Old.Leader!=Slot;
    if(Old.Leader==Slot)
    {
        int32 Next=INDEX_NONE;
        for(int32 I=0;I<Entries.Num();++I){if(I!=Slot&&Entries[I].Leader==Slot){Next=I;break;}}
        if(Next!=INDEX_NONE)
        {
            for(auto& P:Entries){if(P.Leader==Slot){P.Leader=Next;}}
            Entries[Next]={Old.Page,Old.X,Old.Y,Next,Old.Order};
            bSplit=true;
        }
    }
    Entries[Slot]={INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,bSplit?AllocateOrder():Old.Order};TrimOverflow();
}
void FWadBagLayout::RemoveAt(int32 Slot)
{
    if(!Entries.IsValidIndex(Slot)){return;}Detach(Slot);Entries.RemoveAt(Slot);
    for(auto& P:Entries){if(P.Leader>Slot){--P.Leader;}}TrimOverflow();
}
int32 FWadBagLayout::AllocateOrder()
{
    if(NextOrder==MAX_int32)
    {
        // Compact only numeric order labels before overflow, preserving traversal.
        TArray<int32> Records;
        for(int32 I=0;I<Entries.Num();++I){if(Entries[I].Order>=0){Records.Add(I);}}
        Records.Sort([&](int32 A,int32 B){return Entries[A].Order<Entries[B].Order;});
        int32 Order=0;for(int32 I:Records){Entries[I].Order=Order++;}NextOrder=Order;
    }
    return NextOrder++;
}
TArray<int32> FWadBagLayout::LeadersInOrder() const
{
    TArray<int32> Result;
    for(int32 I=0;I<Entries.Num();++I){if(Entries[I].Leader==I){Result.Add(I);}}
    Result.Sort([&](int32 A,int32 B){return Entries[A].Order<Entries[B].Order;});return Result;
}
bool FWadBagLayout::AddSlot()
{if(Entries.Num()>=MaxSlots){return false;}const int32 Order=AllocateOrder();Entries.Add({INDEX_NONE,INDEX_NONE,INDEX_NONE,INDEX_NONE,Order});return true;}
bool FWadBagLayout::Initialize(TConstArrayView<FWadBagItem> Items,bool bAllowLegacyOverflow,bool bRowMajor)
{
    if(Items.Num()>MaxSlots){return false;}FWadBagLayout Fresh;
    for(int32 I=0;I<Items.Num();++I){Fresh.AddSlot();}
    for(int32 I=0;I<Items.Num();++I)
    {if(!Items[I].bOffPage&&!Fresh.AutoPlace(I,Items,bAllowLegacyOverflow,bRowMajor)){return false;}}
    *this=MoveTemp(Fresh);return true;
}
bool FWadBagLayout::Validate(TConstArrayView<FWadBagItem> Items) const
{
    if(Items.Num()!=Entries.Num()||Items.Num()>MaxSlots||Pages<SourcePages||Pages>MaxPages||bLegacyOverflow!=(Pages>SourcePages)){return false;}
    if(NextOrder<0){return false;}
    TSet<int32> Orders;
    int32 Highest=SourcePages;
    TArray<int32> Counts;Counts.Init(0,Entries.Num());
    TArray<int32> Occupied;Occupied.Init(INDEX_NONE,Pages*Columns*Rows);
    for(const auto& P:Entries){if(P.Leader!=INDEX_NONE){if(!Entries.IsValidIndex(P.Leader)){return false;}++Counts[P.Leader];}}
    for(int32 I=0;I<Entries.Num();++I)
    {
        const auto& P=Entries[I];
        if(Items[I].Type<=0||Items[I].Type>MAX_uint16||Items[I].Bitmap.X<=0||Items[I].Bitmap.Y<=0){return false;}
        if(P.Leader==INDEX_NONE||P.Leader==I)
        {if(P.Order<0||P.Order>=NextOrder||Orders.Contains(P.Order)){return false;}Orders.Add(P.Order);}
        else if(P.Order!=INDEX_NONE){return false;}
        if(P.Leader==INDEX_NONE)
        {if(!Items[I].bOffPage||P.Page!=INDEX_NONE||P.X!=INDEX_NONE||P.Y!=INDEX_NONE){return false;}continue;}
        if(Items[I].bOffPage||!Entries.IsValidIndex(P.Leader)||Entries[P.Leader].Leader!=P.Leader||Items[I].Type!=Items[P.Leader].Type){return false;}
        if(P.Leader!=I){if(P.Page!=INDEX_NONE||P.X!=INDEX_NONE||P.Y!=INDEX_NONE){return false;}continue;}
        const FIntPoint F=Footprint(Items[I].Bitmap);
        if(P.Page<0||P.Page>=Pages||P.X<0||P.Y<0||P.X+F.X>Columns||P.Y+F.Y>Rows||Counts[I]>FMath::Max(1,Items[I].StackLimit)){return false;}
        Highest=FMath::Max(Highest,P.Page+1);
        for(int32 Y=P.Y;Y<P.Y+F.Y;++Y)for(int32 X=P.X;X<P.X+F.X;++X)
        {int32& Cell=Occupied[P.Page*Columns*Rows+Y*Columns+X];if(Cell!=INDEX_NONE){return false;}Cell=I;}
    }
    return Pages==Highest;
}
FString FWadBagLayout::Encode(TConstArrayView<FWadBagItem> Items) const
{
    if(!Validate(Items)){return FString();}FString Result=FString::Printf(TEXT("1,%d,%d,%d"),Pages,bLegacyOverflow?1:0,NextOrder);
    for(int32 I=0;I<Entries.Num();++I)
    {const auto& P=Entries[I];Result+=FString::Printf(TEXT("~%d,%d,%d,%d,%d,%d"),Items[I].Type,P.Page,P.X,P.Y,P.Leader,P.Order);}
    return Result;
}
bool FWadBagLayout::Decode(const FString& Payload,TConstArrayView<FWadBagItem> Items)
{
    if(Payload.Len()>4*1024*1024||Items.Num()>MaxSlots){return false;}TArray<FString> Records;
    Payload.ParseIntoArray(Records,TEXT("~"),false);if(Records.Num()!=Items.Num()+1){return false;}
    TArray<FString> Header;Records[0].ParseIntoArray(Header,TEXT(","),false);int32 Version,Legacy;
    FWadBagLayout Parsed;
    if(Header.Num()!=4||!Number(Header[0],Version)||Version!=1||!Number(Header[1],Parsed.Pages)
        ||!Number(Header[2],Legacy)||(Legacy!=0&&Legacy!=1)||!Number(Header[3],Parsed.NextOrder)){return false;}Parsed.bLegacyOverflow=Legacy!=0;
    Parsed.Entries.SetNum(Items.Num());
    for(int32 I=0;I<Items.Num();++I)
    {
        TArray<FString> Fields;Records[I+1].ParseIntoArray(Fields,TEXT(","),false);int32 Type;
        auto& P=Parsed.Entries[I];
        if(Fields.Num()!=6||!Number(Fields[0],Type)||Type!=Items[I].Type||!Number(Fields[1],P.Page)
            ||!Number(Fields[2],P.X)||!Number(Fields[3],P.Y)||!Number(Fields[4],P.Leader)||!Number(Fields[5],P.Order)){return false;}
    }
    if(!Parsed.Validate(Items)){return false;}*this=MoveTemp(Parsed);return true;
}
