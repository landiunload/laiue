#include "physics/fp_environment.h"

#include <stdint.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>

#define PHYSICS_MXCSR_DAZ 0x0040u
#define PHYSICS_MXCSR_EXCEPTION_MASKS 0x1f80u
#define PHYSICS_MXCSR_ROUNDING 0x6000u
#define PHYSICS_MXCSR_FTZ 0x8000u
#define PHYSICS_MXCSR_CONTROL_MASK                                                                 \
    (PHYSICS_MXCSR_DAZ | PHYSICS_MXCSR_EXCEPTION_MASKS | PHYSICS_MXCSR_ROUNDING | PHYSICS_MXCSR_FTZ)

static uint64_t PhysicsFpControlRead(void)
{
    return (uint64_t)_mm_getcsr();
}

static void PhysicsFpControlWrite(uint64_t value)
{
    _mm_setcsr((uint32_t)value);
}

static uint64_t PhysicsFpControlNormalize(uint64_t value)
{
    return (value & ~(uint64_t)PHYSICS_MXCSR_CONTROL_MASK) | PHYSICS_MXCSR_EXCEPTION_MASKS;
}
#elif defined(__aarch64__)
/* GCC and Clang reach the register directly, on every ARM64 operating system.
 * Their own <arm64intr.h> does not declare the MSVC ARM64_FPCR selector, so this
 * branch must precede the _M_ARM64 one that clang-cl would otherwise enter. */
#define PHYSICS_FPCR_CONTROL_MASK 0x07ffff07ULL

static uint64_t PhysicsFpControlRead(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
    return value;
}

static void PhysicsFpControlWrite(uint64_t value)
{
    __asm__ volatile("msr fpcr, %0" : : "r"(value) : "memory");
}

static uint64_t PhysicsFpControlNormalize(uint64_t value)
{
    return value & ~PHYSICS_FPCR_CONTROL_MASK;
}
#elif defined(_M_ARM64)
/* MSVC has no inline assembler on ARM64 and offers the register intrinsics. */
#include <arm64intr.h>
#include <intrin.h>

#define PHYSICS_FPCR_CONTROL_MASK 0x07ffff07ULL

static uint64_t PhysicsFpControlRead(void)
{
    return (uint64_t)_ReadStatusReg(ARM64_FPCR);
}

static void PhysicsFpControlWrite(uint64_t value)
{
    _WriteStatusReg(ARM64_FPCR, (__int64)value);
}

static uint64_t PhysicsFpControlNormalize(uint64_t value)
{
    return value & ~PHYSICS_FPCR_CONTROL_MASK;
}
#else
#error "Deterministic physics requires an x86_64 or ARM64 FP environment"
#endif

bool PhysicsFpEnvironmentIsDeterministic(void)
{
    uint64_t current = PhysicsFpControlRead();
    return current == PhysicsFpControlNormalize(current);
}

void PhysicsFpEnvironmentNormalize(void)
{
    uint64_t current = PhysicsFpControlRead();
    uint64_t normalized = PhysicsFpControlNormalize(current);
    if (current != normalized)
    {
        PhysicsFpControlWrite(normalized);
    }
}
