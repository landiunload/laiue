#pragma once

#include "api.h"
#include "audio/audio.h"
#include "content/content_catalog.h"

#include <stdbool.h>
#include <stdint.h>

// Звукопаки: каталог `.lap` со звуками внутри. Заменить любой звук
// значит положить в свой пак файл с тем же именем — движок возьмёт его
// вместо исходного, ровно как шейдерпак подменяет отдельный шейдер.
//
// Имена звуков принадлежат приложению: движок не знает, что такое «шаг»
// или «удар», и просто отдаёт клип по имени. Никакого списка
// обязательных звуков в движке нет.
//
// Кроме своего `.la` пак принимает WAV и MP3. Прочитав такой файл,
// движок кладёт рядом с ним готовый `.la` и дальше берёт уже его: пока
// исходник не станет новее кэша, разбора не происходит вовсе. Кэш
// переживает исчезновение исходника — собранный звук остаётся, даже
// если WAV удалили.

#define AUDIO_PACK_NAME_MAX LAIUE_CONTENT_NAME_CAPACITY

typedef struct AudioPackEntry
{
    wchar_t name[AUDIO_PACK_NAME_MAX];
    bool active;
} AudioPackEntry;

typedef struct AudioPackList
{
    AudioPackEntry *entries;
    uint32_t count;
} AudioPackList;

typedef enum AudioPackLoadStatus
{
    AUDIO_PACK_LOAD_NOT_ATTEMPTED = 0,
    AUDIO_PACK_LOAD_OK,
    AUDIO_PACK_LOAD_NO_ACTIVE_PACK,
    AUDIO_PACK_LOAD_SOUND_NOT_FOUND,
    AUDIO_PACK_LOAD_INVALID_SOUND,
    AUDIO_PACK_LOAD_IO_ERROR,
    AUDIO_PACK_LOAD_OUT_OF_MEMORY,
} AudioPackLoadStatus;

// Перечисление и выбор пака. Активное имя хранится в sounds/active.txt,
// как у остальных категорий содержимого.
LAIUE_AUDIO_API bool AudioPackEnumerateFrom(LaiueContentCatalog *catalog, AudioPackList *outList);
LAIUE_AUDIO_API bool AudioPackActivateIn(LaiueContentCatalog *catalog, const wchar_t *name);
LAIUE_AUDIO_API void AudioPackListRelease(AudioPackList *list);

// Перечисляет звуки активного пака. Имя — имя файла без расширения.
LAIUE_AUDIO_API bool AudioPackEnumerateSoundsFrom(LaiueContentCatalog *catalog,
                                                  AudioPackList *outList);

// Загружает звук по имени из активного пака и создаёт готовый клип.
// Владение клипом переходит вызывающей стороне.
//
// Когда звука нет — нет пака, нет файла, файл не разобрался — выдача не
// пуста: возвращается тишина, а причина остаётся в outStatus. Так же
// ведёт себя текстурпак с материалом, которого в нём нет: он показывает
// нейтральный слой, а не отказывает. Игра не обязана проверять
// указатель после каждой загрузки, но обязана смотреть на статус, если
// хочет знать правду.
//
// NULL возвращается только на ошибку самой программы: небезопасное имя,
// отсутствующее устройство или нехватка памяти.
LAIUE_AUDIO_API AudioClip *AudioClipLoadFrom(AudioDevice *device, LaiueContentCatalog *catalog,
                                             const wchar_t *soundName,
                                             AudioPackLoadStatus *outStatus);

// Загружает звук из явного файла `.la`, минуя каталог содержимого:
// приложение вправе держать свои звуки где угодно.
LAIUE_AUDIO_API AudioClip *AudioClipLoadFile(AudioDevice *device, const wchar_t *path,
                                             AudioPackLoadStatus *outStatus);

// Разбирает `.la` прямо из памяти. Существует для приложений, которые
// доставляют содержимое своим способом (архив, сеть, app bundle).
LAIUE_AUDIO_API AudioClip *AudioClipLoadMemory(AudioDevice *device, const void *bytes,
                                               uint32_t sizeBytes, AudioPackLoadStatus *outStatus);

// Совместимые обёртки поверх каталога, укоренённого в каталоге программы.
LAIUE_AUDIO_API bool AudioPackEnumerate(AudioPackList *outList);
LAIUE_AUDIO_API bool AudioPackActivate(const wchar_t *name);
