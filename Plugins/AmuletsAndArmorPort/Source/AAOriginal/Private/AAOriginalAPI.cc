#include "AAOriginalAPI.h"
#include "Misc/Crc.h"

// Forward declaration of exposed loader
bool AA_LoadDoomMap(const FString& WadPath, const FString& MapName);

uint32 AA_ComputeCRC32(const uint8* Data, int32 Length)
{
    if (Data == nullptr || Length <= 0)
    {
        return 0u;
    }
    return FCrc::MemCrc32(Data, Length);
}

const TCHAR* AA_GetVersion()
{
    return TEXT("0.0.1");
}
