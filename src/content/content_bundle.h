#pragma once

#include "api.h"
#include "content/content_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define LAIUE_CONTENT_BUNDLE_HASH_SIZE 32U
#define LAIUE_CONTENT_BUNDLE_MAX_BYTES (256ULL * 1024ULL * 1024ULL)
#define LAIUE_CONTENT_BUNDLE_MAX_SOURCES 128U

typedef struct LaiueContentBundleSource
{
    LaiueContentType type;
    wchar_t name[128];
} LaiueContentBundleSource;

typedef struct LaiueContentBundle
{
    uint8_t* bytes;
    uint64_t size;
    uint8_t sha256[LAIUE_CONTENT_BUNDLE_HASH_SIZE];
} LaiueContentBundle;

// Собирает переносимый пакет только из явно перечисленных корневых паков.
// Reparse points игнорируются, объём и число файлов жёстко ограничены.
LAIUE_CONTENT_API bool LaiueContentBundleBuild(
    const LaiueContentBundleSource* sources, uint32_t sourceCount,
    LaiueContentBundle* output);
LAIUE_CONTENT_API void LaiueContentBundleRelease(LaiueContentBundle* bundle);

// Полностью валидирует пакет до записи. Распаковка идёт в *.download,
// прежняя версия сохраняется как *.previous, затем выполняется rename.
LAIUE_CONTENT_API bool LaiueContentBundleInstall(
    const uint8_t* bytes, uint64_t size);
