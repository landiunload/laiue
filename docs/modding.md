# Нативные моды

Модуль `laiue::mod` предоставляет минимальный host для расширений, но не
определяет игровую систему модов. Приложение владеет списком включённых
паков, их порядком, пользовательским интерфейсом, настройками и всеми
доступными модам сервисами. Сетевого или серверного слоя этот API не создаёт.

Нативный мод выполняется в процессе приложения с теми же правами. Проверка
путей защищает от случайного traversal, но не является sandbox. Загружать
следует только доверенные или отдельно проверенные бинарники.

## Структура пака

Один мод — каталог `<mods-root>/<name>.lmp`:

```text
example_weather.lmp/
    mod.lm
    example_weather.dll
    libexample_weather_glibc.so
    libexample_weather_musl.so
```

`mod.lm` — строгий UTF-8-манифест размером не более 64 КиБ:

```text
LAIUE MOD 3
id = example.weather
name = Example Weather
version = 1.2.0
engine = 0.7

[native]
abi = 1
entry_windows_x86_64 = example_weather.dll
entry_linux_x86_64_gnu = libexample_weather_glibc.so
entry_linux_x86_64_musl = libexample_weather_musl.so
```

`id` — стабильный lowercase ASCII identifier. `name` — необязательное
отображаемое UTF-8-имя. `engine` задаёт требуемые major/minor; host принимает
тот же major и не меньший minor. Для текущего процесса выбирается ровно один
из x86_64 artifacts. glibc и musl не взаимозаменяемы.

Имена пака, манифеста и native artifact должны быть безопасными leaf names.
ASCII case-collision вроде `Weather.lmp`/`weather.lmp` делает весь root
неоднозначным и отклоняется одинаково на всех платформах. Прямые
`Inspect`/`Load` требуют точного написания имени, возвращённого discovery.
Каталоги-паки, `mod.lm` и бинарники через symlink/reparse point отклоняются.
Размер native artifact ограничен 256 МиБ. Discovery возвращает остальные
невалидные паки с отдельной диагностикой, но никогда не выполняет их
автоматически.

## ABI 1

Мод включает только `mod/mod_api.h` и экспортирует единственную точку входа:

```c
#include <laiue/mod/mod_api.h>

static void LAIUE_MOD_CALL Unload(void* context)
{
    (void)context;
}

LAIUE_MOD_EXPORT LaiueModResult LAIUE_MOD_CALL
LaiueModLoadV1(const LaiueModHostApiV1* host,
    LaiueModExportsV1* exports)
{
    if (host == NULL || exports == NULL
        || host->abiVersion != LAIUE_MOD_ABI_VERSION_1)
        return 1;

    host->log(host->hostContext, LAIUE_MOD_LOG_INFORMATION, "loaded");
    exports->modContext = NULL;
    exports->unload = Unload;
    return LAIUE_MOD_RESULT_OK;
}
```

Мод не линкуется с DLL/SO движка. Он получает возможности через
`queryService`: имя, минимальную версию и минимальный размер интерфейса.
Конкретный service header публикует приложение или отдельный SDK. Возвращённый
указатель заимствован и действителен до завершения `unload`.

Такое разделение сохраняет направление зависимостей: движок не знает об
игровых интерфейсах, а мод зависит от узкого контракта, а не от глобального
состояния приложения. Новые поля service table добавляются в конец; версия и
`structSize` позволяют старым потребителям запросить известный префикс.

## Host приложения

```c
LaiueModHostConfig config;
LaiueModHostConfigInitialize(&config, modsRoot);
config.log = MyModLog;

LaiueModDiagnostic diagnostic;
LaiueModHost* host = LaiueModHostCreate(&config, &diagnostic);

LaiueModService worldService = {
    .name = "mygame.world",
    .version = 1,
    .implementation = &worldApi,
    .implementationSize = sizeof(worldApi),
};
LaiueModHostRegisterService(host, &worldService, &diagnostic);

const wchar_t* enabled[] = {
    L"example_weather.lmp",
    L"example_ui.lmp",
};
LaiueModHostLoadMany(host, enabled, 2, NULL, &diagnostic);
```

`LoadMany` сохраняет точный порядок приложения. Если один мод не загрузился,
моды этого вызова выгружаются в обратном порядке, а ранее активные остаются.
`UnloadAll` также идёт в обратном порядке успешной загрузки.

Registry сервисов заморожен, пока загружается, выгружается или остаётся
активным хотя бы один мод. Это делает service pointers стабильными. Операции
жизненного цикла host сериализует вызывающее приложение. Перед `Destroy`
нужно остановить внешние вызовы host; каждый мод обязан в `unload` остановить
и присоединить свои worker threads.

## Границы версии 0.7

- ABI поддерживает Windows x86_64, Linux x86_64 glibc и Linux x86_64 musl.
- Моды могут использовать только сервисы, явно опубликованные приложением.
- Движок не предоставляет игровых, сетевых или серверных сервисов.
- Hot reload кода намеренно не обещан: сначала выгружается старый мод со
  всеми его потоками и указателями, затем обычным путём загружается новый.
