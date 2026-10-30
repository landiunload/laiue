#pragma once

/* Файл кеша конвейеров, общий для D3D12 и Vulkan.
 *
 * Драйвер сам проверяет свои данные, но испорченный или чужой кеш умеет
 * ронять отдельные драйверы. Поэтому данные драйвера лежат за собственным
 * заголовком: бэкенд, производитель, устройство, версия драйвера, UUID
 * кеша (Vulkan) и SHA-256 содержимого. Несовпадение любого поля означает
 * пустой кеш, а не попытку скормить драйверу чужие байты.
 *
 * Раскладка (little-endian, 96 байт заголовка, затем данные драйвера):
 *   0 "LPSOCACH"  8 u32 версия (1)  12 u32 бэкенд  16 u32 vendor
 *  20 u32 device  24 u64 версия драйвера  32 u8[16] UUID
 *  48 u64 размер данных  56 u8[32] SHA-256 данных  88 u64 резерв (0) */

#include "render/renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RENDERER_PIPELINE_CACHE_MAXIMUM_BYTES UINT32_C(268435456) /* 256 МиБ */

typedef struct RendererPipelineCacheIdentity
{
    uint32_t backend;
    uint32_t vendorId;
    uint32_t deviceId;
    uint64_t driverVersion;
    uint8_t uuid[16];
} RendererPipelineCacheIdentity;

/* Каталог задан приложением. */
bool RendererPipelineCacheEnabled(void);
/* Читает данные для identity. false — кеш выключен; иначе status->state
 * говорит, найден ли подходящий файл. *outPayload освобождается
 * PlatformFree, при отсутствии данных — NULL. */
bool RendererPipelineCacheLoad(const RendererPipelineCacheIdentity *identity, uint8_t **outPayload,
                               uint64_t *outSize, RendererPipelineCacheStatusV1 *status);
/* Атомарно заменяет файл; ошибка записи не считается ошибкой рендера. */
bool RendererPipelineCacheStore(const RendererPipelineCacheIdentity *identity, const void *payload,
                                uint64_t size);
/* FNV-1a 64: устойчивое имя конвейера из его описания и байт-кода. */
uint64_t RendererPipelineCacheHash(uint64_t hash, const void *bytes, size_t size);
#define RENDERER_PIPELINE_CACHE_HASH_SEED UINT64_C(0xcbf29ce484222325)
