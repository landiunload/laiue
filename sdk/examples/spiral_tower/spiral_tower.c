/*
 * Spiral Tower — потребитель библиотечного мода Builder Lib.
 *
 * Демонстрирует межмодовые интерфейсы: заголовок builder_lib.h
 * распространяет автор библиотеки, таблица приходит через
 * queryInterface. Библиотека обязана стоять в mods/enabled.txt
 * выше этого мода — иначе Init честно откажется (код 1), и вкладка
 * модов покажет причину.
 *
 *     cl /nologo /W4 /O2 /utf-8 /LD /I..\.. /I..\builder_lib ^
 *        spiral_tower.c /Fe:spiral_tower.dll
 */

#include "../builder_lib/builder_lib.h"
#include "laiue_mod_api.h"

__declspec(dllexport) int32_t LaiueModInit(const LaiueModApi* api)
{
    const BuilderLibV1* builder = api->queryInterface(api->host,
        BUILDER_LIB_NAME, BUILDER_LIB_VERSION);
    if (builder == NULL)
    {
        api->log(api->host,
            L"нужен Builder Lib: включите builder_lib.lmp выше по списку");
        return 1;
    }

    /* Площадка и спираль прямо по курсу от точки появления. */
    builder->fillBox(-5, 13, 96, 5, 23, 97, LAIUE_BLOCK_EARTH);
    builder->buildHelix(0, 18, 98, 4, 26, LAIUE_BLOCK_GRASS);
    api->log(api->host, L"спиральная башня построена через laiue.builder");
    return 0;
}

__declspec(dllexport) void LaiueModShutdown(void)
{
}
