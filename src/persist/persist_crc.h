#pragma once

#include <stdint.h>

/* CRC-32C (Castagnoli, отражённый полином 0x82F63B78), как в iSCSI и ext4.
 * Проверочное значение стандарта: "123456789" даёт 0xE3069283.
 * Первый вызов передаёт crc = 0; последующие продолжают предыдущий итог. */
uint32_t LaiuePersistCrc32c(uint32_t crc, const void *bytes, uint64_t size);
