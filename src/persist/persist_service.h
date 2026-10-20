#pragma once

/* Таблица сервиса laiue.persist для приложений, которые загружают модули
 * по имени. Функции совпадают с одноимёнными из persist.h. */

#include "api.h"
#include "mod/module_api.h"
#include "persist/persist.h"

#include <stdint.h>

#define LAIUE_PERSIST_SERVICE_NAME "laiue.persist"
#define LAIUE_PERSIST_SERVICE_ABI_VERSION_1 1u

typedef struct LaiuePersistServiceV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    LaiuePersistStatus (*documentCreate)(const LaiuePersistDocumentDesc *desc,
                                         LaiuePersistDocument **outDocument);
    void (*documentDestroy)(LaiuePersistDocument *document);
    LaiuePersistStatus (*documentBeginSection)(LaiuePersistDocument *document,
                                               const LaiuePersistSectionDesc *desc,
                                               LaiuePersistSectionWriter **outWriter);
    LaiuePersistStatus (*documentAddBorrowed)(LaiuePersistDocument *document,
                                              const LaiuePersistSectionDesc *desc,
                                              const void *bytes, uint64_t size);
    LaiuePersistStatus (*documentAddProduced)(LaiuePersistDocument *document,
                                              const LaiuePersistSectionDesc *desc,
                                              LaiuePersistProducerFn producer, void *context);
    LaiuePersistStatus (*documentCopySection)(LaiuePersistDocument *document,
                                              LaiuePersistReader *reader, uint32_t sectionIndex);
    LaiuePersistStatus (*documentCopyRemaining)(LaiuePersistDocument *document,
                                                LaiuePersistReader *reader);
    bool (*sectionWrite)(LaiuePersistSectionWriter *writer, const void *bytes, uint64_t size);
    uint64_t (*sectionSize)(const LaiuePersistSectionWriter *writer);
    LaiuePersistStatus (*sectionEnd)(LaiuePersistSectionWriter *writer);
    LaiuePersistStatus (*save)(LaiuePersistDocument *document, const LaiuePersistSaveDesc *desc);
    LaiuePersistStatus (*saveStart)(LaiuePersistDocument *document,
                                    const LaiuePersistSaveDesc *desc, LaiuePersistJob **outJob);
    LaiuePersistStatus (*open)(const LaiuePersistOpenDesc *desc, LaiuePersistReader **outReader,
                               LaiuePersistOpenReport *outReport);
    void (*close)(LaiuePersistReader *reader);
    bool (*readerGetInfo)(const LaiuePersistReader *reader, LaiuePersistFileInfo *outInfo);
    bool (*readerGetSection)(const LaiuePersistReader *reader, uint32_t index,
                             LaiuePersistSectionInfo *outInfo);
    LaiuePersistStatus (*readerFind)(const LaiuePersistReader *reader, const char *name,
                                     uint32_t *outIndex);
    LaiuePersistStatus (*readerRead)(LaiuePersistReader *reader, uint32_t index, void *buffer,
                                     uint64_t capacity);
    LaiuePersistStatus (*readerReadUpgraded)(LaiuePersistReader *reader, uint32_t index,
                                             uint32_t targetVersion,
                                             const LaiuePersistMigration *migrations,
                                             uint32_t migrationCount, LaiuePersistBlob *outBlob);
    void (*blobRelease)(LaiuePersistBlob *blob);
    LaiuePersistStatus (*loadStart)(const LaiuePersistLoadDesc *desc, LaiuePersistJob **outJob);
    bool (*jobPoll)(LaiuePersistJob *job, LaiuePersistProgress *outProgress);
    void (*jobCancel)(LaiuePersistJob *job);
    LaiuePersistStatus (*jobWait)(LaiuePersistJob *job);
    void (*jobDestroy)(LaiuePersistJob *job);
    const char *(*statusName)(LaiuePersistStatus status);
    uintptr_t reserved[8];
} LaiuePersistServiceV1;

LAIUE_PERSIST_API const LaiuePersistServiceV1 *LaiuePersistGetStaticServiceV1(void);
LAIUE_PERSIST_API const LaiueModuleApiV1 *LaiuePersistGetStaticModuleApiV1(void);
