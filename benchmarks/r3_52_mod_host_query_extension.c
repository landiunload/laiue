/*
 * Harness fixture 19-mod-host: РјРёРЅРёРјР°Р»СЊРЅС‹Р№ РЅР°С‚РёРІРЅС‹Р№ РјРѕРґ, РєРѕС‚РѕСЂС‹Р№ РІ
 * LaiueModLoadV1 РґС‘СЂРіР°РµС‚ host->queryService РІ tight-loop. РќСѓР¶РµРЅ, РїРѕС‚РѕРјСѓ С‡С‚Рѕ
 * РїСѓР±Р»РёС‡РЅРѕРіРѕ API РґР»СЏ РїРѕРёСЃРєР° СЃРµСЂРІРёСЃР° СЃРЅР°СЂСѓР¶Рё РјРѕРґР° РЅРµС‚: ApiQueryService
 * reachable С‚РѕР»СЊРєРѕ С‡РµСЂРµР· queryService РёР· Р·Р°РіСЂСѓР¶РµРЅРЅРѕРіРѕ СЂР°СЃС€РёСЂРµРЅРёСЏ.
 *
 * РџРѕРІРµРґРµРЅРёРµ РІС‹Р±РёСЂР°РµС‚СЃСЏ РїРѕ host->modId, С‡С‚РѕР±С‹ РѕРґРёРЅ Рё С‚РѕС‚ Р¶Рµ DLL РѕР±СЃР»СѓР¶РёРІР°Р»
 * РѕР±Р° СЃС†РµРЅР°СЂРёСЏ СЃС‚РµРЅРґР°:
 *   - "bench.query": С‚СЏР¶С‘Р»С‹Р№ С†РёРєР» (hit РІ РєРѕРЅС†Рµ, hit РІ РЅР°С‡Р°Р»Рµ, miss),
 *     СЂРµР·СѓР»СЊС‚Р°С‚ РєР»Р°РґС‘С‚СЃСЏ РІ implementation СЃРµСЂРІРёСЃР° "bench.svc.0000";
 *   - Р»СЋР±РѕР№ РґСЂСѓРіРѕР№ id: СЂРѕРІРЅРѕ РѕРґРёРЅ queryService РґР»СЏ РїСЂРѕРІРµСЂРєРё СЃРµСЂРІРёСЃР°,
 *     Р±РµР· Р·Р°РїРёСЃРё СЂРµР·СѓР»СЊС‚Р°С‚Р° (Р»С‘РіРєРёР№ РјРѕРґ РґР»СЏ ordering/lifecycle РЅР°РіСЂСѓР·РѕРє).
 *
 * DLL С„РёРєСЃРёСЂРѕРІР°РЅ РґР»СЏ baseline Рё candidate: РјРµРЅСЏРµС‚СЃСЏ С‚РѕР»СЊРєРѕ laiue_mod.dll.
 */

#include "mod/mod_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef R2_QUERY_LOOP
#define R2_QUERY_LOOP 300000u
#endif

static bool R2AsciiEquals(const char *first, const char *second)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }
    uint32_t index = 0u;
    while (first[index] != '\0' && first[index] == second[index])
    {
        ++index;
    }
    return first[index] == second[index];
}

LAIUE_MOD_EXPORT LaiueModResult LAIUE_MOD_CALL LaiueModLoadV1(const LaiueModHostApiV1 *host,
                                                              LaiueModExportsV1 *exports)
{
    if (host == NULL || exports == NULL ||
        host->structSize < offsetof(LaiueModHostApiV1, reserved) ||
        host->abiVersion != LAIUE_MOD_ABI_VERSION_1 ||
        exports->structSize < offsetof(LaiueModExportsV1, reserved) ||
        exports->abiVersion != LAIUE_MOD_ABI_VERSION_1 || host->queryService == NULL)
    {
        return -1;
    }

    uint32_t version = 0u;
    uint32_t size = 0u;
    const void *anchor = host->queryService(host->hostContext, "bench.svc.0000", 1u, 1u, &version,
                                            &size);
    if (anchor == NULL || version != 1u || size < sizeof(uint32_t) * 2u)
    {
        return -17;
    }
    if (!R2AsciiEquals(host->modId, "bench.query"))
    {
        return LAIUE_MOD_RESULT_OK;
    }

    uint64_t checksum = 0u;
    for (uint32_t iteration = 0u; iteration < R2_QUERY_LOOP; ++iteration)
    {
        uint32_t hitVersion = 0u;
        uint32_t hitSize = 0u;
        const void *tail = host->queryService(host->hostContext, "bench.svc.0063", 1u, 1u,
                                              &hitVersion, &hitSize);
        checksum += tail != NULL ? 1u : 0u;
        checksum += hitVersion;
        checksum += hitSize;

        const void *head = host->queryService(host->hostContext, "bench.svc.0000", 1u, 1u,
                                              &hitVersion, &hitSize);
        checksum += head != NULL ? 1u : 0u;
        checksum += hitVersion;
        checksum += hitSize;

        const void *absent = host->queryService(host->hostContext, "bench.svc.absent", 1u, 1u,
                                                &hitVersion, &hitSize);
        checksum += absent != NULL ? 1u : 0u;
    }

    volatile uint32_t *out = (volatile uint32_t *)anchor;
    out[0] = (uint32_t)(checksum & 0xffffffffu);
    out[1] = (uint32_t)(checksum >> 32);
    return LAIUE_MOD_RESULT_OK;
}
