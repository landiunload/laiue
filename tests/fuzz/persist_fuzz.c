// Фаззер чтения сохранений. Первый байт выбирает режим:
//
// чётный — остальные байты и есть файл сохранения: проверяются пролог,
//          эпилог, оглавление и границы, затем читаются все секции;
// нечётный — байты становятся хранимыми данными единственной LZ-секции
//          внутри корректного контейнера с верными контрольными суммами
//          оглавления и хранимых байтов. Так мутации не упираются в CRC и
//          доходят до разбора блоков и распаковщика LZ.
//
// Сохранение читается с диска, поэтому вход пишется во временный файл в
// текущем каталоге: libFuzzer гоняет входы в одном процессе по очереди.

#include "persist/persist.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_PATH "laiue_persist_fuzz.sav"
#define FUZZ_MAXIMUM_RAW (8u * 1024u * 1024u)

static uint32_t Crc32c(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0u; index < size; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (UINT32_C(0x82F63B78) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void Store32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void Store64(uint8_t *bytes, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

/* Пролог, данные, одна запись оглавления и эпилог по docs/persist.md. */
static uint8_t *BuildContainer(const uint8_t *stored, size_t storedSize, uint64_t rawSize,
                               size_t *outSize)
{
    const size_t size = 32u + storedSize + 128u + 40u;
    uint8_t *file = calloc(1u, size);
    if (file == NULL)
        return NULL;
    memcpy(file, "LAIUESAV", 8u);
    Store32(file + 8u, 1u);
    Store32(file + 12u, 32u);
    Store64(file + 16u, 1u);
    Store32(file + 24u, 1u);
    Store32(file + 28u, Crc32c(file, 28u));
    memcpy(file + 32u, stored, storedSize);
    uint8_t *entry = file + 32u + storedSize;
    memcpy(entry, "fuzz", 4u);
    Store64(entry + 80u, 32u);
    Store64(entry + 88u, storedSize);
    Store64(entry + 96u, rawSize);
    Store32(entry + 104u, 1u);
    Store32(entry + 108u, (uint32_t)LAIUE_PERSIST_CODEC_LZ << 8u);
    Store32(entry + 112u, Crc32c(stored, storedSize));
    Store32(entry + 116u, 0u);
    uint8_t *epilogue = entry + 128u;
    Store64(epilogue, 32u + storedSize);
    Store64(epilogue + 8u, 128u);
    Store64(epilogue + 16u, size);
    Store32(epilogue + 24u, 1u);
    Store32(epilogue + 28u, Crc32c(entry, 128u));
    Store32(epilogue + 32u, UINT32_C(0x4556534C));
    Store32(epilogue + 36u, Crc32c(epilogue, 36u));
    *outSize = size;
    return file;
}

static void ReadEverything(void)
{
    LaiuePersistOpenDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.structSize = sizeof(desc);
    desc.path = FUZZ_PATH;
    desc.verify = LAIUE_PERSIST_VERIFY_FULL;
    LaiuePersistReader *reader = NULL;
    if (LaiuePersistOpen(&desc, &reader, NULL) != LAIUE_PERSIST_OK)
        return;
    LaiuePersistFileInfo info;
    memset(&info, 0, sizeof(info));
    info.structSize = sizeof(info);
    (void)LaiuePersistReaderGetInfo(reader, &info);
    for (uint32_t index = 0u; index < info.sectionCount; ++index)
    {
        LaiuePersistSectionInfo section;
        memset(&section, 0, sizeof(section));
        section.structSize = sizeof(section);
        if (!LaiuePersistReaderGetSection(reader, index, &section) ||
            section.rawSize > FUZZ_MAXIMUM_RAW)
            continue;
        uint8_t *buffer = malloc(section.rawSize == 0u ? 1u : (size_t)section.rawSize);
        if (buffer == NULL)
            continue;
        (void)LaiuePersistReaderRead(reader, index, buffer, section.rawSize);
        free(buffer);
        uint32_t found = 0u;
        (void)LaiuePersistReaderFind(reader, section.name, &found);
    }
    LaiuePersistClose(reader);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1u || size > (4u << 20))
        return 0;
    const bool container = (data[0] & 1u) != 0u;
    uint8_t *file = NULL;
    size_t fileSize = 0u;
    if (container)
    {
        /* Следующие 3 байта задают исходный размер секции. */
        if (size < 4u)
            return 0;
        const uint64_t rawSize =
            ((uint64_t)data[1] | ((uint64_t)data[2] << 8u) | ((uint64_t)data[3] << 16u)) * 2u;
        file = BuildContainer(data + 4u, size - 4u, rawSize, &fileSize);
    }
    else
    {
        fileSize = size - 1u;
        file = malloc(fileSize == 0u ? 1u : fileSize);
        if (file != NULL)
            memcpy(file, data + 1u, fileSize);
    }
    if (file == NULL)
        return 0;
    FILE *output = fopen(FUZZ_PATH, "wb");
    if (output != NULL)
    {
        const bool written = fwrite(file, 1u, fileSize, output) == fileSize;
        const bool closed = fclose(output) == 0;
        if (written && closed)
            ReadEverything();
    }
    free(file);
    (void)remove(FUZZ_PATH);
    return 0;
}
