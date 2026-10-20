#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Блочное LZ-сжатие сохранений. Блок самодостаточен, не больше
 * LAIUE_PERSIST_LZ_BLOCK_SIZE исходных байт.
 *
 * Последовательность: байт-токен, старшие 4 бита — число литералов,
 * младшие — длина совпадения минус 4. Значение 15 продолжается байтами,
 * которые прибавляются, пока байт равен 255. Затем литералы, затем
 * смещение совпадения u16 little-endian (1–65535) и продолжение длины.
 * Последняя последовательность блока содержит только литералы: её
 * токен-младшая часть равна нулю, и после литералов блок заканчивается. */

#define LAIUE_PERSIST_LZ_BLOCK_SIZE UINT32_C(262144) /* 256 КиБ */
/* Худший случай сжатого блока: несжимаемые данные плюс токены длины. */
#define LAIUE_PERSIST_LZ_BOUND(size) ((size) + (size) / 255u + 16u)
/* Рабочая память кодировщика на один поток. */
#define LAIUE_PERSIST_LZ_SCRATCH_BYTES (sizeof(uint32_t) * 65536u)

/* Возвращает размер сжатого блока в output (ёмкость — BOUND(inputSize))
 * или 0, если сжатие не уменьшило блок: тогда блок пишется как есть. */
uint32_t LaiuePersistLzCompress(const uint8_t *input, uint32_t inputSize, uint8_t *output,
                                uint32_t outputCapacity, uint32_t *scratch);

/* Распаковывает ровно outputSize байт. Любое противоречие во входе —
 * выход за границы, смещение до начала, лишние байты — даёт false. */
bool LaiuePersistLzDecompress(const uint8_t *input, uint32_t inputSize, uint8_t *output,
                              uint32_t outputSize);
