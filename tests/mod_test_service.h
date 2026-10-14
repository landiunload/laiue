#pragma once

#include <stdint.h>

typedef struct LaiueModTestCounterService
{
    uint32_t loadCount;
    uint32_t unloadCount;
} LaiueModTestCounterService;

#define LAIUE_MOD_TEST_COUNTER_SERVICE_NAME "laiue.test.counter"
#define LAIUE_MOD_TEST_COUNTER_SERVICE_VERSION 2u
