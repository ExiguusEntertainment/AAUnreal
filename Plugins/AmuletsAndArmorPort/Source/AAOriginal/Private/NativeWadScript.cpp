#include "NativeWadScript.h"
#include "NativeWadRules.h"

#if WITH_EDITOR
#include "Misc/FileHelper.h"

namespace
{
    class FScriptReader
    {
    public:
        explicit FScriptReader(TConstArrayView<uint8> InBytes) : Bytes(InBytes) {}

        bool Unsigned(int32 Width, uint32& Value)
        {
            if (Bytes.Num() - Position < Width)
            {
                return false;
            }
            Value = 0;
            for (int32 Index = 0; Index < Width; ++Index)
            {
                Value |= uint32(Bytes[Position++]) << (Index * 8);
            }
            return true;
        }

        bool ReadLiteral(uint32 Tag, int32& Value)
        {
            if (Tag > 2)
            {
                return false;
            }
            uint32 Raw = 0;
            if (!Unsigned(1 << Tag, Raw))
            {
                return false;
            }
            Value = Tag == 0 ? int8(Raw) : Tag == 1 ? int16(Raw) : int32(Raw);
            return true;
        }

        bool Literal(int32& Value)
        {
            uint32 Tag;
            return Unsigned(1, Tag) && ReadLiteral(Tag, Value);
        }

        bool Number(FWadScriptNumber& Value)
        {
            uint32 Tag;
            if (!Unsigned(1, Tag))
            {
                return false;
            }
            if (Tag != 4)
            {
                return ReadLiteral(Tag, Value.Value);
            }
            uint32 Index;
            if (!Unsigned(2, Index) || Index >= 256)
            {
                return false;
            }
            Value.Value = int32(Index);
            Value.bVariable = true;
            return true;
        }

        bool Literals(TArrayView<int32> Values)
        {
            for (int32& Value : Values)
            {
                if (!Literal(Value))
                {
                    return false;
                }
            }
            return true;
        }

        bool String(FString& Out)
        {
            uint32 Tag = 0;
            uint32 Count = 0;
            if (!Unsigned(1, Tag) || Tag != 3 || !Unsigned(1, Count) || Count > uint32(Bytes.Num() - Position))
            {
                return false;
            }
            Out.Reset(Count);
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                Out.AppendChar(TCHAR(Bytes[Position++]));
            }
            return true;
        }

        bool TaggedByte(int32 Tag, int32& Out)
        {
            uint32 Type = 0;
            uint32 Value = 0;
            if (!Unsigned(1, Type) || Type != uint32(Tag) || !Unsigned(Tag == 4 ? 2 : 1, Value) || Value >= 256)
            {
                return false;
            }
            Out = int32(Value);
            return true;
        }

    private:
        TConstArrayView<uint8> Bytes;
        int32 Position = 0;
    };

    bool ReadCommands(FScriptReader& Reader, int32 SectorCount, int32 SideCount, int32 NextActivity, FWadScriptActivity& Activity,
        FOutputDevice& Output)
    {
        uint32 Opcode = 0;
        while (Reader.Unsigned(1, Opcode))
        {
            if (Opcode == 0)
            {
                return true;
            }
            FWadScriptCommand Command;
            switch (Opcode)
            {
            case 60:
            case 61:
                Command.Operation = Opcode == 60 ? EWadScriptOperation::Block : EWadScriptOperation::Unblock;
                if (!Reader.TaggedByte(4, Command.Target))
                {
                    return false;
                }
                break;
            case 1:
            case 11:
                Command.Operation = Opcode == 1 ? EWadScriptOperation::SetVariable : EWadScriptOperation::CompareVariable;
                if (!Reader.TaggedByte(4, Command.Target) || !Reader.Literal(Command.Value))
                {
                    return false;
                }
                break;
            case 9:
                Command.Operation = EWadScriptOperation::IncrementVariable;
                if (!Reader.TaggedByte(4, Command.Target))
                {
                    return false;
                }
                break;
            case 5:
            case 6:
            case 7:
                Command.Operation = Opcode == 5 ? EWadScriptOperation::AddNumbers
                    : Opcode == 6 ? EWadScriptOperation::SubtractNumbers : EWadScriptOperation::MultiplyNumbers;
                if (!Reader.TaggedByte(4, Command.Target) || !Reader.Number(Command.Left) || !Reader.Number(Command.Right))
                {
                    return false;
                }
                break;
            case 27:
            case 28:
            {
                int32 Attribute;
                // OBJECT_SCRIPT_ATTR_ANGLE is the only object attribute used by the L60 statues.
                if (!Reader.Literal(Command.Target) || Command.Target <= 0 || !Reader.Literal(Attribute) || Attribute != 9)
                {
                    return false;
                }
                if (Opcode == 27)
                {
                    Command.Operation = EWadScriptOperation::SetObjectAngle;
                    if (!Reader.Number(Command.Left))
                    {
                        return false;
                    }
                }
                else
                {
                    Command.Operation = EWadScriptOperation::ReadObjectAngle;
                    if (!Reader.TaggedByte(4, Command.Value))
                    {
                        return false;
                    }
                }
                break;
            }
            case 57:
            case 58:
                Command.Operation = Opcode == 57 ? EWadScriptOperation::ActivateGenerator : EWadScriptOperation::DeactivateGenerator;
                if (!Reader.Literal(Command.Target) || Command.Target < 0)
                {
                    return false;
                }
                break;
            case 3:
                Command.Operation = EWadScriptOperation::IfFlag;
                if (!Reader.TaggedByte(6, Command.Value) || Command.Value > 7 || !Reader.Literal(Command.Target))
                {
                    return false;
                }
                break;
            case 4:
            case 24:
                Command.Operation = Opcode == 4 ? EWadScriptOperation::Jump : EWadScriptOperation::Call;
                if (!Reader.Literal(Command.Target))
                {
                    return false;
                }
                break;
            case 13:
                if (!Reader.Literal(Command.Target) || Command.Target < 0 || Command.Target >= SideCount
                    || !Reader.String(Command.Text) || Command.Text.IsEmpty() || Command.Text.Len() > 8)
                {
                    return false;
                }
                Command.Operation = (Command.Text == TEXT("SWIT1U") || Command.Text == TEXT("SWIT1D"))
                    ? EWadScriptOperation::SetSwitch : EWadScriptOperation::ChangeSideTexture;
                Command.Value = Command.Text == TEXT("SWIT1U") ? 1 : 0;
                break;
            case 65:
                Command.Operation = EWadScriptOperation::AddJournalPage;
                if (!Reader.Literal(Command.Target) || Command.Target < 0 || Command.Target > 9999)
                {
                    return false;
                }
                break;
            case 62:
                Command.Operation = EWadScriptOperation::ToggleWallSwitch;
                if (!Reader.TaggedByte(4, Command.Value) || !Reader.Literal(Command.Target)
                    || Command.Target < 0 || Command.Target >= SideCount
                    || !Reader.String(Command.Text) || Command.Text.IsEmpty() || Command.Text.Len() > 8
                    || !Reader.String(Command.SwitchDownTexture) || Command.SwitchDownTexture.IsEmpty() || Command.SwitchDownTexture.Len() > 8
                    || !Reader.Literal(Command.SwitchUpActivity) || Command.SwitchUpActivity < INDEX_NONE
                    || !Reader.Literal(Command.SwitchDownActivity) || Command.SwitchDownActivity < INDEX_NONE)
                {
                    return false;
                }
                break;
            case 21:
                Command.Operation = EWadScriptOperation::Wait;
                if (!Reader.Literal(Command.Ticks) || Command.Ticks <= 0)
                {
                    return false;
                }
                break;
            case 2:
                Command.Operation = EWadScriptOperation::Message;
                if (!Reader.String(Command.Text))
                {
                    return false;
                }
                break;
            case 15:
            {
                int32 Args[3];
                if (!Reader.Literals(Args))
                {
                    return false;
                }
                Command.Operation = EWadScriptOperation::TeleportPlayer;
                Command.Position = FIntVector(Args[0], Args[1], Args[2]);
                break;
            }
            case 22:
            case 23:
            case 63:
            case 64:
            {
                int32 Args[5];
                if (!Reader.Literals(Args) || Args[0] < 0 || Args[0] >= SectorCount || Args[3] <= 0 || Args[4] < INDEX_NONE)
                {
                    return false;
                }
                // Nice sliders use the current height as their start in the source engine.
                Command.Operation = Opcode == 22 || Opcode == 63 ? EWadScriptOperation::SlideFloor : EWadScriptOperation::SlideCeiling;
                Command.bPauseOnCrush = Opcode == 63 || Opcode == 64;
                Command.Target = Args[0];
                Command.Value = Args[2];
                Command.Ticks = Args[3];
                Command.CompletionActivity = Args[4];
                break;
            }
            case 12:
            case 19:
            case 52:
            case 54:
            {
                int32 Args[8];
                const int32 ArgCount = Opcode == 54 ? 8 : Opcode == 12 ? 1 : Opcode == 19 ? 5 : 4;
                if (!Reader.Literals(MakeArrayView(Args, ArgCount)))
                {
                    return false;
                }
                Command.Target = Args[0];
                if (Opcode == 12)
                {
                    Command.Operation = EWadScriptOperation::PlaySound;
                    Command.Value = 255;
                }
                else if (Opcode == 19)
                {
                    Command.Operation = EWadScriptOperation::PlayAreaSound;
                    Command.Position = FIntVector(Args[1], Args[2], 0);
                    Command.Ticks = Args[3];
                    Command.Value = Args[4];
                }
                else if (Opcode == 52 && Args[0] == 29 && Args[1] > 0)
                {
                    Command.Operation = EWadScriptOperation::Earthquake;
                    Command.Ticks = Args[1];
                }
                else if (Opcode == 52 && Args[0] >= 7 && Args[0] <= 11)
                {
                    Command.Operation = EWadScriptOperation::RestoreVitals;
                    Command.Value = Args[3];
                }
                else if (Opcode == 54 && (Args[0] == NativeWadRules::AcidBall
                    || NativeWadRules::FindHomingMissile(Args[0])
                    || Args[0] == 4005 || Args[0] == 4006 || Args[0] == 4011 || Args[0] == 4012
                    || Args[0] == 4015 || Args[0] == 4016
                    || Args[0] == NativeWadRules::PoisonCrossbowBolt
                    || Args[0] == NativeWadRules::PiercingCrossbowBolt) && Args[7] > 0)
                {
                    Command.Operation = EWadScriptOperation::LaunchMissile;
                    Command.Position = FIntVector(Args[1], Args[2], Args[3]);
                    Command.Aim = FIntVector(Args[4], Args[5], Args[6]);
                    Command.Value = Args[7];
                }
                else
                {
                    Output.Logf(TEXT("WAD script activity %d defers opcode %u (%d)."), Activity.Id, Opcode, Args[0]);
                    continue;
                }
                break;
            }
            default:
                Output.Logf(TEXT("Unsupported WAD script opcode %u in activity %d."), Opcode, Activity.Id);
                return false;
            }
            Activity.Commands.Add(MoveTemp(Command));
        }
        if (NextActivity == INDEX_NONE)
        {
            return false;
        }
        // Place table entries are labels, not return instructions. Source execution can enter the next label.
        FWadScriptCommand& Fallthrough = Activity.Commands.AddDefaulted_GetRef();
        Fallthrough.Operation = EWadScriptOperation::Jump;
        Fallthrough.Target = NextActivity;
        return true;
    }
}

bool ImportWadSounds(const FString& Filename, const TArray<FWadScriptActivity>& Activities,
    TMap<int32, FWadSoundSample>& Sounds, FOutputDevice& Output, TConstArrayView<int32> UnavailableSounds)
{
    TSet<int32> Required;
    for (const FWadScriptActivity& Activity : Activities)
    {
        for (const FWadScriptCommand& Command : Activity.Commands)
        {
            if (Command.Operation == EWadScriptOperation::PlaySound || Command.Operation == EWadScriptOperation::PlayAreaSound)
            {
                Required.Add(Command.Target);
            }
        }
    }
    return ImportWadSoundSamples(Filename, MoveTemp(Required), Sounds, Output, UnavailableSounds);
}

bool ImportWadSoundSamples(const FString& Filename, TSet<int32> Required,
    TMap<int32, FWadSoundSample>& Sounds, FOutputDevice& Output, TConstArrayView<int32> UnavailableSounds)
{
    Sounds.Reset();
    if (Required.IsEmpty())
    {
        return true;
    }

    TArray<uint8> Bytes;
    constexpr int32 EntrySize = 39;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.Num() < 14 || FMemory::Memcmp(Bytes.GetData(), "Res!", 4) != 0)
    {
        return false;
    }
    const auto Read = [&Bytes](int32 Offset, int32 Width)
    {
        uint32 Number = 0;
        for (int32 Index = 0; Index < Width; ++Index)
        {
            Number |= uint32(Bytes[Offset + Index]) << (Index * 8);
        }
        return Number;
    };
    const uint32 Start = Read(4, 4), Size = Read(8, 4), Entries = Read(12, 2);
    if (Size != Entries * EntrySize || uint64(Start) + Size > uint64(Bytes.Num()))
    {
        return false;
    }
    for (uint32 Index = 0; Index < Entries; ++Index)
    {
        const int32 Entry = int32(Start + EntrySize * Index);
        ANSICHAR Name[15] = {};
        FMemory::Memcpy(Name, Bytes.GetData() + Entry + 4, 14);
        const FString Key(ANSI_TO_TCHAR(Name));
        int32 Id;
        if (!Key.StartsWith(TEXT("snd#")) || !LexTryParseString(Id, *Key.Mid(4)) || !Required.Contains(Id))
        {
            continue;
        }
        const uint32 Offset = Read(Entry + 18, 4), Length = Read(Entry + 22, 4);
        if (Length == 0 || uint64(Offset) + Length > Start)
        {
            return false;
        }
        Sounds.FindOrAdd(Id).UnsignedSamples.Append(Bytes.GetData() + Offset, int32(Length));
        Required.Remove(Id);
    }
    Output.Logf(TEXT("Imported %d authored sound samples from %s."), Sounds.Num(), *Filename);
    for (int32 Id : UnavailableSounds)
    {
        if (Required.Remove(Id))
        {
            Output.Logf(TEXT("The supplied sound archive lacks authored script sample %d."), Id);
        }
    }
    return Required.IsEmpty();
}

bool ImportWadScriptActivities(const FString& Filename, TConstArrayView<int32> ActivityIds, int32 SectorCount,
    int32 SideCount, TArray<FWadScriptActivity>& Out, FOutputDevice& Output)
{
    constexpr int32 HeaderBytes = 64; // The original disk header uses 32-bit pointers.
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.Num() < HeaderBytes)
    {
        return false;
    }
    FScriptReader Header(Bytes);
    uint32 Events = 0;
    uint32 Places = 0;
    uint32 CodeSize = 0;
    if (!Header.Unsigned(2, Events) || !Header.Unsigned(2, Places) || !Header.Unsigned(4, CodeSize))
    {
        return false;
    }
    const uint64 PlaceTable = HeaderBytes + uint64(CodeSize) + Events * 2;
    if (PlaceTable + Places * 2 > uint64(Bytes.Num()))
    {
        return false;
    }
    FScriptReader Table(MakeArrayView(Bytes).Slice(int32(PlaceTable), int32(Places * 2)));
    TArray<uint32> Offsets;
    Offsets.Reserve(Places);
    for (uint32 Index = 0; Index < Places; ++Index)
    {
        uint32 Offset = 0;
        if (!Table.Unsigned(2, Offset))
        {
            return false;
        }
        Offsets.Add(Offset);
    }
    TArray<int32> Queue;
    Queue.Append(ActivityIds.GetData(), ActivityIds.Num());
    TArray<FWadScriptActivity> Pending;
    for (int32 Next = 0; Next < Queue.Num(); ++Next)
    {
        const int32 Id = Queue[Next];
        if (!Offsets.IsValidIndex(Id))
        {
            return false;
        }
        const uint32 Start = Offsets[Id];
        if (Start == 0xffff || Start >= CodeSize)
        {
            return false;
        }
        uint32 End = CodeSize;
        int32 NextActivity = INDEX_NONE;
        for (int32 Index = 0; Index < Offsets.Num(); ++Index)
        {
            const uint32 Offset = Offsets[Index];
            if (Offset != 0xffff && Offset > Start && Offset < End)
            {
                End = Offset;
                NextActivity = Index;
            }
        }
        FScriptReader Code(MakeArrayView(Bytes).Slice(int32(HeaderBytes + Start), int32(End - Start)));
        FWadScriptActivity& Activity = Pending.AddDefaulted_GetRef();
        Activity.Id = Id;
        if (!ReadCommands(Code, SectorCount, SideCount, NextActivity, Activity, Output))
        {
            Output.Logf(TEXT("Failed to decode WAD script activity %d."), Id);
            return false;
        }
        for (const FWadScriptCommand& Command : Activity.Commands)
        {
            if (Command.CompletionActivity != INDEX_NONE)
            {
                Queue.AddUnique(Command.CompletionActivity);
            }
            if (Command.SwitchUpActivity != INDEX_NONE)
            {
                Queue.AddUnique(Command.SwitchUpActivity);
            }
            if (Command.SwitchDownActivity != INDEX_NONE)
            {
                Queue.AddUnique(Command.SwitchDownActivity);
            }
            if (Command.Operation == EWadScriptOperation::Call || Command.Operation == EWadScriptOperation::Jump
                || Command.Operation == EWadScriptOperation::IfFlag)
            {
                Queue.AddUnique(Command.Target);
            }
        }
        Output.Logf(TEXT("Imported WAD script activity %d with %d native commands."), Id, Activity.Commands.Num());
    }
    Out = MoveTemp(Pending);
    return true;
}
#endif
