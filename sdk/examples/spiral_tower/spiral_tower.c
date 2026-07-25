/*
 * Spiral Tower — потребитель библиотечного мода Builder Lib.
 *
 * Демонстрирует межмодовые интерфейсы: заголовок builder_lib.h
 * распространяет автор библиотеки, таблица приходит через
 * queryInterface. Библиотека обязана стоять в mods/enabled.txt
 * выше этого мода — иначе Init честно откажется (код 1), и вкладка
 * модов покажет причину.
 *
 * Windows и Linux-команды сборки приведены в docs/modding.md.
 */

#include "../builder_lib/builder_lib.h"
#include "laiue_mod_api.h"

LAIUE_MOD_EXPORT int32_t LAIUE_MOD_CALL LaiueModInit(const LaiueModApi *api)
{
    const BuilderLibV1 *builder =
        api->queryInterface(api->host, BUILDER_LIB_NAME, BUILDER_LIB_VERSION);
    if (builder == NULL)
    {
        api->log(api->host, L"нужен Builder Lib: включите builder_lib.lmp выше по списку");
        return 1;
    }

    /* Площадка и спираль прямо по курсу от точки появления. */
    builder->fillBox(-5, 13, 96, 5, 23, 97, LAIUE_BLOCK_EARTH);
    builder->buildHelix(0, 18, 98, 4, 26, LAIUE_BLOCK_GRASS);
    api->log(api->host, L"спиральная башня построена через laiue.builder");
    return 0;
}

LAIUE_MOD_EXPORT void LAIUE_MOD_CALL LaiueModShutdown(void) {}
