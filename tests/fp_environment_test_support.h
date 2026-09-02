#ifndef LAIUE_TESTS_FP_ENVIRONMENT_TEST_SUPPORT_H
#define LAIUE_TESTS_FP_ENVIRONMENT_TEST_SUPPORT_H

#include <stdint.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>

static void LaiueTestSetHostileFpEnvironment(void)
{
    uint32_t value = (uint32_t)_mm_getcsr();
    value |= 0xe040u;
    _mm_setcsr(value);
}
#elif defined(__aarch64__)
/* Mirrors fp_environment.c: only MSVC itself provides the ARM64_FPCR selector,
 * so every compiler with inline assembly must be handled before _M_ARM64. */
static void LaiueTestSetHostileFpEnvironment(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
    value |= 0x07c00000ULL;
    __asm__ volatile("msr fpcr, %0" : : "r"(value) : "memory");
}
#elif defined(_M_ARM64)
#include <arm64intr.h>
#include <intrin.h>

static void LaiueTestSetHostileFpEnvironment(void)
{
    uint64_t value = (uint64_t)_ReadStatusReg(ARM64_FPCR);
    value |= 0x07c00000ULL;
    _WriteStatusReg(ARM64_FPCR, (__int64)value);
}
#else
#error "FP environment test requires x86_64 or ARM64"
#endif

#endif
