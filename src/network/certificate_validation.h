#pragma once

#include "network/network.h"

#include <stdbool.h>
#include <stdint.h>

bool NetworkCertificateValidateLeafIdentity(
    const NetworkEndpoint *endpoint,
    const uint8_t *der, uint32_t derSize);
