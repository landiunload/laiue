/*
 * Double Jump — второй прыжок в воздухе.
 *
 * Демонстрирует геймплейную функцию API: одного вызова в Init
 * достаточно, хост вернёт значения по умолчанию при выгрузке мода.
 *
 *     cl /nologo /W4 /O2 /utf-8 /LD /I..\.. double_jump.c /Fe:double_jump.dll
 */

#include "laiue_mod_api.h"

__declspec(dllexport) int32_t LaiueModInit(const LaiueModApi* api)
{
    api->setAirJumps(api->host, 1, 7.4f, true);
    api->log(api->host, L"двойной прыжок включён: Space в полёте");
    return 0;
}

__declspec(dllexport) void LaiueModShutdown(void)
{
}
