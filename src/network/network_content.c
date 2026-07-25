#include "network/network.h"
#include "platform/system.h"

void NetworkContentRelease(void *bytes)
{
    PlatformFree(bytes);
}
