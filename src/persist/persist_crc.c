#include "persist/persist_crc.h"

#include "platform/system.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__)
#define LAIUE_PERSIST_CRC_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <nmmintrin.h>
#endif

#define CRC32C_POLYNOMIAL UINT32_C(0x82F63B78)

/* Таблицы slicing-by-8 строятся из полинома один раз. Выводятся формулой,
 * поэтому в исходник не переписываются. */
static uint32_t crcTables[8][256];
static volatile uint32_t crcState; /* 0 — не начато, 1 — строится, 2 — готово. */
#if defined(LAIUE_PERSIST_CRC_X86)
static bool crcHardware;
#endif

#if defined(LAIUE_PERSIST_CRC_X86)
static bool CpuHasSse42(void)
{
#if defined(_MSC_VER)
    int registers[4];
    __cpuid(registers, 1);
    return (((uint32_t)registers[2] >> 20u) & 1u) != 0u;
#else
    uint32_t eax = 1u, ebx, ecx = 0u, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    (void)ebx;
    (void)edx;
    return ((ecx >> 20u) & 1u) != 0u;
#endif
}
#endif

static void BuildTables(void)
{
    for (uint32_t index = 0u; index < 256u; ++index)
    {
        uint32_t crc = index;
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (CRC32C_POLYNOMIAL & (0u - (crc & 1u)));
        crcTables[0][index] = crc;
    }
    for (uint32_t index = 0u; index < 256u; ++index)
        for (uint32_t slice = 1u; slice < 8u; ++slice)
        {
            const uint32_t previous = crcTables[slice - 1u][index];
            crcTables[slice][index] = (previous >> 8u) ^ crcTables[0][previous & 0xffu];
        }
#if defined(LAIUE_PERSIST_CRC_X86)
    crcHardware = CpuHasSse42();
#endif
}

static void EnsureTables(void)
{
    if (PlatformAtomicLoadU32Acquire(&crcState) == 2u)
        return;
    uint32_t expected = 0u;
    if (PlatformAtomicCompareExchangeU32(&crcState, &expected, 1u))
    {
        BuildTables();
        PlatformAtomicStoreU32Release(&crcState, 2u);
        return;
    }
    while (PlatformAtomicLoadU32Acquire(&crcState) != 2u)
        PlatformCpuRelax();
}

static uint32_t SoftwareUpdate(uint32_t crc, const uint8_t *bytes, uint64_t size)
{
    while (size != 0u && ((uintptr_t)bytes & 7u) != 0u)
    {
        crc = (crc >> 8u) ^ crcTables[0][(crc ^ *bytes++) & 0xffu];
        --size;
    }
    while (size >= 8u)
    {
        uint32_t low;
        uint32_t high;
        memcpy(&low, bytes, 4u);
        memcpy(&high, bytes + 4u, 4u);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        low = __builtin_bswap32(low);
        high = __builtin_bswap32(high);
#endif
        low ^= crc;
        crc = crcTables[7][low & 0xffu] ^ crcTables[6][(low >> 8u) & 0xffu] ^
              crcTables[5][(low >> 16u) & 0xffu] ^ crcTables[4][low >> 24u] ^
              crcTables[3][high & 0xffu] ^ crcTables[2][(high >> 8u) & 0xffu] ^
              crcTables[1][(high >> 16u) & 0xffu] ^ crcTables[0][high >> 24u];
        bytes += 8u;
        size -= 8u;
    }
    while (size-- != 0u)
        crc = (crc >> 8u) ^ crcTables[0][(crc ^ *bytes++) & 0xffu];
    return crc;
}

#if defined(LAIUE_PERSIST_CRC_X86)
#if !defined(_MSC_VER) || defined(__clang__)
__attribute__((target("sse4.2")))
#endif
static uint32_t HardwareUpdate(uint32_t crc, const uint8_t *bytes, uint64_t size)
{
    uint64_t wide = crc;
    while (size != 0u && ((uintptr_t)bytes & 7u) != 0u)
    {
        wide = _mm_crc32_u8((uint32_t)wide, *bytes++);
        --size;
    }
    while (size >= 8u)
    {
        uint64_t value;
        memcpy(&value, bytes, sizeof(value));
        wide = _mm_crc32_u64(wide, value);
        bytes += 8u;
        size -= 8u;
    }
    while (size-- != 0u)
        wide = _mm_crc32_u8((uint32_t)wide, *bytes++);
    return (uint32_t)wide;
}
#endif

uint32_t LaiuePersistCrc32c(uint32_t crc, const void *bytes, uint64_t size)
{
    if (bytes == NULL || size == 0u)
        return crc;
    EnsureTables();
    crc = ~crc;
#if defined(LAIUE_PERSIST_CRC_X86)
    if (crcHardware)
        return ~HardwareUpdate(crc, (const uint8_t *)bytes, size);
#endif
    return ~SoftwareUpdate(crc, (const uint8_t *)bytes, size);
}
