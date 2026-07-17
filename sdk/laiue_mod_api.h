/*
 * laiue mod API — единственный файл, который нужен для написания
 * нативного DLL-мода. Исходники игры не требуются: мод не линкуется
 * с движком, все функции приходят таблицей указателей при инициализации.
 *
 * Контракт мода (x64, соглашение вызова по умолчанию):
 *
 *   __declspec(dllexport) int32_t LaiueModInit(const LaiueModApi* api);
 *       Вызывается один раз при включении мода (главный поток, мир уже
 *       создан — функции api можно звать прямо из Init). Верните 0 при
 *       успехе — любое другое значение отменяет загрузку (причина
 *       попадёт в mods/mod_log.txt). Указатель api живёт до выгрузки
 *       мода; сохраните его. Изменение состава включённых модов
 *       перезагружает всю цепочку в порядке mods/enabled.txt.
 *
 *   __declspec(dllexport) void LaiueModShutdown(void);   // опционально
 *       Вызывается при выключении мода и при выходе из игры.
 *       Снимать свои колбеки не обязательно: хост делает это сам.
 *
 * Правила:
 *  - Все колбеки приходят на главном потоке; свои потоки создавать
 *    можно, но функции api разрешено звать только из колбеков.
 *  - Пока открыто меню паузы, кадровые колбеки не вызываются.
 *  - Совместимость: структура только растёт в хвост; проверяйте
 *    structSize перед доступом к полям новее вашей сборки SDK.
 *
 * Подробности, отладка и советы по производительности: docs/modding.md.
 */

#pragma once

/* Только компиляторные заголовки: SDK собирается даже без путей
   Windows SDK в INCLUDE (wchar_t приходит из stddef.h). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAIUE_MOD_API_VERSION 1u

/* Типы блоков версии API 1. */
#define LAIUE_BLOCK_AIR   0u
#define LAIUE_BLOCK_EARTH 1u
#define LAIUE_BLOCK_GRASS 2u

/* Режим игрока. */
#define LAIUE_GAME_MODE_FLY  0u
#define LAIUE_GAME_MODE_WALK 1u

typedef void (*LaiueFrameCallback)(void* user, float deltaSeconds);
typedef void (*LaiueFixedTickCallback)(void* user, float stepSeconds);
typedef void (*LaiueBlockEditCallback)(void* user,
    int64_t x, int64_t y, int64_t z,
    uint8_t previousBlock, uint8_t newBlock);

typedef struct LaiueModApi
{
    /* Размер этой структуры в байтах у хоста: поля за пределами
       structSize отсутствуют — не трогайте их. */
    uint32_t structSize;
    uint32_t apiVersion;        /* LAIUE_MOD_API_VERSION хоста */
    uint32_t gameVersionMajor;  /* версия игры, например 0.5 -> 0 и 5 */
    uint32_t gameVersionMinor;

    /* Непрозрачный контекст мода: первый аргумент каждой функции. */
    void* host;

    /* Журнал: отладчик (OutputDebugString) + файл mods/mod_log.txt. */
    void (*log)(void* host, const wchar_t* message);

    /* === Мир (координаты в блоках; Z — высота) === */
    uint8_t (*getBlock)(void* host, int64_t x, int64_t y, int64_t z);
    /* Ставит блок и перестраивает затронутые чанки. false — тип
       неизвестен этой версии игры. */
    bool (*setBlock)(void* host,
        int64_t x, int64_t y, int64_t z, uint8_t block);

    /* === Игрок и камера === */
    void (*getPlayerPosition)(void* host, double outPosition[3]);
    void (*setPlayerPosition)(void* host, const double position[3]);
    void (*getViewDirection)(void* host, float outDirection[3]);
    /* Толчок: x/y — горизонталь (блоков/с), z — вертикаль. */
    void (*applyImpulse)(void* host, float x, float y, float z);
    bool (*isPlayerGrounded)(void* host);
    uint32_t (*getGameMode)(void* host);

    /* === Время суток === */
    float (*getTimeHours)(void* host);           /* 0..24 */
    void (*setTimeHours)(void* host, float hours);

    /* === Хуки (по одному каждого вида на мод; NULL снимает) === */
    void (*setFrameCallback)(void* host,
        LaiueFrameCallback callback, void* user);
    /* Правки блоков игроком (ломание/установка). Изменения из
       setBlock самого мода событие не порождают. */
    void (*setBlockEditCallback)(void* host,
        LaiueBlockEditCallback callback, void* user);

    /* === Геймплей === */
    /* Дополнительные прыжки в воздухе: extraJumps 0..3, impulse —
       вертикальная скорость (блоков/с), refillOnGround — восстановление
       заряда при касании земли. При перезагрузке набора модов хост
       возвращает значения по умолчанию (0 прыжков). */
    void (*setAirJumps)(void* host,
        int32_t extraJumps, float impulse, bool refillOnGround);

    /* === Библиотечные моды (межмодовые интерфейсы) ===
     *
     * Мод-библиотека публикует именованную таблицу функций, потребитель
     * запрашивает её по имени. Имя — ASCII вида "автор.фича"; version
     * растёт при добавлениях, ломающие изменения — новое имя.
     * Указатель принадлежит библиотеке и живёт, пока она загружена;
     * хост перезагружает всю цепочку модов при любом изменении состава,
     * поэтому висячих указателей между модами не бывает.
     * Публиковать и запрашивать — из LaiueModInit; библиотека должна
     * стоять в mods/enabled.txt раньше потребителя.
     *
     * queryInterface возвращает NULL, если интерфейс не опубликован
     * или его версия меньше minimumVersion. Потребитель без библиотеки
     * возвращает из Init ненулевой код — хост выгрузит его и запишет
     * причину в журнал. */
    bool (*publishInterface)(void* host,
        const char* name, uint32_t version, void* interfacePointer);
    void* (*queryInterface)(void* host,
        const char* name, uint32_t minimumVersion);

    /* Фиксированный тик: постоянный шаг 1/60 с независимо от FPS —
       для геймплея, способностей и таймеров (кадровый хук оставьте
       визуалу). За длинный кадр приходит несколько тиков подряд,
       но не больше пяти; пока открыто меню паузы, тиков нет. */
    void (*setFixedTickCallback)(void* host,
        LaiueFixedTickCallback callback, void* user);

    /* === Данные мода в сохранении мира ===
       Блоб на мод в saves/<мир>/moddata/<имя>.bin. Читайте в
       LaiueModInit, пишите при изменениях или в LaiueModShutdown
       (он вызывается до записи сохранения при выходе).
       readModData возвращает фактический размер (0 — данных нет
       или буфер мал). */
    bool (*writeModData)(void* host, const void* bytes, uint32_t size);
    uint32_t (*readModData)(void* host, void* buffer, uint32_t capacity);
} LaiueModApi;

typedef int32_t (*LaiueModInitFunction)(const LaiueModApi* api);
typedef void (*LaiueModShutdownFunction)(void);
