#pragma once

#include "mod/mod_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define LAIUE_MOD_MANIFEST_FORMAT_VERSION 3u
#define LAIUE_MOD_MANIFEST_FILE_NAME L"mod.lm"
#define LAIUE_MOD_PACK_EXTENSION L".lmp"
#define LAIUE_MOD_MANIFEST_MAX_BYTES (64u * 1024u)
#define LAIUE_MOD_NATIVE_MAX_BYTES (256u * 1024u * 1024u)
#define LAIUE_MOD_PACK_ENUMERATION_LIMIT 4096u

#define LAIUE_MOD_ID_CAPACITY 64u
#define LAIUE_MOD_DISPLAY_NAME_CAPACITY 128u
#define LAIUE_MOD_VERSION_CAPACITY 32u
#define LAIUE_MOD_NATIVE_NAME_CAPACITY 128u

typedef struct LaiueModManifest
{
    uint32_t structSize;
    uint32_t formatVersion;

    char id[LAIUE_MOD_ID_CAPACITY];
    char displayName[LAIUE_MOD_DISPLAY_NAME_CAPACITY];
    char version[LAIUE_MOD_VERSION_CAPACITY];

    uint32_t requiredEngineMajor;
    uint32_t requiredEngineMinor;
    uint32_t requiredAbi;

    wchar_t entryWindowsX86_64[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryWindowsArm64[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryLinuxX86_64Gnu[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryLinuxX86_64Musl[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryLinuxArm64Gnu[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryLinuxArm64Musl[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryMacosX86_64[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    wchar_t entryMacosArm64[LAIUE_MOD_NATIVE_NAME_CAPACITY];
} LaiueModManifest;

typedef struct LaiueModPackInfo
{
    wchar_t packName[LAIUE_MOD_NATIVE_NAME_CAPACITY];
    LaiueModManifest manifest;
    LaiueModStatus status;
    LaiueModDiagnostic diagnostic;
} LaiueModPackInfo;

typedef struct LaiueModPackList
{
    LaiueModPackInfo *entries;
    uint32_t count;
} LaiueModPackList;

/* A pack name is one safe leaf such as "author_feature.lmp". */
LAIUE_MOD_API bool LaiueModPackNameIsSafe(const wchar_t *packName);

/*
 * Parses strict UTF-8 `LAIUE MOD 3` data. Unknown keys and sections are
 * ignored for forward-compatible metadata, while duplicate known fields and
 * malformed required fields are rejected.
 */
LAIUE_MOD_API LaiueModStatus LaiueModManifestParse(const void *bytes, size_t byteCount,
                                                   LaiueModManifest *outManifest,
                                                   LaiueModDiagnostic *diagnostic);

/* Selects the current supported platform artifact declared by the manifest. */
LAIUE_MOD_API LaiueModStatus LaiueModManifestSelectNativeEntry(const LaiueModManifest *manifest,
                                                               wchar_t *destination,
                                                               uint32_t capacity,
                                                               LaiueModDiagnostic *diagnostic);

/*
 * Inspects one pack without executing code. The root directory is an explicit,
 * application-owned trust boundary; packName and every child artifact remain
 * validated leaf names beneath it. The pack name must exactly match the
 * directory entry, and an ASCII case-collision rejects the complete root.
 */
LAIUE_MOD_API LaiueModStatus LaiueModPackInspect(const wchar_t *rootDirectory,
                                                 const wchar_t *packName, LaiueModPackInfo *outInfo,
                                                 LaiueModDiagnostic *diagnostic);

/*
 * Discovers safe *.lmp entries and sorts them by ordinal UTF-8 pack name, so
 * the result is stable across Windows and Linux. ASCII case-colliding names
 * reject the ambiguous root. Invalid packs are otherwise returned with a
 * per-entry diagnostic and are never loaded implicitly.
 */
LAIUE_MOD_API LaiueModStatus LaiueModPackEnumerate(const wchar_t *rootDirectory,
                                                   LaiueModPackList *outList,
                                                   LaiueModDiagnostic *diagnostic);
LAIUE_MOD_API void LaiueModPackListRelease(LaiueModPackList *list);
