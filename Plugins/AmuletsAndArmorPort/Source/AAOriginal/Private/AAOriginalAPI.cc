#include "CoreMinimal.h"
#include "AAOriginalAPI.h"

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
