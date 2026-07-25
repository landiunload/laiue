/*
 * Double Jump — второй прыжок в воздухе.
 *
 * Демонстрирует геймплейную функцию API: одного вызова в Init
 * достаточно, хост вернёт значения по умолчанию при выгрузке мода.
 *
 * Windows и Linux-команды сборки приведены в docs/modding.md.
 */

#include "laiue_mod_api.h"

LAIUE_MOD_EXPORT int32_t LAIUE_MOD_CALL LaiueModInit(const LaiueModApi *api)
{
    api->setAirJumps(api->host, 1, 7.4f, true);
    api->log(api->host, L"двойной прыжок включён: Space в полёте");
    return 0;
}

LAIUE_MOD_EXPORT void LAIUE_MOD_CALL LaiueModShutdown(void) {}
